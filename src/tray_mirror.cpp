#include "tray_mirror.hpp"

#include "log.hpp"

#include <objbase.h>

#include <algorithm>
#include <cstdio>

namespace bamti {
namespace {

constexpr int kTrayPriorityBase = 5;
constexpr ULONGLONG kPerfLogMs = 300000;
constexpr UINT kSlowEnumMs = 200;
constexpr int kSlowStreakStop = 3;
constexpr wchar_t kClockClass[] = L"SystemTray.OmniButton";
constexpr wchar_t kShowDesktopClass[] = L"SystemTray.ShowDesktopButton";

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
  for (const TrayIconInfo& icon : icons) {
    if (icon.system_icon) {
      return icon.order;
    }
  }
  return -1;
}

}  // namespace

TrayMirror::TrayMirror() {
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
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
}

const char* TrayMirror::Name() const {
  return "tray";
}

bool TrayMirror::Start(StatusSink* sink) {
  Stop();
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
  StartWorkerLocked();
  return true;
}

void TrayMirror::Stop() {
  StopWorker();
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

void TrayMirror::SetSettings(const WidgetSettings& next) {
  bool start_worker = false;
  bool stop_worker = false;
  {
    std::lock_guard lock(mu_);
    const bool was = settings_.tray_mirror;
    settings_ = next;
    if (next.tray_mirror && !was && sink_ != nullptr && !stopped_slow_) {
      start_worker = true;
      reset_pending_ = true;
    } else if (!next.tray_mirror && was) {
      stop_worker = true;
    } else if (wake_event_ != nullptr) {
      SetEvent(wake_event_);
    }
    if (start_worker) {
      StartWorkerLocked();
    }
  }
  if (stop_worker) {
    StopWorker();
    DropAll();
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

bool TrayMirror::Include(const TrayIconInfo& icon, int overflow_order, bool system_icons) const {
  if (icon.class_name == kClockClass) {
    return false;
  }
  if (icon.class_name == kShowDesktopClass) {
    return false;
  }
  if (overflow_order >= 0 && icon.order == overflow_order) {
    return false;
  }
  if (icon.system_icon) {
    return system_icons;
  }
  return icon.automation_id == L"NotifyItemIcon";
}

void TrayMirror::Publish(const TrayIconInfo& icon, int order) {
  StatusItem item;
  item.id = MakeId(icon.key);
  item.source = "tray";
  item.icon.kind = IconKind::kGlyph;
  item.icon.glyph = FirstGlyph(icon.tip);
  if (item.icon.glyph.size() > kStatusGlyphMaxChars) {
    item.icon.glyph.resize(kStatusGlyphMaxChars);
  }
  item.icon.cache_key = HashStatusIcon(item.icon);
  item.tooltip = Truncate(icon.tip, kStatusPanelTextMaxChars);
  item.state = StatusState::kNormal;
  item.priority = kTrayPriorityBase - order;
  item.visible = icon.offscreen == FALSE;
  StatusSink* sink = nullptr;
  {
    std::lock_guard lock(mu_);
    sink = sink_;
    ItemState st;
    st.key = icon.key;
    st.tip = icon.tip;
    st.order = order;
    st.visible = item.visible;
    st.id = item.id;
    items_[icon.key] = std::move(st);
  }
  if (sink != nullptr) {
    sink->Upsert(std::move(item));
  }
}

void TrayMirror::DoRound(TrayBackend* backend) {
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
    interval_ms_ = ClampInterval(elapsed);
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
    if (!Include(icon, overflow, settings.tray_system_icons)) {
      continue;
    }
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
    ItemState st;
    st.key = copy.key;
    st.tip = copy.tip;
    st.order = copy.order;
    st.visible = copy.offscreen == FALSE;
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
          it->second.visible != st.visible) {
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
      const bool vis = icon.offscreen == FALSE;
      if (it == prev.end() || it->second.tip != icon.tip || it->second.order != icon.order ||
          it->second.visible != vis) {
        if (it == prev.end()) {
          ++added;
        }
        Publish(icon, icon.order);
      }
    }
    if (added != 0 || removed != 0) {
      Log(L"tray", L"items +%d -%d now=%zu", added, removed, keep.size());
    }
  }

  std::vector<uint64_t> keys;
  keys.reserve(next.size());
  for (const ItemState& st : next) {
    keys.push_back(st.key);
  }
  std::sort(keys.begin(), keys.end());
  bool log_stable = false;
  int stable = 0;
  unsigned churn = 0;
  {
    std::lock_guard lock(mu_);
    if (key_round_n_ < 5) {
      key_rounds_[key_round_n_] = keys;
      ++key_round_n_;
      if (key_round_n_ == 5) {
        log_stable = true;
        stable = 1;
        std::vector<uint64_t> all = key_rounds_[0];
        for (int i = 1; i < 5; ++i) {
          if (key_rounds_[i] != key_rounds_[0]) {
            stable = 0;
          }
          std::vector<uint64_t> merged = all;
          merged.insert(merged.end(), key_rounds_[i].begin(), key_rounds_[i].end());
          std::sort(merged.begin(), merged.end());
          merged.erase(std::unique(merged.begin(), merged.end()), merged.end());
          all.swap(merged);
        }
        size_t min_n = key_rounds_[0].size();
        for (int i = 1; i < 5; ++i) {
          min_n = (std::min)(min_n, key_rounds_[i].size());
        }
        churn = static_cast<unsigned>(all.size() - min_n);
        if (stable == 0) {
          use_runtime_id_ = false;
        }
      }
    }
    const ULONGLONG now = GetTickCount64();
    if (last_perf_log_ == 0 || now - last_perf_log_ >= kPerfLogMs) {
      last_perf_log_ = now;
      Log(L"tray", L"enum_ms=%llu interval_ms=%u items=%zu", last_enum_ms_, interval_ms_, keep.size());
    }
  }
  if (log_stable) {
    Log(L"tray", L"key stable=%d churn=%u", stable, churn);
    if (stable == 0) {
      Log(L"tray", L"runtime id churn; falling back to automation_id+class+order");
    }
  }
}

void TrayMirror::WorkerLoop() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
  auto backend = MakeUiaTrayBackend();
  const bool probed = backend != nullptr && backend->Probe();
  Log(L"tray", L"backend=%hs capture=no hide_mode=hidden right_click=bamti_menu overflow=not_mirrored probe=%d",
      backend != nullptr ? backend->Name() : "none", probed ? 1 : 0);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  for (;;) {
    if (stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
      break;
    }

    bool reset = false;
    bool active = false;
    bool slow_stop = false;
    UINT interval = 1000;
    {
      std::lock_guard lock(mu_);
      reset = reset_pending_;
      reset_pending_ = false;
      active = active_;
      slow_stop = stopped_slow_;
      interval = interval_ms_;
    }
    if (slow_stop) {
      break;
    }
    if (reset && backend != nullptr) {
      backend->Reset();
    }
    if (active && backend != nullptr) {
      DoRound(backend.get());
      std::lock_guard lock(mu_);
      interval = interval_ms_;
      slow_stop = stopped_slow_;
    }
    if (slow_stop) {
      break;
    }

    HANDLE waits[3]{};
    DWORD n = 0;
    waits[n++] = stop_event_;
    waits[n++] = wake_event_;
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
    const DWORD wait = WaitForMultipleObjects(n, waits, FALSE, active ? flush_ms : INFINITE);
    if (wait == WAIT_OBJECT_0) {
      break;
    }
    if (wait == WAIT_TIMEOUT) {
      if (sink != nullptr) {
        sink->Flush();
      }
      continue;
    }
    if (wait == WAIT_FAILED) {
      Log(L"tray", L"wait failed err=%lu", GetLastError());
      Sleep(50);
    }
  }

  if (timer != nullptr) {
    CloseHandle(timer);
  }
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
  Log(L"tray", L"worker stop");
}

}  // namespace bamti
