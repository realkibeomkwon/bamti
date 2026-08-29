#pragma once

#include "settings.hpp"
#include "status_source.hpp"
#include "tray_backend.hpp"

#include <cstdint>
#include <functional>
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
  void OnEvent(const StatusEvent& ev) override;
  void SetActive(bool active) override;

  WidgetSettings settings() const;
  void SetSettings(const WidgetSettings& next);
  void OnExplorerRestart();
  void SetRectLookup(std::function<bool(uint64_t key, RECT* screen)> lookup);
  bool ForwardsContextMenu() const;
  static uint64_t ParseId(const std::string& id);
  static std::string KeyText(uint64_t key);

 private:
  struct ItemState {
    uint64_t key = 0;
    std::wstring tip;
    int order = 0;
    bool visible = true;
    uint64_t icon_hash = 0;
    std::string id;
  };

  void WorkerLoop();
  void StartWorkerLocked();
  void StopWorker();
  void StartIntercept();
  void StopIntercept();
  void DoRound(TrayBackend* backend, bool events_live);
  void Publish(const TrayIconInfo& icon, int order);
  void DropAll();
  bool Include(const TrayIconInfo& icon, int overflow_order, const WidgetSettings& settings) const;
  void DrainInvoke(TrayBackend* backend);
  static std::wstring FirstGlyph(const std::wstring& tip);
  static std::string MakeId(uint64_t key);
  static bool KeyHidden(uint64_t key, const std::vector<std::string>& hidden);

  StatusSink* sink_ = nullptr;
  mutable std::mutex mu_;
  WidgetSettings settings_{};
  std::thread worker_;
  HANDLE stop_event_ = nullptr;
  HANDLE wake_event_ = nullptr;
  HANDLE struct_event_ = nullptr;
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
  uint64_t pending_invoke_ = 0;
  bool pending_right_ = false;
  std::unique_ptr<TrayBackend> intercept_;
  std::function<bool(uint64_t, RECT*)> rect_lookup_;
};

}  // namespace bamti
