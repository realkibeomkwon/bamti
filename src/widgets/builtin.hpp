#pragma once

#include "settings.hpp"
#include "status_source.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace bamti {

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

 private:
  enum class PendingAction { kPowerSettings, kNetworkSettings, kTaskManager, kWidgetBoard };

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
  void SampleNet();
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
  ULONGLONG battery_due_ = 0;
  ULONGLONG cpu_due_ = 0;
  ULONGLONG net_due_ = 0;
  bool cpu_has_baseline_ = false;
  bool net_has_baseline_ = false;
  uint64_t cpu_idle_ = 0;
  uint64_t cpu_kernel_ = 0;
  uint64_t cpu_user_ = 0;
  uint64_t net_in_ = 0;
  uint64_t net_out_ = 0;
  ULONGLONG net_tick_ = 0;
  uint64_t session_in_ = 0;
  uint64_t session_out_ = 0;
  std::wstring fp_battery_;
  std::wstring fp_cpu_;
  std::wstring fp_net_;
  std::wstring fp_board_;
  bool logged_no_battery_ = false;
  bool logged_slow_if_ = false;
};

}  // namespace bamti
