#include "tray_mirror.hpp"

#include "log.hpp"
#include "tray_intercept.hpp"

#include <objbase.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bamti {
namespace {

constexpr int kTrayPriorityBase = 5;
constexpr ULONGLONG kPerfLogMs = 300000;
constexpr UINT kSlowEnumMs = 200;
constexpr size_t kTrayMenuLabelMax = 40;

uint64_t ParseKeyText(const std::string& one) {
  if (one.size() >= 2 && one[0] == '0' && (one[1] == 'x' || one[1] == 'X')) {
    return static_cast<uint64_t>(strtoull(one.c_str() + 2, nullptr, 16));
  }
  return static_cast<uint64_t>(strtoull(one.c_str(), nullptr, 16));
}

std::wstring ClipMenuLabel(std::wstring text) {
  if (text.size() > kTrayMenuLabelMax) {
    text.resize(kTrayMenuLabelMax);
  }
  return text;
}

std::wstring HiddenKeyLabel(uint64_t key) {
  const std::string hex = TrayMirror::KeyText(key);
  std::wstring out = L"(숨김) ";
  const size_t start = (hex.size() >= 2 && hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) ? 2 : 0;
  for (size_t i = 0; i < 6 && start + i < hex.size(); ++i) {
    out.push_back(static_cast<wchar_t>(hex[start + i]));
  }
  return out;
}

bool GuidEmpty(const GUID& g) {
  const auto* p = reinterpret_cast<const uint8_t*>(&g);
  for (size_t i = 0; i < sizeof(GUID); ++i) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

struct KnownIcon {
  GUID guid;
  const wchar_t* label;
};

const KnownIcon kKnownIcons[] = {
    // 볼륨. explorer, 관측 tip "DELL U4025QW: 100%"
    {{0x7820AE73, 0x23E3, 0x4229, {0x82, 0xC1, 0xE4, 0x1C, 0xB6, 0x7D, 0x5B, 0x9C}}, L"볼륨"},
    // 배터리. explorer, 관측 tip 빈 값 후 "배터리 상태: …"
    {{0x7820AE75, 0x23E3, 0x4229, {0x82, 0xC1, 0xE4, 0x1C, 0xB6, 0x7D, 0x5B, 0x9C}}, L"배터리"},
};

const wchar_t* LabelForGuid(const GUID& guid) {
  if (GuidEmpty(guid)) {
    return nullptr;
  }
  for (const KnownIcon& one : kKnownIcons) {
    if (InlineIsEqualGUID(one.guid, guid)) {
      return one.label;
    }
  }
  return nullptr;
}

std::wstring MenuLabel(const std::wstring& tip, const GUID& guid, bool hidden, uint64_t key) {
  if (const wchar_t* known = LabelForGuid(guid)) {
    return ClipMenuLabel(known);
  }
  if (!tip.empty()) {
    return ClipMenuLabel(tip);
  }
  if (hidden) {
    return HiddenKeyLabel(key);
  }
  return ClipMenuLabel(L"(이름 없음)");
}
constexpr int kSlowStreakStop = 3;
constexpr UINT kEventDebounceMs = 300;
constexpr UINT kEventMinIntervalMs = 1000;
constexpr UINT kSafetyIntervalMs = 5000;
constexpr ULONGLONG kDiagWindowMs = 180000;
constexpr ULONGLONG kDiagEnumMinMs = 5000;
constexpr UINT kFloodWindowMs = 10000;
constexpr long kFloodMaxEnums = 8;
constexpr ULONGLONG kFloodRetryMs = 300000;
constexpr wchar_t kClockClass[] = L"SystemTray.OmniButton";
constexpr wchar_t kShowDesktopClass[] = L"SystemTray.ShowDesktopButton";
constexpr wchar_t kQuickSettingsClass[] = L"SystemTray.AccentButton";
constexpr wchar_t kQuickVolumeClass[] = L"SystemTray.OmniButtonCenter";
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

std::wstring RosterTips(const std::vector<TrayIconInfo>& icons) {
  std::wstring tips;
  for (size_t i = 0; i < icons.size(); ++i) {
    if (i != 0) {
      tips.push_back(L'|');
    }
    std::wstring one = SanitizeTipForLog(icons[i].tip);
    if (one.empty()) {
      one = L"(no tip)";
    }
    tips += one;
  }
  if (tips.size() > 1000) {
    tips.resize(1000);
    tips += L"...";
  }
  return tips;
}

void LogRoster(const wchar_t* which, const std::vector<TrayIconInfo>& icons) {
  const std::wstring tips = RosterTips(icons);
  Log(L"tray", L"%s roster n=%u tips=\"%s\"", which, static_cast<unsigned>(icons.size()), tips.c_str());
}

void LogDiagRosters(TrayBackend* intercept, TrayBackend* uia) {
  std::vector<TrayIconInfo> uia_icons;
  std::vector<TrayIconInfo> intercept_icons;
  if (uia != nullptr) {
    uia->Enumerate(&uia_icons);
  }
  if (intercept != nullptr) {
    intercept->Enumerate(&intercept_icons);
  }
  LogRoster(L"intercept", intercept_icons);
  LogRoster(L"uia", uia_icons);
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
    }
  }
  if (!settings_.tray_mirror) {
    TakePrestartedInterceptTrayBackend();
    return true;
  }
  if (settings_.tray_backend == "intercept") {
    StartIntercept();
    if (intercept_ == nullptr) {
      Log(L"tray", L"intercept unavailable; using uia");
    }
  }
  {
    std::lock_guard lock(mu_);
    StartWorkerLocked();
  }
  TakePrestartedInterceptTrayBackend();
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
  auto pre = TakePrestartedInterceptTrayBackend();
  StopIntercept();
  if (pre != nullptr) {
    intercept_ = std::move(pre);
  } else {
    intercept_ = MakeInterceptTrayBackend();
    if (intercept_ == nullptr || !intercept_->Probe()) {
      Log(L"tray", L"intercept spy failed; explorer receives icons directly");
      intercept_.reset();
      return;
    }
  }
  if (rect_lookup_) {
    intercept_->SetRectLookup(rect_lookup_);
  }
}

