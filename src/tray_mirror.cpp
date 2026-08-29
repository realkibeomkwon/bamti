#include "tray_mirror.hpp"

#include "log.hpp"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>

namespace bamti {
namespace {

constexpr int kTrayPriorityBase = 5;
constexpr ULONGLONG kPerfLogMs = 300000;
constexpr UINT kSlowEnumMs = 200;
constexpr int kSlowStreakStop = 3;
constexpr UINT kEventDebounceMs = 300;
constexpr UINT kEventMinIntervalMs = 1000;
constexpr UINT kSafetyIntervalMs = 5000;
constexpr UINT kFloodWindowMs = 10000;
constexpr long kFloodMaxEvents = 50;
constexpr wchar_t kClockClass[] = L"SystemTray.OmniButton";
constexpr wchar_t kShowDesktopClass[] = L"SystemTray.ShowDesktopButton";
constexpr wchar_t kOverflowButtonClass[] = L"SystemTray.NormalButton";

std::wstring Truncate(std::wstring text, size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars == 0) {
    return {};
  }
  text.resize(max_chars - 1);
  text.push_back(L'\u2026');
  return text;
}

UINT ClampInterval(ULONGLONG enum_ms) {
  uint64_t scaled = enum_ms * 100ull;
  if (enum_ms > 50) {
    scaled = 5000;
  }
  if (scaled < 1000) {
    return 1000;
  }
  if (scaled > 5000) {
    return 5000;
  }
  return static_cast<UINT>(scaled);
}

int OverflowOrder(const std::vector<TrayIconInfo>& icons) {
  // 오버플로 단추는 가장 왼쪽 SystemTrayIcon 중
  // ClassName이 NormalButton이면서 자식 Image가 없는 버튼이다.
  // 다른 시스템 아이콘은 AccentButton, OmniButton, OmniButtonCenter,
  // ShowDesktopButton이므로 이 조건에 걸리지 않는다.
  for (const TrayIconInfo& icon : icons) {
    if (icon.system_icon && icon.class_name == kOverflowButtonClass && !icon.has_image_child) {
      return icon.order;
    }
  }
  return -1;
}

}  // namespace

TrayMirror::TrayMirror() {
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  struct_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
}

TrayMirror::~TrayMirror() {
  Stop();
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  if (wake_event_ != nullptr) {
    CloseHandle(wake_event_);
    wake_event_ = nullptr;
  }
  if (struct_event_ != nullptr) {
    CloseHandle(struct_event_);
    struct_event_ = nullptr;
  }
}

const char* TrayMirror::Name() const {
  return "tray";
}

bool TrayMirror::Start(StatusSink* sink) {
  Stop();
  {
    std::lock_guard lock(mu_);
    sink_ = sink;
    settings_ = LoadWidgetSettings();
    stopped_slow_ = false;
    reset_pending_ = true;
    key_round_n_ = 0;
    use_runtime_id_ = true;
    items_.clear();
    if (!settings_.tray_mirror) {
      Log(L"tray", L"disabled");
      return true;
    }
  }
  StartIntercept();
  std::lock_guard lock(mu_);
  StartWorkerLocked();
  return true;
}

void TrayMirror::Stop() {
  StopWorker();
  StopIntercept();
  DropAll();
  std::lock_guard lock(mu_);
  sink_ = nullptr;
  items_.clear();
}

void TrayMirror::StartWorkerLocked() {
  if (worker_.joinable()) {
    if (wake_event_ != nullptr) {
      SetEvent(wake_event_);
    }
    return;
  }
  if (stop_event_ == nullptr || wake_event_ == nullptr) {
    Log(L"tray", L"worker events missing");
    return;
  }
  ResetEvent(stop_event_);
  worker_ = std::thread([this] { WorkerLoop(); });
}

void TrayMirror::StopWorker() {
  if (stop_event_ != nullptr) {
    SetEvent(stop_event_);
  }
  std::thread worker;
  {
    std::lock_guard lock(mu_);
    if (worker_.joinable()) {
      worker = std::move(worker_);
    }
  }
  if (worker.joinable()) {
    worker.join();
  }
}

