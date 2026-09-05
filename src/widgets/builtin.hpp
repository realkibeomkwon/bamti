#pragma once

#include "control_center.hpp"
#include "settings.hpp"
#include "status_source.hpp"
#include "widgets/brightness.hpp"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace bamti {

class VolumeControl;

class BuiltinWidgets : public StatusSource {
 public:
  BuiltinWidgets();
  BuiltinWidgets(const BuiltinWidgets&) = delete;
  BuiltinWidgets& operator=(const BuiltinWidgets&) = delete;
  ~BuiltinWidgets() override;

  const char* Name() const override;
  bool Start(StatusSink* sink) override;
  void Stop() override;
  void OnEvent(const StatusEvent& ev) override;
  void SetActive(bool active) override;

  WidgetSettings settings() const;
  void SetSettings(const WidgetSettings& next);
  void NotePowerEvent(bool resumed);
  ControlCenterLive LiveForControlCenter() const;
  std::vector<BtDeviceInfo> BtScanResult() const;
  void RequestBtConnect(std::wstring address, bool connect);

 private:
  enum class PendingAction {
    kPowerSettings,
    kNetworkSettings,
    kWifiSettings,
    kTaskManager,
    kWidgetBoard,
    kSoundSettings
  };

  void WorkerLoop();
  void StartWorkerLocked();
  void StopWorker();
  void ResetBaselines();
  void SubmitSave();
  void DrainSaves();
  static VOID CALLBACK SaveSettingsCallback(PTP_CALLBACK_INSTANCE instance, PVOID ctx);
  void SampleDue(ULONGLONG now);
  void SampleBattery();
  void SampleCpu();
  void SampleVolume();
  void SampleBrightness();
  void SampleNetwork();
  void SampleBluetooth();
  void PublishBoard();
  void Publish(StatusItem item);
  void DropItem(const char* id);
  void Execute(PendingAction action);
  bool HasSampleDeadlineLocked() const;
  ULONGLONG NextDeadlineLocked(ULONGLONG now) const;

  StatusSink* sink_ = nullptr;
  mutable std::mutex mu_;
  WidgetSettings settings_{};
  std::thread worker_;
  HANDLE stop_event_ = nullptr;
  HANDLE wake_event_ = nullptr;
  HANDLE save_idle_event_ = nullptr;
  WidgetSettings pending_save_{};
  uint64_t save_gen_ = 0;
  bool save_busy_ = false;
  bool active_ = true;
  bool reset_pending_ = true;
  bool power_pending_ = false;
  std::vector<PendingAction> actions_;
  std::optional<float> pending_level_;
  std::optional<bool> pending_mute_;
  std::optional<float> pending_brightness_;
  std::optional<bool> pending_bt_on_;
  std::optional<bool> pending_saver_on_;
  bool pending_volume_device_ = false;
  bool pending_bt_scan_ = false;
  bool bt_scanning_ = false;
  uint64_t bt_scan_rev_ = 0;
  std::vector<BtDeviceInfo> bt_scan_result_;
  bool bt_scan_discard_ = false;
  struct BtConnectReq {
    std::wstring address;
    bool connect = false;
  };
  std::vector<BtConnectReq> bt_connect_reqs_;
  std::wstring bt_connecting_addr_;
  uint64_t bt_list_rev_ = 0;
  uint64_t bt_connect_fail_rev_ = 0;
  std::map<std::wstring, std::vector<GUID>> bt_disabled_services_;
  std::unique_ptr<VolumeControl> volume_;
  bool logged_notify_latency_ = false;
  ULONGLONG battery_due_ = 0;
  ULONGLONG cpu_due_ = 0;
  ULONGLONG volume_due_ = 0;
  ULONGLONG volume_refresh_due_ = 0;
  ULONGLONG brightness_due_ = 0;
  ULONGLONG network_due_ = 0;
  ULONGLONG bluetooth_due_ = 0;
  BrightnessBackend brightness_backend_ = BrightnessBackend::kNone;
  bool brightness_probed_ = false;
  bool last_volume_ok_ = false;
  float last_volume_ = 0.0f;
  bool last_muted_ = false;
  bool last_brightness_ok_ = false;
  float last_brightness_ = 0.0f;
  bool last_wifi_on_ = false;
  std::wstring last_wifi_name_ = L"연결 안 됨";
  bool last_eth_on_ = false;
  std::wstring last_eth_name_;
  bool last_bt_present_ = false;
  bool last_bt_on_ = false;
  bool last_bt_can_toggle_ = false;
  std::wstring last_bt_name_;
  bool last_battery_ok_ = false;
  float last_battery_level_ = 0.0f;
  bool last_battery_ac_ = false;
  bool last_battery_charging_ = false;
  std::wstring last_battery_remain_;
  bool last_battery_saver_on_ = false;
  bool last_cpu_ok_ = false;
  float last_cpu_usage_ = 0.0f;
  float last_cpu_user_ = 0.0f;
  float last_cpu_kernel_ = 0.0f;
  unsigned last_cpu_nproc_ = 0;
  bool cpu_has_baseline_ = false;
  uint64_t cpu_idle_ = 0;
  uint64_t cpu_kernel_ = 0;
  uint64_t cpu_user_ = 0;
  std::wstring fp_battery_;
  std::wstring fp_cpu_;
  std::wstring fp_volume_;
  std::wstring fp_network_;
  std::wstring fp_bluetooth_;
  std::wstring fp_board_;
  bool logged_no_battery_ = false;
  bool logged_no_volume_ = false;
  bool logged_slow_if_ = false;
};

bool IsWidgetBoardAvailable();

}  // namespace bamti