void TrayMirror::StopIntercept() {
  intercept_.reset();
}

void TrayMirror::SetRectLookup(std::function<bool(uint64_t key, RECT* screen)> lookup) {
  rect_lookup_ = std::move(lookup);
  if (intercept_ != nullptr) {
    intercept_->SetRectLookup(rect_lookup_);
  }
}

bool TrayMirror::ForwardsContextMenu() const {
  return intercept_ != nullptr;
}

DWORD TrayMirror::OwnerPid(uint64_t key) const {
  std::lock_guard lock(mu_);
  const auto it = items_.find(key);
  if (it == items_.end()) {
    return 0;
  }
  return it->second.owner_pid;
}

void TrayMirror::SetSettings(const WidgetSettings& next) {
  bool start_worker = false;
  bool stop_worker = false;
  bool restart_backend = false;
  std::vector<std::string> drop;
  StatusSink* sink = nullptr;
  std::string backend;
  {
    std::lock_guard lock(mu_);
    const bool was = settings_.tray_mirror;
    const std::string was_backend = settings_.tray_backend;
    settings_ = next;
    backend = next.tray_backend;
    sink = sink_;
    if (next.tray_mirror && !was && sink_ != nullptr && !stopped_slow_) {
      start_worker = true;
      reset_pending_ = true;
    } else if (!next.tray_mirror && was) {
      stop_worker = true;
    } else if (next.tray_mirror && was && was_backend != next.tray_backend) {
      restart_backend = true;
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
  if (restart_backend) {
    StopWorker();
    StopIntercept();
    DropAll();
    if (backend == "intercept") {
      StartIntercept();
      if (intercept_ == nullptr) {
        Log(L"tray", L"intercept unavailable; using uia");
      }
    }
    std::lock_guard lock(mu_);
    reset_pending_ = true;
    StartWorkerLocked();
  }
  if (start_worker) {
    if (backend == "intercept") {
      StartIntercept();
      if (intercept_ == nullptr) {
        Log(L"tray", L"intercept unavailable; using uia");
      }
    }
    std::lock_guard lock(mu_);
    StartWorkerLocked();
  }
}

void TrayMirror::OnExplorerRestart() {
  TrayBackend* intercept = nullptr;
  {
    std::lock_guard lock(mu_);
    reset_pending_ = true;
    intercept = intercept_.get();
    if (wake_event_ != nullptr) {
      SetEvent(wake_event_);
    }
  }
  if (intercept != nullptr) {
    intercept->OnShellRestart();
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

std::vector<TrayMirror::MenuItem> TrayMirror::MenuItems() const {
  std::lock_guard lock(mu_);
  std::vector<ItemState> vis;
  vis.reserve(items_.size());
  for (const auto& [key, st] : items_) {
    vis.push_back(st);
  }
  std::sort(vis.begin(), vis.end(), [](const ItemState& a, const ItemState& b) {
    if (a.order != b.order) {
      return a.order < b.order;
    }
    return a.key < b.key;
  });

  std::vector<MenuItem> out;
  std::unordered_set<uint64_t> seen;
  out.reserve(vis.size() + settings_.tray_hidden_keys.size());
  for (const ItemState& st : vis) {
    MenuItem row;
    row.key = st.key;
    row.shown = true;
    GUID guid = st.guid;
    if (GuidEmpty(guid)) {
      const auto remembered = last_tips_.find(st.key);
      if (remembered != last_tips_.end()) {
        guid = remembered->second.guid;
      }
    }
    row.label = MenuLabel(st.tip, guid, false, st.key);
    out.push_back(std::move(row));
    seen.insert(st.key);
  }
  for (const std::string& one : settings_.tray_hidden_keys) {
    const uint64_t key = ParseKeyText(one);
    if (key == 0 || seen.find(key) != seen.end()) {
      continue;
    }
    MenuItem row;
    row.key = key;
    row.shown = false;
    std::wstring tip;
    GUID guid{};
    const auto remembered = last_tips_.find(key);
    if (remembered != last_tips_.end()) {
      tip = remembered->second.tip;
      guid = remembered->second.guid;
    }
    row.label = MenuLabel(tip, guid, true, key);
    out.push_back(std::move(row));
    seen.insert(key);
  }
  return out;
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
  const bool dblclk = ev.event == "dblclick";
  if ((!dblclk && ev.event != "click") || (ev.button != "left" && ev.button != "right")) {
    return;
  }
  const uint64_t key = ParseId(ev.id);
  if (key == 0) {
    return;
  }
  {
    std::lock_guard lock(mu_);
    pending_invoke_ = key;
    pending_right_ = ev.button == "right";
    pending_dblclk_ = dblclk;
  }
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void TrayMirror::DrainInvoke(TrayBackend* backend) {
  uint64_t key = 0;
  bool right = false;
  bool dblclk = false;
  {
    std::lock_guard lock(mu_);
    key = pending_invoke_;
    pending_invoke_ = 0;
    right = pending_right_;
    pending_right_ = false;
    dblclk = pending_dblclk_;
    pending_dblclk_ = false;
  }
  if (key == 0 || backend == nullptr) {
    return;
  }
  TrayIconInfo icon;
  icon.key = key;
  if (backend->Invoke(icon, right, dblclk)) {
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
  if (icon.class_name == kQuickSettingsClass || icon.class_name == kQuickVolumeClass) {
    return false;
  }
  // explorer가 콜백 없이 등록한 옛 시스템 아이콘(볼륨, 전원)은 누를 수 없다.
  // 같은 기능을 내장 위젯과 제어 센터가 이미 담당한다.
  if (icon.callback_message == 0 && _wcsicmp(icon.owner_exe.c_str(), L"explorer.exe") == 0) {
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
    if (!icon.tip.empty() || !GuidEmpty(icon.guid_item)) {
      LastTip& rec = last_tips_[icon.key];
      if (!icon.tip.empty()) {
        rec.tip = icon.tip;
      }
      if (!GuidEmpty(icon.guid_item)) {
        rec.guid = icon.guid_item;
      }
    }
    ItemState st;
    st.key = icon.key;
    st.tip = icon.tip;
    st.guid = icon.guid_item;
    st.order = order;
    st.visible = item.visible;
    st.icon_hash = item.icon.cache_key;
    st.id = item.id;
    if (icon.owner != nullptr) {
      GetWindowThreadProcessId(icon.owner, &st.owner_pid);
    }
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
  std::vector<LastTip> tips;
  std::vector<uint64_t> tip_keys;
  next.reserve(raw.size());
  keep.reserve(raw.size());
  tips.reserve(raw.size());
  tip_keys.reserve(raw.size());
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
    if (copy.key != 0 && (!copy.tip.empty() || !GuidEmpty(copy.guid_item))) {
      LastTip rec;
      rec.tip = copy.tip;
      rec.guid = copy.guid_item;
      tip_keys.push_back(copy.key);
      tips.push_back(std::move(rec));
    }
    if (!Include(copy, overflow, settings)) {
      continue;
    }
    ItemState st;
    st.key = copy.key;
    st.tip = copy.tip;
    st.guid = copy.guid_item;
    st.order = copy.order;
    st.visible = copy.from_overflow ? true : (copy.offscreen == FALSE);
    st.icon_hash = copy.png.empty() ? 0 : Fnv1a64(copy.png.data(), copy.png.size());
    st.id = MakeId(copy.key);
    if (copy.owner != nullptr) {
      GetWindowThreadProcessId(copy.owner, &st.owner_pid);
    }
    next.push_back(st);
    keep.push_back(std::move(copy));
  }

  std::unordered_map<uint64_t, ItemState> prev;
  {
    std::lock_guard lock(mu_);
    prev = items_;
    for (size_t i = 0; i < tips.size(); ++i) {
      LastTip& rec = last_tips_[tip_keys[i]];
      if (!tips[i].tip.empty()) {
        rec.tip = tips[i].tip;
      }
      if (!GuidEmpty(tips[i].guid)) {
        rec.guid = tips[i].guid;
      }
    }
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
  bool probed = backend != nullptr && backend->Probe();
  bool intercept = intercept_ != nullptr && backend == intercept_.get();
  bool events_abandoned = false;
  bool events_retried = false;
  bool events_gave_up = false;
  bool events_live = false;
  std::unique_ptr<TrayBackend> diag_uia;
  ULONGLONG diag_started = 0;
  ULONGLONG last_diag_enum = 0;
  if (intercept) {
    diag_uia = MakeUiaTrayBackend();
    if (diag_uia != nullptr) {
      diag_uia->Probe();
      diag_started = GetTickCount64();
    }
  }
  if (backend != nullptr && !events_abandoned && !intercept) {
    events_live = backend->SubscribeStructureChanged(struct_event_);
  }
  if (intercept) {
    events_live = true;
  }
  Log(L"tray",
      L"backend=%hs capture=no hide_mode=hidden right_click=%hs overflow=mirrored "
      L"structure_changed=%d probe=%d",
      backend != nullptr ? backend->Name() : "none", intercept ? "app" : "bamti_menu", events_live ? 1 : 0,
      probed ? 1 : 0);

  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
  bool run_enum = true;
  bool struct_pending = false;
  ULONGLONG last_enum = 0;
  ULONGLONG last_struct = 0;
  ULONGLONG flood_t0 = 0;
  long flood_n = 0;
  ULONGLONG flood_abandon_at = 0;

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
    if (!intercept && events_abandoned && !events_gave_up && !events_retried && flood_abandon_at != 0) {
      const ULONGLONG now = GetTickCount64();
      if (now - flood_abandon_at >= kFloodRetryMs) {
        events_abandoned = false;
        events_retried = true;
        flood_n = 0;
        flood_t0 = 0;
        if (backend != nullptr) {
          backend->AllowStructureRetry();
          events_live = backend->SubscribeStructureChanged(struct_event_);
        }
        Log(L"tray", L"structure_changed retry after flood structure_changed=%d", events_live ? 1 : 0);
      }
    }
    if (intercept && (intercept_ == nullptr || !intercept_->ParseLive())) {
      Log(L"tray", L"intercept parse failed; switching to uia");
      if (diag_uia != nullptr) {
        LogDiagRosters(intercept_.get(), diag_uia.get());
        diag_uia.reset();
      }
      if (backend != nullptr) {
        backend->UnsubscribeStructureChanged();
      }
      StopIntercept();
      owned = MakeUiaTrayBackend();
      backend = owned.get();
      intercept = false;
      probed = backend != nullptr && backend->Probe();
      events_abandoned = false;
      events_live = backend != nullptr && backend->SubscribeStructureChanged(struct_event_);
      reset = true;
      Log(L"tray",
          L"backend=%hs capture=no hide_mode=hidden right_click=bamti_menu overflow=mirrored "
          L"structure_changed=%d probe=%d",
          backend != nullptr ? backend->Name() : "none", events_live ? 1 : 0, probed ? 1 : 0);
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
      if (diag_uia != nullptr) {
        const ULONGLONG now = GetTickCount64();
        if (diag_started != 0 && now - diag_started >= kDiagWindowMs) {
          LogDiagRosters(backend, diag_uia.get());
          diag_uia.reset();
        } else if (last_diag_enum == 0 || now - last_diag_enum >= kDiagEnumMinMs) {
          std::vector<TrayIconInfo> diag;
          diag_uia->Enumerate(&diag);
          last_diag_enum = now;
        }
      }
      last_enum = GetTickCount64();
      run_enum = false;
      struct_pending = false;
      if (!intercept && events_live && !events_abandoned) {
        if (flood_n == 0 || last_enum - flood_t0 >= kFloodWindowMs) {
          flood_t0 = last_enum;
          flood_n = 0;
        }
        ++flood_n;
        if (flood_n > kFloodMaxEnums) {
          events_abandoned = true;
          events_live = false;
          flood_abandon_at = last_enum;
          if (backend != nullptr) {
            backend->AbandonStructureChanged();
          }
          if (events_retried) {
            events_gave_up = true;
            Log(L"tray", L"structure_changed flood enums=%ld window_ms=%llu; polling only (no retry)", flood_n,
                last_enum - flood_t0);
          } else {
            Log(L"tray", L"structure_changed flood enums=%ld window_ms=%llu; polling only", flood_n,
                last_enum - flood_t0);
          }
        }
      }
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
      if (backend != nullptr) {
        backend->TakeStructureChangedCount();
      }
      if (active) {
        last_struct = GetTickCount64();
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
  if (diag_uia != nullptr) {
    LogDiagRosters(backend, diag_uia.get());
    diag_uia.reset();
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