WidgetSettings TrayMirror::settings() const {
  std::lock_guard lock(mu_);
  return settings_;
}

void TrayMirror::StartIntercept() {
  StopIntercept();
  intercept_ = MakeInterceptTrayBackend();
  if (intercept_ == nullptr || !intercept_->Probe()) {
    Log(L"tray", L"intercept spy failed; explorer receives icons directly");
    intercept_.reset();
  }
}

void TrayMirror::StopIntercept() {
  intercept_.reset();
}

void TrayMirror::SetSettings(const WidgetSettings& next) {
  bool start_worker = false;
  bool stop_worker = false;
  std::vector<std::string> drop;
  StatusSink* sink = nullptr;
  {
    std::lock_guard lock(mu_);
    const bool was = settings_.tray_mirror;
    settings_ = next;
    sink = sink_;
    if (next.tray_mirror && !was && sink_ != nullptr && !stopped_slow_) {
      start_worker = true;
      reset_pending_ = true;
    } else if (!next.tray_mirror && was) {
      stop_worker = true;
    } else if (wake_event_ != nullptr) {
      SetEvent(wake_event_);
    }
    if (!stop_worker) {
      for (auto it = items_.begin(); it != items_.end();) {
        if (KeyHidden(it->first, next.tray_hidden_keys)) {
          drop.push_back(it->second.id);
          it = items_.erase(it);
        } else {
          ++it;
        }
      }
    }
  }
  if (sink != nullptr) {
    for (const std::string& id : drop) {
      sink->Remove(id);
    }
  }
  if (stop_worker) {
    StopWorker();
    StopIntercept();
    DropAll();
  }
  if (start_worker) {
    StartIntercept();
    std::lock_guard lock(mu_);
    StartWorkerLocked();
  }
}

