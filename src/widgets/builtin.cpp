#include "widgets/builtin.hpp"

#include "log.hpp"
#include "status_item.hpp"
#include "theme.hpp"
#include "widgets/brightness.hpp"
#include "widgets/volume.hpp"

// netioapi.h (via iphlpapi.h) needs _WS2IPDEF_. Do not include winsock2.h.
#include <ws2def.h>
#include <ws2ipdef.h>
#include <appmodel.h>
#include <iphlpapi.h>
#include <netioapi.h>
#include <objbase.h>
#include <shellapi.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <optional>

namespace bamti {
namespace {

constexpr char kBatteryId[] = "bamti.widget/battery";
constexpr char kCpuId[] = "bamti.widget/cpu";
constexpr char kVolumeId[] = "bamti.widget/volume";
constexpr char kNetworkId[] = "bamti.widget/network";
constexpr char kBoardId[] = "bamti.widget/board";

constexpr int kBatteryPriority = 40;
constexpr int kVolumePriority = 35;
constexpr int kNetworkPriority = 36;
constexpr int kCpuPriority = 30;
constexpr int kBoardPriority = 10;

constexpr ULONGLONG kBatteryPeriodMs = 60000;
constexpr ULONGLONG kCpuPeriodMs = 5000;
constexpr ULONGLONG kVolumePeriodMs = 1000;
constexpr ULONGLONG kBrightnessPeriodMs = 2000;
constexpr ULONGLONG kVolumeRefreshMs = 20000;
constexpr ULONGLONG kFirstSampleMs = 1000;

// Segoe Fluent Icons가 없을 때 DrawVectorIcon이 되돌리는 문자.
[[maybe_unused]] constexpr wchar_t kVolumeGlyph[] = L"♪";
[[maybe_unused]] constexpr wchar_t kVolumeMuteGlyph[] = L"♪";
constexpr wchar_t kBoardGlyph[] = L"▤";

uint64_t FileTimeToU64(const FILETIME& ft) {
  ULARGE_INTEGER u;
  u.LowPart = ft.dwLowDateTime;
  u.HighPart = ft.dwHighDateTime;
  return u.QuadPart;
}

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

float ClampUnit(double value) {
  if (!std::isfinite(value) || value < 0.0) {
    return 0.0f;
  }
  if (value > 1.0) {
    return 1.0f;
  }
  return static_cast<float>(value);
}

std::wstring PercentText(int pct, bool plus) {
  if (pct < 0) {
    pct = 0;
  }
  if (pct > 100) {
    pct = 100;
  }
  wchar_t buf[16]{};
  swprintf_s(buf, plus ? L"+%d%%" : L"%d%%", pct);
  return buf;
}

std::wstring RemainText(DWORD seconds) {
  if (seconds == static_cast<DWORD>(-1)) {
    return {};
  }
  const DWORD hours = seconds / 3600;
  const DWORD mins = (seconds % 3600) / 60;
  wchar_t buf[64]{};
  if (hours > 0) {
    swprintf_s(buf, L"남은 시간 %u시간 %u분", hours, mins);
  } else {
    swprintf_s(buf, L"남은 시간 %u분", mins);
  }
  return buf;
}

void SetGlyph(StatusItem* item, const wchar_t* glyph) {
  item->icon.kind = IconKind::kGlyph;
  item->icon.glyph = glyph;
  if (item->icon.glyph.size() > kStatusGlyphMaxChars) {
    item->icon.glyph.resize(kStatusGlyphMaxChars);
  }
  item->icon.cache_key = HashStatusIcon(item->icon);
}

void SetVectorIcon(StatusItem* item, VectorIcon vector, float value, uint32_t flags) {
  item->icon.kind = IconKind::kVector;
  item->icon.vector = vector;
  item->icon.value = ClampUnit(value);
  item->icon.flags = flags;
  item->icon.glyph.clear();
  item->icon.cache_key = HashStatusIcon(item->icon);
}

StatusRow GaugeRow(std::wstring label, float value, std::wstring value_text, std::wstring detail = {}) {
  StatusRow row;
  row.type = RowType::kGauge;
  row.label = Truncate(std::move(label), kStatusPanelTextMaxChars);
  row.value = ClampUnit(value);
  row.value_text = Truncate(std::move(value_text), kStatusPanelTextMaxChars);
  row.detail = Truncate(std::move(detail), kStatusPanelTextMaxChars);
  return row;
}

StatusRow KvRow(std::wstring label, std::wstring value) {
  StatusRow row;
  row.type = RowType::kKeyValue;
  row.label = Truncate(std::move(label), kStatusPanelTextMaxChars);
  row.value_text = Truncate(std::move(value), kStatusPanelTextMaxChars);
  return row;
}

StatusRow SepRow() {
  StatusRow row;
  row.type = RowType::kSeparator;
  return row;
}

StatusRow ButtonRow(const char* row_id, std::wstring label) {
  StatusRow row;
  row.type = RowType::kButton;
  row.row_id = row_id;
  row.label = Truncate(std::move(label), kStatusPanelTextMaxChars);
  return row;
}

StatusRow ToggleRow(const char* row_id, std::wstring label, bool on) {
  StatusRow row;
  row.type = RowType::kToggle;
  row.row_id = row_id;
  row.label = Truncate(std::move(label), kStatusPanelTextMaxChars);
  row.on = on;
  return row;
}

StatusRow SliderRow(const char* row_id, std::wstring label, float value, std::wstring value_text) {
  StatusRow row;
  row.type = RowType::kSlider;
  row.row_id = row_id;
  row.label = Truncate(std::move(label), kStatusPanelTextMaxChars);
  row.value = ClampUnit(value);
  row.value_text = Truncate(std::move(value_text), kStatusPanelTextMaxChars);
  return row;
}

std::wstring Fingerprint(const StatusItem& item) {
  std::wstring fp = item.icon.glyph;
  fp.push_back(L'\x1f');
  fp += item.text;
  fp.push_back(L'\x1f');
  fp += std::to_wstring(static_cast<int>(item.state));
  fp.push_back(L'\x1f');
  fp += item.tooltip;
  if (!item.panel) {
    return fp;
  }
  fp.push_back(L'\x1f');
  fp += item.panel->title;
  fp += item.panel->subtitle;
  fp += item.panel->updated_text;
  for (const StatusRow& row : item.panel->rows) {
    fp.push_back(L'\x1f');
    fp += std::to_wstring(static_cast<int>(row.type));
    fp.append(row.row_id.begin(), row.row_id.end());
    fp += row.label;
    fp += row.value_text;
    fp += row.detail;
    fp += row.note;
  }
  return fp;
}

bool ReadCpuTimes(uint64_t* idle, uint64_t* kernel, uint64_t* user) {
  FILETIME fi{};
  FILETIME fk{};
  FILETIME fu{};
  if (!GetSystemTimes(&fi, &fk, &fu)) {
    return false;
  }
  *idle = FileTimeToU64(fi);
  *kernel = FileTimeToU64(fk);
  *user = FileTimeToU64(fu);
  return true;
}

enum class NetKind { kNone, kWifi, kEthernet };

struct RouteSnap {
  NetKind kind = NetKind::kNone;
  std::wstring alias;
  double route_ms = 0.0;
  double entry_ms = 0.0;
  DWORD route_err = NO_ERROR;
};

RouteSnap ReadDefaultRoute() {
  RouteSnap snap;
  SOCKADDR_INET dest{};
  dest.si_family = AF_INET;
  dest.Ipv4.sin_family = AF_INET;
  dest.Ipv4.sin_addr.s_addr = 0;

  MIB_IPFORWARD_ROW2 route{};
  SOCKADDR_INET source{};
  LARGE_INTEGER freq{};
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  snap.route_err = GetBestRoute2(nullptr, 0, nullptr, &dest, 0, &route, &source);
  QueryPerformanceCounter(&t1);
  if (freq.QuadPart != 0) {
    snap.route_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
  }
  if (snap.route_err != NO_ERROR) {
    return snap;
  }

  MIB_IF_ROW2 row{};
  row.InterfaceIndex = route.InterfaceIndex;
  QueryPerformanceCounter(&t0);
  const DWORD entry_err = GetIfEntry2(&row);
  QueryPerformanceCounter(&t1);
  if (freq.QuadPart != 0) {
    snap.entry_ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
  }
  if (entry_err != NO_ERROR) {
    return snap;
  }
  snap.alias = row.Alias;
  if (row.Type == IF_TYPE_IEEE80211 || row.PhysicalMediumType == NdisPhysicalMediumNative802_11) {
    snap.kind = NetKind::kWifi;
  } else if (row.Type == IF_TYPE_ETHERNET_CSMACD) {
    snap.kind = NetKind::kEthernet;
  }
  return snap;
}

void OpenWidgetBoard() {
  INPUT in[4]{};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wVk = VK_LWIN;
  in[1].type = INPUT_KEYBOARD;
  in[1].ki.wVk = 'W';
  in[2].type = INPUT_KEYBOARD;
  in[2].ki.wVk = 'W';
  in[2].ki.dwFlags = KEYEVENTF_KEYUP;
  in[3].type = INPUT_KEYBOARD;
  in[3].ki.wVk = VK_LWIN;
  in[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, in, sizeof(INPUT));
}

INT_PTR ShellOpen(const wchar_t* target) {
  return reinterpret_cast<INT_PTR>(ShellExecuteW(nullptr, L"open", target, nullptr, nullptr, SW_SHOWNORMAL));
}

constexpr ULONGLONG kBoardCheckPeriodMs = 30000;

bool WebExperienceInstalled() {
  UINT32 count = 0;
  UINT32 bytes = 0;
  const LONG rc = FindPackagesByPackageFamily(
      L"MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy", PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT, &count,
      nullptr, &bytes, nullptr, nullptr);
  if (rc == ERROR_INSUFFICIENT_BUFFER) {
    return count > 0;
  }
  if (rc == ERROR_SUCCESS) {
    return count > 0;
  }
  return false;
}

std::optional<int> ReadTaskbarDa() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LSTATUS rc = RegGetValueW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
                                  L"TaskbarDa", RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (rc != ERROR_SUCCESS) {
    return std::nullopt;
  }
  return static_cast<int>(value);
}

bool TaskbarWidgetsEnabled() {
  const std::optional<int> value = ReadTaskbarDa();
  if (!value) {
    return true;
  }
  return *value != 0;
}

bool WidgetBoardAvailable() {
  static const bool package = WebExperienceInstalled();
  static std::atomic<ULONGLONG> checked_at{0};
  static std::atomic<bool> enabled{false};
  if (!package) {
    return false;
  }
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG checked = checked_at.load(std::memory_order_relaxed);
  if (checked == 0 || now - checked >= kBoardCheckPeriodMs) {
    const bool next = TaskbarWidgetsEnabled();
    enabled.store(next, std::memory_order_relaxed);
    checked_at.store(now, std::memory_order_relaxed);
    return next;
  }
  return enabled.load(std::memory_order_relaxed);
}

}  // namespace

