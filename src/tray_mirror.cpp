#include "tray_mirror.hpp"

#include "log.hpp"
#include "task_list.hpp"
#include "tray_intercept.hpp"

#include <objbase.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <iterator>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace bamti {
namespace {

std::unordered_set<uint64_t> g_live_keys;

constexpr int kTrayPriorityBase = 5;
constexpr ULONGLONG kPerfLogMs = 300000;
constexpr UINT kSlowEnumMs = 200;
constexpr int kFillIconPx = 32;
constexpr size_t kFillOwnerMax = 256;
constexpr int kFillUiaKeyProbeRounds = 6;
constexpr size_t kTrayMenuLabelMax = 40;
constexpr wchar_t kTaskMgrTipHead[] = L"작업 관리자";

struct FillUiaKeyProbe {
  int rounds = 0;
  std::unordered_set<uint64_t> keys;
  std::unordered_map<uint64_t, int> seen;
  std::unordered_set<uint64_t> tm_keys;
  std::unordered_set<std::wstring> tm_tips;
  bool done = false;
};

FillUiaKeyProbe g_fill_uia_key_probe;

bool FillCandidate(const TrayIconInfo& icon);

void ResetFillUiaKeyProbe() {
  g_fill_uia_key_probe = {};
}

bool TipLooksLikeTaskMgr(const std::wstring& tip) {
  const size_t n = sizeof(kTaskMgrTipHead) / sizeof(kTaskMgrTipHead[0]) - 1;
  return tip.size() >= n && tip.compare(0, n, kTaskMgrTipHead) == 0;
}

void NoteFillUiaKeyProbe(const std::vector<TrayIconInfo>& icons) {
  FillUiaKeyProbe& p = g_fill_uia_key_probe;
  if (p.done) {
    return;
  }
  for (const TrayIconInfo& icon : icons) {
    if (!FillCandidate(icon)) {
      continue;
    }
    p.keys.insert(icon.key);
    ++p.seen[icon.key];
    if (TipLooksLikeTaskMgr(icon.tip)) {
      p.tm_keys.insert(icon.key);
      p.tm_tips.insert(icon.tip);
    }
  }
  ++p.rounds;
  if (p.rounds < kFillUiaKeyProbeRounds) {
    return;
  }
  p.done = true;
  int persistent = 0;
  for (const auto& kv : p.seen) {
    if (kv.second == p.rounds) {
      ++persistent;
    }
  }
  Log(L"tray", L"fill uia key probe rounds=%d unique=%zu persistent=%d tm_keys=%zu tm_tips=%zu tm_stable=%d",
      p.rounds, p.keys.size(), persistent, p.tm_keys.size(), p.tm_tips.size(), p.tm_keys.size() <= 1 ? 1 : 0);
}

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

std::wstring GuidText(const GUID& g) {
  wchar_t buf[64]{};
  if (StringFromGUID2(g, buf, 64) <= 0) {
    return L"{}";
  }
  return buf;
}

constexpr wchar_t kBluetoothTrayTip[] = L"Bluetooth 장치";
constexpr wchar_t kExplorerExe[] = L"explorer.exe";

bool IsSystemBluetoothIcon(const TrayIconInfo& icon) {
  if (icon.tip != kBluetoothTrayTip) {
    return false;
  }
  if (icon.owner_exe.empty()) {
    return true;
  }
  return _wcsicmp(icon.owner_exe.c_str(), kExplorerExe) == 0;
}

void LogHideBluetoothOnce(const TrayIconInfo& icon) {
  static std::atomic<bool> logged{false};
  if (logged.exchange(true)) {
    return;
  }
  const std::wstring guid = GuidEmpty(icon.guid_item) ? std::wstring(L"{}") : GuidText(icon.guid_item);
  Log(L"tray", L"hiding system bluetooth icon (widget on) guid=%s", guid.c_str());
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
constexpr ULONGLONG kFillRefreshMs = 5000;
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

bool FillCandidate(const TrayIconInfo& icon) {
  return !icon.system_icon && icon.automation_id == L"NotifyItemIcon" && !icon.tip.empty();
}

bool SameFillItem(const TrayIconInfo& intercept, const TrayIconInfo& uia) {
  if (intercept.tip.empty() || uia.tip.empty()) {
    return false;
  }
  if (intercept.tip != uia.tip) {
    return false;
  }
  if (!intercept.owner_exe.empty() && !uia.owner_exe.empty() &&
      _wcsicmp(intercept.owner_exe.c_str(), uia.owner_exe.c_str()) != 0) {
    return false;
  }
  return true;
}

bool InterceptOwns(const std::vector<TrayIconInfo>& intercept, const TrayIconInfo& uia) {
  for (const TrayIconInfo& one : intercept) {
    if (one.key != 0 && one.key == uia.key) {
      return true;
    }
    if (SameFillItem(one, uia)) {
      return true;
    }
  }
  return false;
}

void LogUiaDetails(const std::vector<TrayIconInfo>& icons) {
  for (const TrayIconInfo& icon : icons) {
    const std::wstring tip = SanitizeTipForLog(icon.tip);
    Log(L"tray", L"uia detail key=0x%llX autoid=%s class=%s offscreen=%d tip=\"%s\"",
        static_cast<unsigned long long>(icon.key), icon.automation_id.c_str(), icon.class_name.c_str(),
        icon.offscreen ? 1 : 0, tip.empty() ? L"(no tip)" : tip.c_str());
  }
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
    fill_icons_.clear();
    fill_keys_.clear();
    fill_logged_.clear();
    fill_skip_unknown_.clear();
    fill_detail_logged_ = false;
    ResetFillUiaKeyProbe();
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
        const ItemState& st = it->second;
        bool hidden = KeyHidden(it->first, next.tray_hidden_keys);
        if (!hidden && !st.stable.empty()) {
          for (const std::string& one : next.tray_hidden) {
            if (one == st.stable) {
              hidden = true;
              break;
            }
          }
        }
        if (hidden) {
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
    fill_keys_.clear();
  }
  fill_icons_.clear();
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

std::string TrayMirror::StableKey(const TrayIconInfo& icon) {
  if (!icon.owner_exe.empty() && icon.owner_exe != L"?") {
    std::wstring exe = icon.owner_exe;
    CharLowerBuffW(exe.data(), static_cast<DWORD>(exe.size()));
    return "exe:" + WideToUtf8Bytes(exe) + "#" + std::to_string(icon.uid);
  }
  if (!icon.tip.empty()) {
    std::wstring tip = icon.tip;
    if (tip.size() > 64) {
      tip.resize(64);
    }
    return "tip:" + WideToUtf8Bytes(tip);
  }
  return {};
}

bool TrayMirror::StableHidden(const TrayIconInfo& icon, const std::vector<std::string>& hidden) {
  const std::string key = StableKey(icon);
  if (key.empty()) {
    return false;
  }
  for (const std::string& one : hidden) {
    if (one == key) {
      return true;
    }
  }
  return false;
}

std::string TrayMirror::StableForKey(uint64_t key) const {
  std::lock_guard lock(mu_);
  const auto it = items_.find(key);
  if (it == items_.end()) {
    return {};
  }
  return it->second.stable;
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
  std::unordered_set<std::string> seen_stable;
  out.reserve(vis.size() + settings_.tray_hidden_keys.size() + settings_.tray_hidden.size());
  for (const ItemState& st : vis) {
    MenuItem row;
    row.key = st.key;
    row.shown = true;
    row.stable = st.stable;
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
    if (!st.stable.empty()) {
      seen_stable.insert(st.stable);
    }
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
    const std::string hex = KeyText(key);
    std::wstring key_w(hex.begin(), hex.end());
    Log(L"tray", L"hidden key=%s live=%d remembered=%d label=%s", key_w.c_str(),
        g_live_keys.count(key) ? 1 : 0, remembered != last_tips_.end() ? 1 : 0, row.label.c_str());
    if (LabelForGuid(guid) == nullptr && tip.empty()) {
      continue;
    }
    out.push_back(std::move(row));
    seen.insert(key);
  }
  for (const std::string& one : settings_.tray_hidden) {
    if (one.empty() || seen_stable.find(one) != seen_stable.end()) {
      continue;
    }
    MenuItem row;
    row.shown = false;
    row.stable = one;
    std::wstring tip;
    GUID guid{};
    const auto remembered = last_stable_tips_.find(one);
    if (remembered != last_stable_tips_.end()) {
      tip = remembered->second.tip;
      guid = remembered->second.guid;
    }
    row.label = MenuLabel(tip, guid, true, row.key);
    if (LabelForGuid(guid) == nullptr && tip.empty()) {
      continue;
    }
    out.push_back(std::move(row));
    seen_stable.insert(one);
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

bool TrayMirror::InvokeByExe(const std::wstring& exe_path) {
  if (exe_path.empty()) {
    return false;
  }
  std::vector<std::pair<uint64_t, DWORD>> candidates;
  {
    std::lock_guard lock(mu_);
    if (!settings_.tray_mirror || stopped_slow_ || !worker_.joinable()) {
      return false;
    }
    candidates.reserve(items_.size());
    for (const auto& [key, st] : items_) {
      if (st.owner_pid != 0) {
        candidates.emplace_back(key, st.owner_pid);
      }
    }
  }
  uint64_t match = 0;
  for (const auto& [key, pid] : candidates) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
      continue;
    }
    wchar_t buf[MAX_PATH]{};
    DWORD n = MAX_PATH;
    std::wstring image;
    if (QueryFullProcessImageNameW(process, 0, buf, &n) != FALSE && n > 0) {
      image.assign(buf, n);
    } else {
      std::wstring grow(32768, L'\0');
      n = static_cast<DWORD>(grow.size());
      if (QueryFullProcessImageNameW(process, 0, grow.data(), &n) != FALSE) {
        grow.resize(n);
        image = std::move(grow);
      }
    }
    CloseHandle(process);
    if (!image.empty() && SameDockPin(exe_path, image)) {
      match = key;
      break;
    }
  }
  if (match == 0) {
    return false;
  }
  {
    std::lock_guard lock(mu_);
    if (!settings_.tray_mirror || stopped_slow_ || !worker_.joinable()) {
      return false;
    }
    pending_invoke_ = match;
    pending_right_ = false;
    pending_dblclk_ = true;
  }
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
  return true;
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

void TrayMirror::DrainInvoke(TrayBackend* backend, TrayBackend* fill_uia) {
  uint64_t key = 0;
  bool right = false;
  bool dblclk = false;
  bool fill = false;
  {
    std::lock_guard lock(mu_);
    key = pending_invoke_;
    pending_invoke_ = 0;
    right = pending_right_;
    pending_right_ = false;
    dblclk = pending_dblclk_;
    pending_dblclk_ = false;
    fill = fill_keys_.count(key) != 0;
  }
  if (key == 0) {
    return;
  }
  TrayIconInfo icon;
  icon.key = key;
  TrayBackend* target = backend;
  if (fill && fill_uia != nullptr) {
    target = fill_uia;
  }
  if (target == nullptr) {
    return;
  }
  if (target->Invoke(icon, right, dblclk)) {
    return;
  }
  HRESULT hr = E_FAIL;
  const char* pattern = "none";
  target->LastInvokeError(&hr, &pattern);
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
  if (KeyHidden(icon.key, settings.tray_hidden_keys) || StableHidden(icon, settings.tray_hidden)) {
    return false;
  }
  if (settings.bluetooth && IsSystemBluetoothIcon(icon)) {
    LogHideBluetoothOnce(icon);
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
      const std::string stable = StableKey(icon);
      if (!stable.empty()) {
        LastTip& rec_s = last_stable_tips_[stable];
        if (!icon.tip.empty()) {
          rec_s.tip = icon.tip;
        }
        if (!GuidEmpty(icon.guid_item)) {
          rec_s.guid = icon.guid_item;
        }
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
    st.stable = StableKey(icon);
    if (icon.owner != nullptr) {
      GetWindowThreadProcessId(icon.owner, &st.owner_pid);
    }
    items_[icon.key] = std::move(st);
  }
  if (sink != nullptr) {
    sink->Upsert(std::move(item));
  }
}

void TrayMirror::RememberFillOwners(const std::vector<TrayIconInfo>& intercept) {
  for (const TrayIconInfo& icon : intercept) {
    if (icon.owner == nullptr || icon.tip.empty() || IsWindow(icon.owner) == FALSE) {
      continue;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(icon.owner, &pid);
    if (pid == 0) {
      continue;
    }
    std::wstring path;
    const auto exe_it = fill_exe_by_owner_.find(icon.owner);
    if (exe_it != fill_exe_by_owner_.end() && exe_it->second.first == pid) {
      path = exe_it->second.second;
    } else {
      path = WindowExePath(icon.owner);
      if (path.empty()) {
        continue;
      }
      fill_exe_by_owner_[icon.owner] = {pid, path};
    }
    const auto tip_it = fill_tip_by_owner_.find(icon.owner);
    if (tip_it != fill_tip_by_owner_.end()) {
      if (tip_it->second == icon.tip) {
        auto own = fill_owners_.find(icon.tip);
        if (own != fill_owners_.end() && own->second.pid == pid) {
          own->second.owner = icon.owner;
          own->second.exe_path = path;
          continue;
        }
      } else {
        fill_owners_.erase(tip_it->second);
      }
    }
    FillOwner rec;
    rec.exe_path = std::move(path);
    rec.owner = icon.owner;
    rec.pid = pid;
    fill_owners_[icon.tip] = rec;
    fill_tip_by_owner_[icon.owner] = icon.tip;
  }
  PruneFillOwners();
}

void TrayMirror::PruneFillOwners() {
  if (fill_owners_.size() <= kFillOwnerMax && fill_exe_by_owner_.size() <= kFillOwnerMax &&
      fill_tip_by_owner_.size() <= kFillOwnerMax) {
    return;
  }
  std::vector<HWND> dead;
  dead.reserve(fill_exe_by_owner_.size());
  for (const auto& one : fill_exe_by_owner_) {
    if (IsWindow(one.first) == FALSE) {
      dead.push_back(one.first);
    }
  }
  for (HWND hwnd : dead) {
    const auto tip_it = fill_tip_by_owner_.find(hwnd);
    if (tip_it != fill_tip_by_owner_.end()) {
      fill_owners_.erase(tip_it->second);
      fill_tip_by_owner_.erase(tip_it);
    }
    fill_exe_by_owner_.erase(hwnd);
  }
  Log(L"tray", L"fill owners pruned n=%zu", dead.size());
}

bool TrayMirror::FillPngForExe(const std::wstring& exe_path, std::vector<uint8_t>* out) {
  if (out == nullptr || exe_path.empty()) {
    return false;
  }
  const auto cached = fill_png_cache_.find(exe_path);
  if (cached != fill_png_cache_.end()) {
    if (cached->second.empty()) {
      return false;
    }
    *out = cached->second;
    return true;
  }
  HICON extracted = nullptr;
  const UINT got =
      PrivateExtractIconsW(exe_path.c_str(), 0, kFillIconPx, kFillIconPx, &extracted, nullptr, 1, LR_DEFAULTCOLOR);
  std::vector<uint8_t> png;
  bool ok = false;
  if (got != 0 && extracted != nullptr) {
    ok = IconToPng(extracted, &png);
    DestroyIcon(extracted);
  }
  fill_png_cache_[exe_path] = ok ? png : std::vector<uint8_t>{};
  if (!ok) {
    return false;
  }
  *out = std::move(png);
  return true;
}

void TrayMirror::RefreshUiaFill(TrayBackend* uia, const std::vector<TrayIconInfo>& intercept) {
  if (uia == nullptr) {
    return;
  }
  std::vector<TrayIconInfo> icons;
  const ULONGLONG t0 = GetTickCount64();
  if (!uia->Enumerate(&icons)) {
    return;
  }
  const ULONGLONG ms = GetTickCount64() - t0;
  if (ms > kSlowEnumMs) {
    Log(L"tray", L"fill enum slow ms=%llu count=%zu", ms, icons.size());
  }
  if (!fill_detail_logged_) {
    fill_detail_logged_ = true;
    LogRoster(L"intercept", intercept);
    LogRoster(L"uia", icons);
    LogUiaDetails(icons);
  }
  NoteFillUiaKeyProbe(icons);
  fill_icons_.clear();
  for (const TrayIconInfo& icon : icons) {
    if (FillCandidate(icon)) {
      fill_icons_.push_back(icon);
    }
  }
}

void TrayMirror::MergeUiaFill(std::vector<TrayIconInfo>* raw) {
  if (raw == nullptr) {
    return;
  }
  std::unordered_set<uint64_t> prev_keys;
  std::unordered_map<uint64_t, std::wstring> prev_tips;
  {
    std::lock_guard lock(mu_);
    prev_keys = fill_keys_;
    for (uint64_t key : prev_keys) {
      const auto it = items_.find(key);
      if (it != items_.end()) {
        prev_tips[key] = it->second.tip;
      }
    }
  }
  std::vector<TrayIconInfo> extra;
  extra.reserve(fill_icons_.size());
  for (const TrayIconInfo& icon : fill_icons_) {
    if (InterceptOwns(*raw, icon)) {
      continue;
    }
    const auto owner = fill_owners_.find(icon.tip);
    if (owner == fill_owners_.end()) {
      if (fill_skip_unknown_.insert(icon.key).second) {
        Log(L"tray", L"fill skip unknown key=0x%llX tip=\"%s\"", static_cast<unsigned long long>(icon.key),
            SanitizeTipForLog(icon.tip).c_str());
      }
      continue;
    }
    if (IsWindow(owner->second.owner) == FALSE) {
      continue;
    }
    DWORD now_pid = 0;
    GetWindowThreadProcessId(owner->second.owner, &now_pid);
    if (now_pid != owner->second.pid) {
      continue;
    }
    TrayIconInfo copy = icon;
    if (!FillPngForExe(owner->second.exe_path, &copy.png)) {
      continue;
    }
    copy.owner = owner->second.owner;
    copy.owner_exe = owner->second.exe_path;
    extra.push_back(std::move(copy));
  }
  std::unordered_set<uint64_t> keys;
  keys.reserve(extra.size());
  for (const TrayIconInfo& icon : extra) {
    keys.insert(icon.key);
    if (fill_logged_.insert(icon.key).second) {
      const std::wstring tip = SanitizeTipForLog(icon.tip);
      const wchar_t* exe = icon.owner_exe.empty() ? L"-" : icon.owner_exe.c_str();
      Log(L"tray", L"fill key=0x%llX exe=%s tip=\"%s\"", static_cast<unsigned long long>(icon.key), exe, tip.c_str());
    }
  }
  for (uint64_t key : prev_keys) {
    if (keys.count(key) != 0) {
      continue;
    }
    fill_logged_.erase(key);
    std::wstring tip;
    const auto remembered = prev_tips.find(key);
    if (remembered != prev_tips.end()) {
      tip = SanitizeTipForLog(remembered->second);
    }
    Log(L"tray", L"fill drop key=0x%llX tip=\"%s\"", static_cast<unsigned long long>(key), tip.c_str());
  }
  {
    std::lock_guard lock(mu_);
    fill_keys_ = std::move(keys);
  }
  raw->insert(raw->end(), extra.begin(), extra.end());
}

void TrayMirror::DoRound(TrayBackend* backend, bool events_live, TrayBackend* fill_uia, bool refresh_fill) {
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
  RememberFillOwners(raw);
  if (slow_streak_ >= kSlowStreakStop) {
    Log(L"tray", L"auto-stop enum_ms=%llu over %u ms x%d", elapsed, kSlowEnumMs, kSlowStreakStop);
    DropAll();
    std::lock_guard lock(mu_);
    stopped_slow_ = true;
    return;
  }
  if (fill_uia != nullptr) {
    if (refresh_fill) {
      RefreshUiaFill(fill_uia, raw);
      // 보충 열거가 느리면 그 사이에 가로채기 응답이 들어온다. 최신 목록으로 다시 맞춘다.
      backend->Enumerate(&raw);
      RememberFillOwners(raw);
    }
    MergeUiaFill(&raw);
  }

  const int overflow = OverflowOrder(raw);
  std::vector<ItemState> next;
  std::vector<TrayIconInfo> keep;
  std::vector<LastTip> tips;
  std::vector<uint64_t> tip_keys;
  std::vector<std::string> tip_stables;
  std::vector<uint64_t> live;
  next.reserve(raw.size());
  keep.reserve(raw.size());
  tips.reserve(raw.size());
  tip_keys.reserve(raw.size());
  tip_stables.reserve(raw.size());
  live.reserve(raw.size());
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
    if (copy.key != 0) {
      live.push_back(copy.key);
    }
    if (copy.key != 0 && (!copy.tip.empty() || !GuidEmpty(copy.guid_item))) {
      LastTip rec;
      rec.tip = copy.tip;
      rec.guid = copy.guid_item;
      tip_keys.push_back(copy.key);
      tips.push_back(std::move(rec));
      tip_stables.push_back(StableKey(copy));
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
    st.stable = StableKey(copy);
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
    g_live_keys.clear();
    g_live_keys.insert(live.begin(), live.end());
    for (size_t i = 0; i < tips.size(); ++i) {
      LastTip& rec = last_tips_[tip_keys[i]];
      if (!tips[i].tip.empty()) {
        rec.tip = tips[i].tip;
      }
      if (!GuidEmpty(tips[i].guid)) {
        rec.guid = tips[i].guid;
      }
      if (!tip_stables[i].empty()) {
        LastTip& rec_s = last_stable_tips_[tip_stables[i]];
        if (!tips[i].tip.empty()) {
          rec_s.tip = tips[i].tip;
        }
        if (!GuidEmpty(tips[i].guid)) {
          rec_s.guid = tips[i].guid;
        }
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
  bool fill_started = false;
  ULONGLONG last_fill_refresh = 0;
  bool diag_finished = false;
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
      {
        std::lock_guard lock(mu_);
        fill_keys_.clear();
      }
      fill_icons_.clear();
      fill_logged_.clear();
      fill_skip_unknown_.clear();
      fill_detail_logged_ = false;
      ResetFillUiaKeyProbe();
      fill_started = false;
      last_fill_refresh = 0;
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
      DrainInvoke(backend, intercept ? diag_uia.get() : nullptr);
    }
    if (active && backend != nullptr && run_enum) {
      bool refresh_fill = false;
      if (intercept && diag_uia != nullptr) {
        const ULONGLONG now = GetTickCount64();
        if (!fill_started) {
          refresh_fill = true;
          fill_started = true;
        }
        if (backend->TakeStartupFillPulse() > 0) {
          refresh_fill = true;
        }
        if (last_fill_refresh == 0 || now - last_fill_refresh >= kFillRefreshMs) {
          refresh_fill = true;
        }
        if (refresh_fill) {
          last_fill_refresh = now;
        }
      }
      DoRound(backend, events_live, intercept ? diag_uia.get() : nullptr, refresh_fill);
      if (diag_uia != nullptr && !diag_finished) {
        const ULONGLONG now = GetTickCount64();
        if (diag_started != 0 && now - diag_started >= kDiagWindowMs) {
          LogDiagRosters(backend, diag_uia.get());
          diag_finished = true;
        } else if (!refresh_fill && (last_diag_enum == 0 || now - last_diag_enum >= kDiagEnumMinMs)) {
          std::vector<TrayIconInfo> diag;
          diag_uia->Enumerate(&diag);
          last_diag_enum = now;
        } else if (refresh_fill) {
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