void TrayMirror::OnExplorerRestart() {
  std::lock_guard lock(mu_);
  reset_pending_ = true;
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void TrayMirror::SetActive(bool active) {
  {
    std::lock_guard lock(mu_);
    if (active_ == active) {
      return;
    }
    active_ = active;
    if (active) {
      reset_pending_ = true;
    }
  }
  Log(L"tray", L"active=%d", active ? 1 : 0);
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void TrayMirror::DropAll() {
  std::vector<std::string> ids;
  StatusSink* sink = nullptr;
  {
    std::lock_guard lock(mu_);
    sink = sink_;
    ids.reserve(items_.size());
    for (const auto& pair : items_) {
      ids.push_back(pair.second.id);
    }
    items_.clear();
  }
  if (sink != nullptr) {
    for (const std::string& id : ids) {
      sink->Remove(id);
    }
  }
}

std::wstring TrayMirror::FirstGlyph(const std::wstring& tip) {
  if (tip.empty()) {
    return L"?";
  }
  const wchar_t c0 = tip[0];
  if (c0 >= 0xD800 && c0 <= 0xDBFF && tip.size() >= 2) {
    const wchar_t c1 = tip[1];
    if (c1 >= 0xDC00 && c1 <= 0xDFFF) {
      return tip.substr(0, 2);
    }
  }
  return std::wstring(1, c0);
}

std::string TrayMirror::MakeId(uint64_t key) {
  char buf[40]{};
  sprintf_s(buf, "bamti.tray/%016llx", static_cast<unsigned long long>(key));
  return buf;
}

std::string TrayMirror::KeyText(uint64_t key) {
  char buf[24]{};
  sprintf_s(buf, "0x%016llx", static_cast<unsigned long long>(key));
  return buf;
}

uint64_t TrayMirror::ParseId(const std::string& id) {
  constexpr char kPrefix[] = "bamti.tray/";
  if (id.size() <= 11 || id.compare(0, 11, kPrefix) != 0) {
    return 0;
  }
  return static_cast<uint64_t>(strtoull(id.c_str() + 11, nullptr, 16));
}

bool TrayMirror::KeyHidden(uint64_t key, const std::vector<std::string>& hidden) {
  const std::string hex = KeyText(key);
  const std::string bare = hex.size() > 2 ? hex.substr(2) : hex;
  char alt[24]{};
  sprintf_s(alt, "%016llx", static_cast<unsigned long long>(key));
  for (const std::string& one : hidden) {
    if (one == hex || one == bare || one == alt) {
      return true;
    }
    if (one.size() >= 2 && (one[0] == '0' && (one[1] == 'x' || one[1] == 'X'))) {
      if (strtoull(one.c_str() + 2, nullptr, 16) == key) {
        return true;
      }
    }
  }
  return false;
}

void TrayMirror::OnEvent(const StatusEvent& ev) {
  if (ev.event != "click" || ev.button != "left") {
    return;
  }
  const uint64_t key = ParseId(ev.id);
  if (key == 0) {
    return;
  }
  {
    std::lock_guard lock(mu_);
    pending_invoke_ = key;
  }
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void TrayMirror::DrainInvoke(TrayBackend* backend) {
  uint64_t key = 0;
  {
    std::lock_guard lock(mu_);
    key = pending_invoke_;
    pending_invoke_ = 0;
  }
  if (key == 0 || backend == nullptr) {
    return;
  }
  TrayIconInfo icon;
  icon.key = key;
  if (backend->Invoke(icon)) {
    return;
  }
  HRESULT hr = E_FAIL;
  const char* pattern = "none";
  backend->LastInvokeError(&hr, &pattern);
  Log(L"tray", L"invoke fail pattern=%hs hr=0x%08X", pattern, static_cast<unsigned>(hr));
}

bool TrayMirror::Include(const TrayIconInfo& icon, int overflow_order, const WidgetSettings& settings) const {
  if (icon.class_name == kClockClass) {
    return false;
  }
  if (icon.class_name == kShowDesktopClass) {
    return false;
  }
  if (overflow_order >= 0 && icon.order == overflow_order) {
    return false;
  }
  if (KeyHidden(icon.key, settings.tray_hidden_keys)) {
    return false;
  }
  if (icon.from_overflow) {
    return settings.tray_overflow_icons && icon.automation_id == L"NotifyItemIcon";
  }
  if (icon.system_icon) {
    return settings.tray_system_icons;
  }
  return icon.automation_id == L"NotifyItemIcon";
}

void TrayMirror::Publish(const TrayIconInfo& icon, int order) {
  StatusItem item;
  item.id = MakeId(icon.key);
  item.source = "tray";
  if (!icon.png.empty()) {
    item.icon.kind = IconKind::kPng;
    item.icon.bytes = icon.png;
  } else {
    item.icon.kind = IconKind::kGlyph;
    item.icon.glyph = FirstGlyph(icon.tip);
    if (item.icon.glyph.size() > kStatusGlyphMaxChars) {
      item.icon.glyph.resize(kStatusGlyphMaxChars);
    }
  }
  item.icon.cache_key = HashStatusIcon(item.icon);
  item.tooltip = Truncate(icon.tip, kStatusPanelTextMaxChars);
  item.state = StatusState::kNormal;
  item.priority = kTrayPriorityBase - order;
  // 알림 영역 항목: explorer가 감춘 것이면 우리도 감춘다.
  // 오버플로 항목: 원래 화면 밖이므로 IsOffscreen은 판단 근거가 되지 못한다.
  item.visible = icon.from_overflow ? true : (icon.offscreen == FALSE);
  StatusSink* sink = nullptr;
  {
    std::lock_guard lock(mu_);
    sink = sink_;
    ItemState st;
    st.key = icon.key;
    st.tip = icon.tip;
    st.order = order;
    st.visible = item.visible;
    st.icon_hash = item.icon.cache_key;
    st.id = item.id;
    items_[icon.key] = std::move(st);
  }
  if (sink != nullptr) {
    sink->Upsert(std::move(item));
  }
}

void TrayMirror::DoRound(TrayBackend* backend, bool events_live) {
  if (backend == nullptr) {
    return;
  }
  const ULONGLONG t0 = GetTickCount64();
  std::vector<TrayIconInfo> raw;
  const bool ok = backend->Enumerate(&raw);
  const ULONGLONG elapsed = GetTickCount64() - t0;

  WidgetSettings settings;
  StatusSink* sink = nullptr;
  bool use_runtime = true;
  {
    std::lock_guard lock(mu_);
    last_enum_ms_ = elapsed;
    interval_ms_ = events_live ? kSafetyIntervalMs : ClampInterval(elapsed);
    settings = settings_;
    sink = sink_;
    use_runtime = use_runtime_id_;
    if (elapsed > kSlowEnumMs) {
      ++slow_streak_;
    } else {
      slow_streak_ = 0;
    }
  }
  if (!ok) {
    return;
  }
  if (slow_streak_ >= kSlowStreakStop) {
    Log(L"tray", L"auto-stop enum_ms=%llu over %u ms x%d", elapsed, kSlowEnumMs, kSlowStreakStop);
    DropAll();
    std::lock_guard lock(mu_);
    stopped_slow_ = true;
    return;
  }

  const int overflow = OverflowOrder(raw);
  std::vector<ItemState> next;
  std::vector<TrayIconInfo> keep;
  next.reserve(raw.size());
  keep.reserve(raw.size());
  for (const TrayIconInfo& icon : raw) {
    TrayIconInfo copy = icon;
    if (!use_runtime) {
      copy.key = 0;
    }
    if (copy.key == 0) {
      const std::string id = WideToUtf8Bytes(copy.automation_id);
      const std::string cls = WideToUtf8Bytes(copy.class_name);
      uint64_t hash = Fnv1a64(reinterpret_cast<const uint8_t*>(id.data()), id.size());
      hash = Fnv1a64(reinterpret_cast<const uint8_t*>(cls.data()), cls.size(), hash);
      const int32_t ord = copy.order;
      copy.key = Fnv1a64(reinterpret_cast<const uint8_t*>(&ord), sizeof(ord), hash);
    }
    if (!Include(copy, overflow, settings)) {
      continue;
    }
    ItemState st;
    st.key = copy.key;
    st.tip = copy.tip;
    st.order = copy.order;
    st.visible = copy.from_overflow ? true : (copy.offscreen == FALSE);
    st.icon_hash = copy.png.empty() ? 0 : Fnv1a64(copy.png.data(), copy.png.size());
    st.id = MakeId(copy.key);
    next.push_back(st);
    keep.push_back(std::move(copy));
  }

  std::unordered_map<uint64_t, ItemState> prev;
  {
    std::lock_guard lock(mu_);
    prev = items_;
  }

  bool same = prev.size() == next.size();
  if (same) {
    for (const ItemState& st : next) {
      const auto it = prev.find(st.key);
      if (it == prev.end() || it->second.tip != st.tip || it->second.order != st.order ||
          it->second.visible != st.visible || it->second.icon_hash != st.icon_hash) {
        same = false;
        break;
      }
    }
  }

  int added = 0;
  int removed = 0;
  if (!same) {
    for (const auto& pair : prev) {
      bool found = false;
      for (const ItemState& st : next) {
        if (st.key == pair.first) {
          found = true;
          break;
        }
      }
      if (!found) {
        ++removed;
        if (sink != nullptr) {
          sink->Remove(pair.second.id);
        }
        std::lock_guard lock(mu_);
        items_.erase(pair.first);
      }
    }
    for (const TrayIconInfo& icon : keep) {
      const auto it = prev.find(icon.key);
      const bool vis = icon.from_overflow ? true : (icon.offscreen == FALSE);
      const uint64_t icon_hash = icon.png.empty() ? 0 : Fnv1a64(icon.png.data(), icon.png.size());
      if (it == prev.end() || it->second.tip != icon.tip || it->second.order != icon.order ||
          it->second.visible != vis || it->second.icon_hash != icon_hash) {
        if (it == prev.end()) {
          ++added;
        }
        Publish(icon, icon.order);
      }
    }
    if (added != 0 || removed != 0) {
      Log(L"tray", L"items +%d -%d now=%zu", added, removed, keep.size());
    } else if (keep.empty()) {
      Log(L"tray", L"enum raw=%zu keep=0 system=%d", raw.size(), settings.tray_system_icons ? 1 : 0);
    }
  }

  std::vector<uint64_t> keys;
  keys.reserve(next.size());
  for (const ItemState& st : next) {
    keys.push_back(st.key);
  }
  std::sort(keys.begin(), keys.end());
  bool log_stable = false;
  bool log_deferred = false;
  size_t common_n = 0;
  int stable = 0;
  unsigned churn = 0;
  {
    std::lock_guard lock(mu_);
    if (key_round_n_ < 5) {
      key_rounds_[key_round_n_] = keys;
      ++key_round_n_;
      if (key_round_n_ == 5) {
        std::vector<uint64_t> common = key_rounds_[0];
        std::vector<uint64_t> all = key_rounds_[0];
        for (int i = 1; i < 5; ++i) {
          std::vector<uint64_t> next_common;
          std::set_intersection(common.begin(), common.end(), key_rounds_[i].begin(), key_rounds_[i].end(),
                                std::back_inserter(next_common));
          common.swap(next_common);
          all.insert(all.end(), key_rounds_[i].begin(), key_rounds_[i].end());
        }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        if (common.size() < 2) {
          key_round_n_ = 0;
          log_deferred = true;
          common_n = common.size();
        } else {
          log_stable = true;
          stable = 1;
          churn = static_cast<unsigned>(all.size() - common.size());
        }
      }
    }
    const ULONGLONG now = GetTickCount64();
    if (last_perf_log_ == 0 || now - last_perf_log_ >= kPerfLogMs) {
      last_perf_log_ = now;
      Log(L"tray", L"enum_ms=%llu interval_ms=%u items=%zu", last_enum_ms_, interval_ms_, keep.size());
    }
  }
  if (log_deferred) {
    Log(L"tray", L"key stable deferred common=%zu", common_n);
  }
  if (log_stable) {
    Log(L"tray", L"key stable=%d churn=%u", stable, churn);
    if (stable == 0) {
      Log(L"tray", L"runtime id churn; falling back to automation_id+class+order");
    }
  }
}

void TrayMirror::WorkerLoop() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  std::unique_ptr<TrayBackend> owned;
  TrayBackend* backend = intercept_.get();
  if (backend == nullptr) {
    owned = MakeUiaTrayBackend();
    backend = owned.get();
  } else {
    backend->SetChangeSink([this] {
      if (wake_event_ != nullptr) {
        SetEvent(wake_event_);
      }
    });
  }
  const bool probed = backend != nullptr && backend->Probe();
  const bool intercept = intercept_ != nullptr && backend == intercept_.get();
  bool events_abandoned = false;
  bool events_live = false;
  if (backend != nullptr && !events_abandoned && !intercept) {
    events_live = backend->SubscribeStructureChanged(struct_event_);
  }
  if (intercept) {
    events_live = true;
  }
  Log(L"tray",
      L"backend=%hs capture=no hide_mode=hidden right_click=bamti_menu overflow=mirrored "
      L"structure_changed=%d probe=%d",
      backend != nullptr ? backend->Name() : "none", events_live ? 1 : 0, probed ? 1 : 0);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  bool run_enum = true;
  bool struct_pending = false;
  ULONGLONG last_enum = 0;
  ULONGLONG last_struct = 0;
  ULONGLONG flood_t0 = 0;
  long flood_n = 0;

  for (;;) {
    if (stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
      break;
    }

    bool reset = false;
    bool active = false;
    bool slow_stop = false;
    bool have_invoke = false;
    UINT interval = 1000;
    {
      std::lock_guard lock(mu_);
      reset = reset_pending_;
      reset_pending_ = false;
      active = active_;
      slow_stop = stopped_slow_;
      interval = interval_ms_;
      have_invoke = pending_invoke_ != 0;
    }
    if (slow_stop) {
      break;
    }
    if (reset && backend != nullptr) {
      backend->Reset();
      if (intercept) {
        events_live = true;
      } else {
        events_live = false;
        if (!events_abandoned) {
          events_live = backend->SubscribeStructureChanged(struct_event_);
        }
      }
    }
    if (have_invoke) {
      DrainInvoke(backend);
    }
    if (active && backend != nullptr && run_enum) {
      DoRound(backend, events_live);
      last_enum = GetTickCount64();
      run_enum = false;
      struct_pending = false;
      std::lock_guard lock(mu_);
      interval = interval_ms_;
      slow_stop = stopped_slow_;
    }
    if (slow_stop) {
      break;
    }

    HANDLE waits[4]{};
    DWORD n = 0;
    waits[n++] = stop_event_;
    waits[n++] = wake_event_;
    waits[n++] = struct_event_;
    if (active && timer != nullptr) {
      LARGE_INTEGER rel{};
      const ULONGLONG delay = interval == 0 ? 1 : interval;
      rel.QuadPart = -static_cast<LONGLONG>(delay * 10000ull);
      if (SetWaitableTimerEx(timer, &rel, 0, nullptr, nullptr, nullptr, 200)) {
        waits[n++] = timer;
      }
    }

    uint32_t flush_ms = 0xFFFFFFFFu;
    StatusSink* sink = nullptr;
    {
      std::lock_guard lock(mu_);
      sink = sink_;
      if (sink_ != nullptr) {
        flush_ms = sink_->NotifyWaitTimeoutMs();
      }
    }
    DWORD timeout = active ? flush_ms : INFINITE;
    if (struct_pending && active) {
      const ULONGLONG now = GetTickCount64();
      ULONGLONG ready = last_struct + kEventDebounceMs;
      if (last_enum != 0) {
        const ULONGLONG min_at = last_enum + kEventMinIntervalMs;
        if (min_at > ready) {
          ready = min_at;
        }
      }
      const DWORD deb = ready > now ? static_cast<DWORD>(ready - now) : 0;
      if (deb < timeout) {
        timeout = deb;
      }
    }
    const DWORD wait = MsgWaitForMultipleObjectsEx(n, waits, timeout, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
    if (wait == WAIT_OBJECT_0 + n) {
      MSG msg;
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      continue;
    }
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait == WAIT_TIMEOUT) {
      if (struct_pending && active) {
        const ULONGLONG now = GetTickCount64();
        const bool debounced = now >= last_struct + kEventDebounceMs;
        const bool spaced = last_enum == 0 || now >= last_enum + kEventMinIntervalMs;
        if (debounced && spaced) {
          run_enum = true;
        }
      }
      if (sink != nullptr) {
        sink->Flush();
      }
      continue;
    }
    if (wait == WAIT_OBJECT_0 + 1) {
      run_enum = true;
      continue;
    }
    if (wait == WAIT_OBJECT_0 + 2) {
      const long nfire = backend != nullptr ? backend->TakeStructureChangedCount() : 0;
      if (active) {
        const ULONGLONG now = GetTickCount64();
        if (flood_n == 0 || now - flood_t0 >= kFloodWindowMs) {
          flood_t0 = now;
          flood_n = 0;
        }
        flood_n += nfire > 0 ? nfire : 1;
        if (!events_abandoned && flood_n > kFloodMaxEvents) {
          events_abandoned = true;
          events_live = false;
          if (backend != nullptr) {
            backend->AbandonStructureChanged();
          }
          Log(L"tray", L"structure_changed flood count=%ld window_ms=%llu; polling only", flood_n,
              now - flood_t0);
        }
        last_struct = now;
        struct_pending = true;
      }
      continue;
    }
    if (n > 3 && wait == WAIT_OBJECT_0 + 3) {
      run_enum = true;
      continue;
    }
    if (wait == WAIT_FAILED) {
      Log(L"tray", L"wait failed err=%lu", GetLastError());
      Sleep(50);
    }
  }

  if (backend != nullptr) {
    backend->UnsubscribeStructureChanged();
  }
  owned.reset();
  if (timer != nullptr) {
    CloseHandle(timer);
  }
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
  Log(L"tray", L"worker stop");
}

}  // namespace bamti