bool IsWidgetBoardAvailable() {
  return WidgetBoardAvailable();
}

BuiltinWidgets::BuiltinWidgets() {
  volume_ = std::make_unique<VolumeControl>();
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  wake_event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  save_idle_event_ = CreateEventW(nullptr, TRUE, TRUE, nullptr);
  volume_->BindWake(wake_event_);
}

BuiltinWidgets::~BuiltinWidgets() {
  Stop();
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  if (wake_event_ != nullptr) {
    CloseHandle(wake_event_);
    wake_event_ = nullptr;
  }
  if (save_idle_event_ != nullptr) {
    CloseHandle(save_idle_event_);
    save_idle_event_ = nullptr;
  }
}

const char* BuiltinWidgets::Name() const {
  return "builtin";
}

bool BuiltinWidgets::Start(StatusSink* sink) {
  Stop();
  const bool package = WebExperienceInstalled();
  const std::optional<int> da = ReadTaskbarDa();
  const int da_log = da ? *da : -1;
  const bool available = package && (!da || *da != 0);
  Log(L"widget", L"board available=%d package=%d taskbar_da=%d", available ? 1 : 0, package ? 1 : 0, da_log);
  std::lock_guard lock(mu_);
  sink_ = sink;
  // 메시지 루프 전의 1KB 미만 읽기라 UI 응답성 문제가 없고, 백그라운드로 넘기면 위젯 생성 경합만 생긴다.
  settings_ = LoadWidgetSettings();
  reset_pending_ = true;
  if (settings_.Any()) {
    StartWorkerLocked();
  }
  return true;
}

