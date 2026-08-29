#pragma once

#include "settings.hpp"
#include "status_source.hpp"
#include "tray_backend.hpp"

#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace bamti {

class TrayMirror : public StatusSource {
 public:
  TrayMirror();
  TrayMirror(const TrayMirror&) = delete;
  TrayMirror& operator=(const TrayMirror&) = delete;
  ~TrayMirror() override;

  const char* Name() const override;
  bool Start(StatusSink* sink) override;
  void Stop() override;
  void SetActive(bool active) override;

  WidgetSettings settings() const;
  void SetSettings(const WidgetSettings& next);
  void OnExplorerRestart();

 private:
  struct ItemState {
    uint64_t key = 0;
    std::wstring tip;
    int order = 0;
    bool visible = true;
    std::string id;
  };

  void WorkerLoop();
  void StartWorkerLocked();
  void StopWorker();
  void DoRound(TrayBackend* backend);
  void Publish(const TrayIconInfo& icon, int order);
  void DropAll();
  bool Include(const TrayIconInfo& icon, int overflow_order, bool system_icons) const;
  static std::wstring FirstGlyph(const std::wstring& tip);
  static std::string MakeId(uint64_t key);

  StatusSink* sink_ = nullptr;
  mutable std::mutex mu_;
  WidgetSettings settings_{};
  std::thread worker_;
  HANDLE stop_event_ = nullptr;
  HANDLE wake_event_ = nullptr;
  bool active_ = true;
  bool reset_pending_ = false;
  bool stopped_slow_ = false;
  std::unordered_map<uint64_t, ItemState> items_;
  std::vector<uint64_t> key_rounds_[5];
  int key_round_n_ = 0;
  bool use_runtime_id_ = true;
  ULONGLONG last_enum_ms_ = 0;
  UINT interval_ms_ = 1000;
  int slow_streak_ = 0;
  ULONGLONG last_perf_log_ = 0;
};

}  // namespace bamti