void BuiltinWidgets::Stop() {
  StopWorker();
  {
    std::lock_guard lock(mu_);
    sink_ = nullptr;
  }
  if (save_idle_event_ != nullptr) {
    const DWORD wait = WaitForSingleObject(save_idle_event_, 2000);
    if (wait == WAIT_TIMEOUT) {
      Log(L"widget", L"settings save still running at stop");
    }
  }
}

void BuiltinWidgets::StartWorkerLocked() {
  if (worker_.joinable()) {
    if (wake_event_ != nullptr) {
      SetEvent(wake_event_);
    }
    return;
  }
  if (stop_event_ == nullptr || wake_event_ == nullptr) {
    Log(L"widget", L"worker events missing");
    return;
  }
  ResetEvent(stop_event_);
  worker_ = std::thread([this] { WorkerLoop(); });
}

void BuiltinWidgets::StopWorker() {
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

WidgetSettings BuiltinWidgets::settings() const {
  std::lock_guard lock(mu_);
  return settings_;
}

ControlCenterLive BuiltinWidgets::LiveForControlCenter() const {
  std::lock_guard lock(mu_);
  ControlCenterLive live;
  live.volume_ok = last_volume_ok_;
  live.volume = last_volume_;
  live.muted = last_muted_;
  live.brightness_ok = last_brightness_ok_;
  live.brightness = last_brightness_;
  live.wifi_on = last_wifi_on_;
  live.wifi_name = last_wifi_name_;
  live.eth_on = last_eth_on_;
  live.eth_name = last_eth_name_;
  return live;
}

void BuiltinWidgets::SetSettings(const WidgetSettings& next) {
  bool stop_worker = false;
  bool submit_save = false;
  StatusSink* sink = nullptr;
  const char* drop[6]{};
  size_t drop_n = 0;
  {
    std::lock_guard lock(mu_);
    const WidgetSettings prev = settings_;
    settings_ = next;
    pending_save_ = next;
    ++save_gen_;
    if (!save_busy_) {
      save_busy_ = true;
      submit_save = true;
      if (save_idle_event_ != nullptr) {
        ResetEvent(save_idle_event_);
      }
    }
    sink = sink_;
    auto note_drop = [&](bool was, bool now, const char* id, std::wstring* fp) {
      if (was && !now) {
        drop[drop_n++] = id;
        fp->clear();
      }
    };
    note_drop(prev.battery, next.battery, kBatteryId, &fp_battery_);
    note_drop(prev.cpu, next.cpu, kCpuId, &fp_cpu_);
    note_drop(prev.network, next.network, kNetworkId, &fp_network_);
    note_drop(prev.volume, next.volume, kVolumeId, &fp_volume_);
    note_drop(prev.widget_board, next.widget_board, kBoardId, &fp_board_);
    const ULONGLONG now = GetTickCount64();
    if (!prev.battery && next.battery) {
      battery_due_ = now;
      logged_no_battery_ = false;
    }
    if (!prev.cpu && next.cpu) {
      cpu_has_baseline_ = false;
      cpu_due_ = now;
    }
    if (!prev.network && next.network) {
      network_due_ = now;
    }
    if (!prev.volume && next.volume) {
      volume_due_ = now;
      volume_refresh_due_ = 0;
      logged_no_volume_ = false;
    }
    if (!prev.control_center && next.control_center) {
      volume_due_ = now;
      brightness_due_ = now;
      network_due_ = now;
    }
    if (next.Any()) {
      StartWorkerLocked();
    } else if (worker_.joinable()) {
      stop_worker = true;
    }
  }
  if (sink != nullptr) {
    for (size_t i = 0; i < drop_n; ++i) {
      sink->Remove(drop[i]);
    }
  }
  if (stop_worker) {
    StopWorker();
  }
  if (submit_save) {
    SubmitSave();
  }
}

void BuiltinWidgets::NotePowerEvent(bool resumed) {
  {
    std::lock_guard lock(mu_);
    if (resumed) {
      reset_pending_ = true;
    } else {
      power_pending_ = true;
    }
  }
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void BuiltinWidgets::SetActive(bool active) {
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
  Log(L"widget", L"active=%d", active ? 1 : 0);
  if (wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void BuiltinWidgets::OnEvent(const StatusEvent& ev) {
  PendingAction action{};
  bool have = false;
  bool wake = false;
  if (ev.event == "slide" && ev.id == kVolumeId && ev.row_id == "volume_level") {
    std::lock_guard lock(mu_);
    pending_level_ = ClampUnit(ev.value);
    wake = true;
  } else if (ev.event == "scroll" && ev.id == kVolumeId) {
    std::lock_guard lock(mu_);
    const float base = pending_level_.has_value() ? *pending_level_ : last_volume_;
    const float next = ClampUnit(base + ev.value);
    pending_level_ = next;
    if (ev.value > 0.0f && last_muted_) {
      pending_mute_ = false;  // 볼륨을 올리면 음소거를 푼다.
    }
    wake = true;
  } else if (ev.event == "slide" && ev.id == "bamti.control_center" && ev.row_id == "brightness") {
    std::lock_guard lock(mu_);
    pending_brightness_ = ClampUnit(ev.value);
    wake = true;
  } else if (ev.event == "toggle" && ev.id == kVolumeId && ev.row_id == "volume_mute") {
    std::lock_guard lock(mu_);
    pending_mute_ = ev.on;
    wake = true;
  } else if (ev.event == "click" && ev.id == kBoardId && ev.button == "left") {
    action = PendingAction::kWidgetBoard;
    have = true;
  } else if (ev.event == "invoke") {
    if (ev.row_id == "power_settings") {
      action = PendingAction::kPowerSettings;
      have = true;
    } else if (ev.row_id == "network_settings") {
      action = PendingAction::kNetworkSettings;
      have = true;
    } else if (ev.row_id == "wifi_settings") {
      action = PendingAction::kWifiSettings;
      have = true;
    } else if (ev.row_id == "task_manager") {
      action = PendingAction::kTaskManager;
      have = true;
    } else if (ev.row_id == "sound_settings") {
      action = PendingAction::kSoundSettings;
      have = true;
    }
  }
  if (have) {
    std::lock_guard lock(mu_);
    actions_.push_back(action);
    wake = true;
  }
  if (wake && wake_event_ != nullptr) {
    SetEvent(wake_event_);
  }
}

void BuiltinWidgets::SubmitSave() {
  if (!TrySubmitThreadpoolCallback(&BuiltinWidgets::SaveSettingsCallback, this, nullptr)) {
    std::lock_guard lock(mu_);
    save_busy_ = false;
    if (save_idle_event_ != nullptr) {
      SetEvent(save_idle_event_);
    }
    Log(L"widget", L"settings save submit failed");
  }
}

VOID CALLBACK BuiltinWidgets::SaveSettingsCallback(PTP_CALLBACK_INSTANCE instance, PVOID ctx) {
  (void)instance;
  static_cast<BuiltinWidgets*>(ctx)->DrainSaves();
}

void BuiltinWidgets::DrainSaves() {
  for (;;) {
    WidgetSettings snap;
    uint64_t gen = 0;
    {
      std::lock_guard lock(mu_);
      snap = pending_save_;
      gen = save_gen_;
    }
    SaveWidgetSettings(snap);
    {
      std::lock_guard lock(mu_);
      if (gen != save_gen_) {
        continue;
      }
      save_busy_ = false;
      if (save_idle_event_ != nullptr) {
        SetEvent(save_idle_event_);
      }
      return;
    }
  }
}

void BuiltinWidgets::ResetBaselines() {
  uint64_t idle = 0;
  uint64_t kernel = 0;
  uint64_t user = 0;
  const bool cpu_ok = ReadCpuTimes(&idle, &kernel, &user);
  const ULONGLONG now = GetTickCount64();
  std::lock_guard lock(mu_);
  cpu_has_baseline_ = cpu_ok;
  if (cpu_ok) {
    cpu_idle_ = idle;
    cpu_kernel_ = kernel;
    cpu_user_ = user;
  }
  cpu_due_ = now + kFirstSampleMs;
  battery_due_ = now;
  volume_due_ = now;
  network_due_ = now;
}

bool BuiltinWidgets::HasSampleDeadlineLocked() const {
  return settings_.battery || settings_.cpu || settings_.network || settings_.volume ||
         settings_.control_center;
}

ULONGLONG BuiltinWidgets::NextDeadlineLocked(ULONGLONG now) const {
  (void)now;
  ULONGLONG due = MAXULONGLONG;
  if (settings_.battery && battery_due_ < due) {
    due = battery_due_;
  }
  if (settings_.cpu && cpu_due_ < due) {
    due = cpu_due_;
  }
  if ((settings_.volume || settings_.control_center) && volume_due_ < due) {
    due = volume_due_;
  }
  if (settings_.control_center && brightness_due_ < due) {
    due = brightness_due_;
  }
  if ((settings_.network || settings_.control_center) && network_due_ < due) {
    due = network_due_;
  }
  return due;
}

void BuiltinWidgets::Publish(StatusItem item) {
  item.source = "builtin";
  const std::wstring fp = Fingerprint(item);
  StatusSink* sink = nullptr;
  {
    std::lock_guard lock(mu_);
    std::wstring* slot = nullptr;
    if (item.id == kBatteryId) {
      slot = &fp_battery_;
    } else if (item.id == kCpuId) {
      slot = &fp_cpu_;
    } else if (item.id == kVolumeId) {
      slot = &fp_volume_;
    } else if (item.id == kNetworkId) {
      slot = &fp_network_;
    } else if (item.id == kBoardId) {
      slot = &fp_board_;
    }
    if (slot != nullptr && *slot == fp) {
      return;
    }
    sink = sink_;
    if (sink == nullptr) {
      return;
    }
    if (slot != nullptr) {
      *slot = fp;
    }
  }
  if (const auto prev = sink->Get(item.id)) {
    item.revision = prev->revision + 1;
  } else {
    item.revision = 1;
  }
  sink->Upsert(std::move(item));
}

void BuiltinWidgets::DropItem(const char* id) {
  StatusSink* sink = nullptr;
  {
    std::lock_guard lock(mu_);
    if (std::strcmp(id, kBatteryId) == 0) {
      fp_battery_.clear();
    } else if (std::strcmp(id, kCpuId) == 0) {
      fp_cpu_.clear();
    } else if (std::strcmp(id, kVolumeId) == 0) {
      fp_volume_.clear();
    } else if (std::strcmp(id, kNetworkId) == 0) {
      fp_network_.clear();
    } else if (std::strcmp(id, kBoardId) == 0) {
      fp_board_.clear();
    }
    sink = sink_;
  }
  if (sink != nullptr) {
    sink->Remove(id);
  }
}

void BuiltinWidgets::Execute(PendingAction action) {
  INT_PTR rc = 33;
  switch (action) {
    case PendingAction::kPowerSettings:
      rc = ShellOpen(L"ms-settings:powersleep");
      break;
    case PendingAction::kNetworkSettings:
      rc = ShellOpen(L"ms-settings:network");
      break;
    case PendingAction::kWifiSettings:
      rc = ShellOpen(L"ms-settings:network-wifi");
      break;
    case PendingAction::kTaskManager:
      rc = ShellOpen(L"taskmgr.exe");
      break;
    case PendingAction::kSoundSettings:
      rc = ShellOpen(L"ms-settings:sound");
      break;
    case PendingAction::kWidgetBoard:
      if (!WidgetBoardAvailable()) {
        static bool logged = false;
        if (!logged) {
          logged = true;
          Log(L"widget", L"widget board unavailable; ignoring click");
        }
        return;
      }
      OpenWidgetBoard();
      return;
  }
  if (rc <= 32) {
    Log(L"widget", L"ShellExecute failed rc=%d", static_cast<int>(rc));
  }
}

void BuiltinWidgets::PublishBoard() {
  StatusItem item;
  item.id = kBoardId;
  item.priority = kBoardPriority;
  SetGlyph(&item, kBoardGlyph);
  item.tooltip = Truncate(L"위젯 보드 열기", kStatusPanelTextMaxChars);
  Publish(std::move(item));
}

void BuiltinWidgets::SampleBattery() {
  SYSTEM_POWER_STATUS status{};
  const BOOL ok = GetSystemPowerStatus(&status);
  const ULONGLONG now = GetTickCount64();
  {
    std::lock_guard lock(mu_);
    battery_due_ = now + kBatteryPeriodMs;
    if (!settings_.battery || !active_ || sink_ == nullptr) {
      return;
    }
  }
  if (!ok || (status.BatteryFlag & BATTERY_FLAG_NO_BATTERY) != 0 || status.BatteryLifePercent == 255) {
    bool log_now = false;
    {
      std::lock_guard lock(mu_);
      if (!logged_no_battery_) {
        logged_no_battery_ = true;
        log_now = true;
      }
    }
    if (log_now) {
      Log(L"widget", L"no battery; hiding %hs", kBatteryId);
    }
    DropItem(kBatteryId);
    return;
  }

  const int pct = static_cast<int>(status.BatteryLifePercent);
  const bool ac = status.ACLineStatus == 1;
  const bool charging = (status.BatteryFlag & BATTERY_FLAG_CHARGING) != 0;
  const float level = static_cast<float>(pct) / 100.0f;

  StatusItem item;
  item.id = kBatteryId;
  item.priority = kBatteryPriority;
  SetVectorIcon(&item, VectorIcon::kBattery, level, ac ? kVectorFlagCharging : 0);
  item.text = Truncate(PercentText(pct, false), kStatusTextMaxChars);
  std::wstring tip = L"배터리 ";
  tip += PercentText(pct, false);
  if (charging) {
    tip += L" · 충전 중";
  } else if (ac) {
    tip += L" · 연결됨";
  } else {
    const std::wstring remain = RemainText(status.BatteryLifeTime);
    if (!remain.empty()) {
      tip += L" · ";
      tip += remain;
    }
  }
  item.tooltip = Truncate(std::move(tip), kStatusPanelTextMaxChars);
  if (!ac && pct <= 10) {
    item.state = StatusState::kError;
  } else if (!ac && pct <= 20) {
    item.state = StatusState::kWarn;
  } else {
    item.state = StatusState::kNormal;
  }

  StatusPanel panel;
  panel.title = L"배터리";
  std::wstring power = L"알 수 없음";
  if (status.ACLineStatus == 0) {
    power = L"배터리 사용 중";
  } else if (status.ACLineStatus == 1) {
    power = L"연결됨";
  }
  panel.rows.push_back(GaugeRow(L"잔량", level, PercentText(pct, false),
                                ac ? std::wstring{} : RemainText(status.BatteryLifeTime)));
  panel.rows.back().fill_rgb = BatteryFillRgb(ShellUsesDarkMode(), level, ac);
  panel.rows.push_back(KvRow(L"전원", std::move(power)));
  panel.rows.push_back(KvRow(L"절전 모드", (status.SystemStatusFlag & 1) != 0 ? L"켜짐" : L"꺼짐"));
  panel.rows.push_back(SepRow());
  panel.rows.push_back(ButtonRow("power_settings", L"전원 설정 열기"));
  item.panel = std::move(panel);
  Publish(std::move(item));
}

void BuiltinWidgets::SampleCpu() {
  uint64_t idle = 0;
  uint64_t kernel = 0;
  uint64_t user = 0;
  if (!ReadCpuTimes(&idle, &kernel, &user)) {
    std::lock_guard lock(mu_);
    cpu_due_ = GetTickCount64() + kCpuPeriodMs;
    return;
  }

  double usage = 0.0;
  double user_share = 0.0;
  double kernel_share = 0.0;
  bool publish = false;
  {
    std::lock_guard lock(mu_);
    const ULONGLONG now = GetTickCount64();
    if (!cpu_has_baseline_) {
      cpu_idle_ = idle;
      cpu_kernel_ = kernel;
      cpu_user_ = user;
      cpu_has_baseline_ = true;
      cpu_due_ = now + kFirstSampleMs;
      return;
    }
    const uint64_t idle_d = idle - cpu_idle_;
    const uint64_t kernel_d = kernel - cpu_kernel_;
    const uint64_t user_d = user - cpu_user_;
    cpu_idle_ = idle;
    cpu_kernel_ = kernel;
    cpu_user_ = user;
    cpu_due_ = now + kCpuPeriodMs;
    if (!settings_.cpu || !active_ || sink_ == nullptr) {
      return;
    }
    const uint64_t total = kernel_d + user_d;
    if (total > 0) {
      usage = 1.0 - static_cast<double>(idle_d) / static_cast<double>(total);
      user_share = static_cast<double>(user_d) / static_cast<double>(total);
      const uint64_t kernel_only = kernel_d > idle_d ? kernel_d - idle_d : 0;
      kernel_share = static_cast<double>(kernel_only) / static_cast<double>(total);
    }
    if (usage < 0.0) {
      usage = 0.0;
    }
    if (usage > 1.0) {
      usage = 1.0;
    }
    publish = true;
  }
  if (!publish) {
    return;
  }

  const int pct = static_cast<int>(usage * 100.0);
  const int user_pct = static_cast<int>(user_share * 100.0);
  const int kernel_pct = static_cast<int>(kernel_share * 100.0);

  SYSTEM_INFO info{};
  GetSystemInfo(&info);
  wchar_t nproc[16]{};
  swprintf_s(nproc, L"%u", info.dwNumberOfProcessors);

  const float level = static_cast<float>(pct) / 100.0f;

  StatusItem item;
  item.id = kCpuId;
  item.priority = kCpuPriority;
  SetVectorIcon(&item, VectorIcon::kCpu, level, 0);
  item.text = Truncate(PercentText(pct, false), kStatusTextMaxChars);
  wchar_t tip[128]{};
  swprintf_s(tip, L"CPU %d%% · 사용자 %d%% · 커널 %d%%", pct, user_pct, kernel_pct);
  item.tooltip = Truncate(tip, kStatusPanelTextMaxChars);
  item.state = StatusState::kNormal;

  StatusPanel panel;
  panel.title = L"CPU";
  panel.rows.push_back(GaugeRow(L"전체 사용률", level, PercentText(pct, false)));
  panel.rows.back().fill_rgb = CpuFillRgb(ShellUsesDarkMode(), level);
  panel.rows.push_back(KvRow(L"사용자", PercentText(user_pct, false)));
  panel.rows.push_back(KvRow(L"커널", PercentText(kernel_pct, false)));
  panel.rows.push_back(KvRow(L"논리 프로세서", nproc));
  panel.rows.push_back(SepRow());
  panel.rows.push_back(ButtonRow("task_manager", L"작업 관리자 열기"));
  item.panel = std::move(panel);
  Publish(std::move(item));
}

void BuiltinWidgets::SampleVolume() {
  const ULONGLONG now = GetTickCount64();
  bool refresh = false;
  bool enabled = false;
  {
    std::lock_guard lock(mu_);
    volume_due_ = now + kVolumePeriodMs;
    if (volume_refresh_due_ == 0) {
      volume_refresh_due_ = now + kVolumeRefreshMs;
    } else if (now >= volume_refresh_due_) {
      refresh = true;
      volume_refresh_due_ = now + kVolumeRefreshMs;
    }
    enabled = (settings_.volume || settings_.control_center) && active_ && sink_ != nullptr;
  }
  if (!enabled) {
    volume_->Release();
    return;
  }
  if (refresh) {
    volume_->Release();
  }
  const VolumeState state = volume_->Read();
  if (!state.ok) {
    bool log_now = false;
    {
      std::lock_guard lock(mu_);
      if (!logged_no_volume_) {
        logged_no_volume_ = true;
        log_now = true;
      }
    }
    if (log_now) {
      Log(L"widget", L"no audio device; hiding %hs", kVolumeId);
    }
    DropItem(kVolumeId);
    {
      std::lock_guard lock(mu_);
      last_volume_ok_ = false;
    }
    return;
  }
  bool publish = false;
  {
    std::lock_guard lock(mu_);
    logged_no_volume_ = false;
    last_volume_ok_ = true;
    last_volume_ = state.level;
    last_muted_ = state.muted;
    publish = settings_.volume;
  }

  if (!publish) {
    return;
  }

  const int pct = static_cast<int>(state.level * 100.0f + 0.5f);
  StatusItem item;
  item.id = kVolumeId;
  item.priority = kVolumePriority;
  SetVectorIcon(&item, VectorIcon::kVolume, state.level, state.muted ? kVectorFlagMuted : 0);
  if (state.muted) {
    item.text = Truncate(L"음소거", kStatusTextMaxChars);
    item.state = StatusState::kOff;
  } else {
    item.text = Truncate(PercentText(pct, false), kStatusTextMaxChars);
    item.state = StatusState::kNormal;
  }
  std::wstring tip = L"볼륨 ";
  if (state.muted) {
    tip += L"음소거";
  } else {
    tip += PercentText(pct, false);
  }
  if (!state.device.empty()) {
    tip += L" · ";
    tip += state.device;
  }
  item.tooltip = Truncate(std::move(tip), kStatusPanelTextMaxChars);

  StatusPanel panel;
  panel.title = L"장치";
  panel.subtitle = Truncate(state.device, kStatusPanelTextMaxChars);
  panel.rows.push_back(SliderRow("volume_level", L"크기", state.level, PercentText(pct, false)));
  panel.rows.push_back(ToggleRow("volume_mute", L"음소거", state.muted));
  panel.rows.push_back(SepRow());
  panel.rows.push_back(ButtonRow("sound_settings", L"소리 설정 열기"));
  item.panel = std::move(panel);
  Publish(std::move(item));
}

void BuiltinWidgets::SampleBrightness() {
  bool enabled = false;
  bool probe = false;
  BrightnessBackend backend = BrightnessBackend::kNone;
  {
    std::lock_guard lock(mu_);
    brightness_due_ = GetTickCount64() + kBrightnessPeriodMs;
    enabled = settings_.control_center && active_;
    probe = !brightness_probed_;
    backend = brightness_backend_;
  }
  if (!enabled) {
    return;
  }
  if (probe) {
    const BrightnessSample ddc = ProbeDdcciBrightness();
    Log(L"cc", L"brightness ddcci=%d value=%lu took %.2f ms", ddc.ok ? 1 : 0, ddc.value, ddc.ms);
    const BrightnessSample wmi = ProbeWmiBrightness();
    Log(L"cc", L"brightness wmi=%d value=%lu took %.2f ms", wmi.ok ? 1 : 0, wmi.value, wmi.ms);
    BrightnessSample chosen;
    if (ddc.ok) {
      chosen = ddc;
    } else if (wmi.ok) {
      chosen = wmi;
    }
    std::lock_guard lock(mu_);
    brightness_probed_ = true;
    brightness_backend_ = chosen.backend;
    last_brightness_ok_ = chosen.ok;
    last_brightness_ = chosen.ok ? static_cast<float>(chosen.value) / 100.0f : 0.0f;
    if (!chosen.ok) {
      brightness_due_ = MAXULONGLONG;
    }
    return;
  }
  if (backend == BrightnessBackend::kNone) {
    return;
  }
  const BrightnessSample sample =
      backend == BrightnessBackend::kDdcci ? ProbeDdcciBrightness() : ProbeWmiBrightness();
  std::lock_guard lock(mu_);
  last_brightness_ok_ = sample.ok;
  if (sample.ok) {
    last_brightness_ = static_cast<float>(sample.value) / 100.0f;
  }
}

void BuiltinWidgets::SampleNetwork() {
  bool enabled = false;
  {
    std::lock_guard lock(mu_);
    network_due_ = GetTickCount64() + kBrightnessPeriodMs;
    enabled = (settings_.network || settings_.control_center) && active_;
  }
  if (!enabled) {
    return;
  }
  const RouteSnap route = ReadDefaultRoute();
  static bool logged_ms = false;
  if (!logged_ms) {
    logged_ms = true;
    Log(L"widget", L"GetBestRoute2 took %.2f ms GetIfEntry2 took %.2f ms", route.route_ms, route.entry_ms);
    if (route.route_ms > 5.0 || route.entry_ms > 5.0) {
      Log(L"widget", L"route query over 5ms");
    }
  }
  if (route.route_err != NO_ERROR) {
    std::lock_guard lock(mu_);
    if (!logged_slow_if_) {
      logged_slow_if_ = true;
      Log(L"widget", L"GetBestRoute2 failed err=%lu", static_cast<unsigned long>(route.route_err));
    }
  }

  const WlanStatus wifi = QueryWlanStatus();
  static bool logged_wlan = false;
  if (!logged_wlan) {
    logged_wlan = true;
    Log(L"cc", L"wlan query took %.2f ms", wifi.ms);
    if (wifi.ms > 5.0) {
      Log(L"cc", L"wlan query over 5ms; prefetching on widget worker");
    }
  }
  bool publish = false;
  {
    std::lock_guard lock(mu_);
    last_wifi_on_ = wifi.radio || wifi.connected;
    last_wifi_name_ = wifi.connected ? wifi.name : std::wstring(L"연결 안 됨");
    last_eth_on_ = route.kind == NetKind::kEthernet;
    last_eth_name_ = route.alias;
    publish = settings_.network && sink_ != nullptr;
  }
  if (!publish) {
    return;
  }

  StatusItem item;
  item.id = kNetworkId;
  item.priority = kNetworkPriority;
  std::wstring tip;
  if (route.kind == NetKind::kEthernet) {
    SetVectorIcon(&item, VectorIcon::kEthernet, 1.0f, 0);
    item.state = StatusState::kOn;
    tip = L"이더넷 · ";
    tip += route.alias.empty() ? std::wstring(L"연결됨") : route.alias;
  } else if (route.kind == NetKind::kWifi) {
    SetVectorIcon(&item, VectorIcon::kWifi, 1.0f, 0);
    item.state = StatusState::kOn;
    tip = L"Wi-Fi · ";
    tip += wifi.connected ? wifi.name : std::wstring(L"연결 안 됨");
  } else {
    SetVectorIcon(&item, VectorIcon::kWifi, 0.0f, 0);
    item.state = StatusState::kOff;
    tip = L"연결 안 됨";
  }
  item.tooltip = Truncate(std::move(tip), kStatusPanelTextMaxChars);
  Publish(std::move(item));
}

void BuiltinWidgets::SampleDue(ULONGLONG now) {
  bool bat = false;
  bool cpu = false;
  bool volume = false;
  bool brightness = false;
  bool network = false;
  {
    std::lock_guard lock(mu_);
    if (!active_) {
      return;
    }
    bat = settings_.battery && battery_due_ <= now;
    cpu = settings_.cpu && cpu_due_ <= now;
    volume = (settings_.volume || settings_.control_center) && volume_due_ <= now;
    brightness = settings_.control_center && brightness_due_ <= now;
    network = (settings_.network || settings_.control_center) && network_due_ <= now;
  }
  if (bat) {
    SampleBattery();
  }
  if (cpu) {
    SampleCpu();
  }
  if (volume) {
    SampleVolume();
  }
  if (brightness) {
    SampleBrightness();
  }
  if (network) {
    SampleNetwork();
  }
}

void BuiltinWidgets::WorkerLoop() {
  const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Log(L"widget", L"worker start");
  HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);

  for (;;) {
    if (stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
      break;
    }

    std::vector<PendingAction> acts;
    std::optional<float> level;
    std::optional<bool> mute;
    std::optional<float> bright;
    bool do_reset = false;
    bool do_power = false;
    WidgetSettings s{};
    bool active = false;
    bool volume_notify = false;
    LONGLONG notify_qpc = 0;
    {
      std::lock_guard lock(mu_);
      acts.swap(actions_);
      if (pending_level_) {
        last_volume_ = *pending_level_;
      }
      level.swap(pending_level_);
      mute.swap(pending_mute_);
      bright.swap(pending_brightness_);
      do_reset = reset_pending_;
      reset_pending_ = false;
      do_power = power_pending_;
      power_pending_ = false;
      s = settings_;
      active = active_;
      if (volume_->TakeNotifyDirty()) {
        volume_due_ = 0;
        volume_notify = true;
        notify_qpc = volume_->TakeNotifyQpc();
      }
    }
    if (volume_notify && notify_qpc != 0 && !logged_notify_latency_) {
      LARGE_INTEGER freq{};
      LARGE_INTEGER now_qpc{};
      QueryPerformanceFrequency(&freq);
      QueryPerformanceCounter(&now_qpc);
      double ms = 0.0;
      if (freq.QuadPart != 0) {
        ms = static_cast<double>(now_qpc.QuadPart - notify_qpc) * 1000.0 / static_cast<double>(freq.QuadPart);
      }
      logged_notify_latency_ = true;
      Log(L"widget", L"volume notify -> sample %.2f ms", ms);
    }

    for (const PendingAction action : acts) {
      Execute(action);
    }
    bool volume_changed = false;
    if (level) {
      volume_->SetLevel(*level);
      volume_changed = true;
    }
    if (mute) {
      volume_->SetMute(*mute);
      volume_changed = true;
    }
    if (bright) {
      const unsigned pct = static_cast<unsigned>(ClampUnit(*bright) * 100.0f + 0.5f);
      BrightnessBackend backend = BrightnessBackend::kNone;
      {
        std::lock_guard lock(mu_);
        backend = brightness_backend_;
        last_brightness_ = ClampUnit(*bright);
      }
      if (backend == BrightnessBackend::kDdcci) {
        SetDdcciBrightness(pct);
      } else if (backend == BrightnessBackend::kWmi) {
        SetWmiBrightness(pct);
      }
    }
    if (do_reset) {
      ResetBaselines();
    }
    if (s.widget_board && WidgetBoardAvailable()) {
      PublishBoard();
    } else if (s.widget_board) {
      DropItem(kBoardId);
    }
    if (!s.volume && !s.control_center) {
      volume_->Release();
    }
    if (active) {
      if (do_power || do_reset) {
        if (s.battery) {
          SampleBattery();
        }
      }
      if (volume_changed && (s.volume || s.control_center)) {
        SampleVolume();
      }
      SampleDue(GetTickCount64());
    }

    const ULONGLONG now = GetTickCount64();
    bool has_due = false;
    ULONGLONG due = 0;
    uint32_t flush_ms = 0xFFFFFFFFu;
    StatusSink* sink = nullptr;
    {
      std::lock_guard lock(mu_);
      sink = sink_;
      if (active_ && HasSampleDeadlineLocked()) {
        has_due = true;
        due = NextDeadlineLocked(now);
      }
      if (sink_ != nullptr) {
        flush_ms = sink_->NotifyWaitTimeoutMs();
      }
    }

    HANDLE waits[3]{};
    DWORD n = 0;
    waits[n++] = stop_event_;
    waits[n++] = wake_event_;
    if (has_due && timer != nullptr) {
      const ULONGLONG delay = due > now ? due - now : 0;
      LARGE_INTEGER rel{};
      rel.QuadPart = delay == 0 ? -1LL : -static_cast<LONGLONG>(delay * 10000ull);
      if (SetWaitableTimerEx(timer, &rel, 0, nullptr, nullptr, nullptr, 200)) {
        waits[n++] = timer;
      }
    }

    const DWORD wait = WaitForMultipleObjects(n, waits, FALSE, flush_ms);
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
      Log(L"widget", L"wait failed err=%lu", GetLastError());
      Sleep(50);
    }
  }

  if (timer != nullptr) {
    CloseHandle(timer);
  }
  volume_->Release();
  if (SUCCEEDED(co)) {
    CoUninitialize();
  }
  Log(L"widget", L"worker stop");
}

}  // namespace bamti
