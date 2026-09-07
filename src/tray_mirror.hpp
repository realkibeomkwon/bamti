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
#include <unordered_set>
#include <utility>
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

  struct MenuItem {
    uint64_t key = 0;
    std::wstring label;
    bool shown = true;
  };

  WidgetSettings settings() const;
  void SetSettings(const WidgetSettings& next);
  void OnExplorerRestart();
  void SetRectLookup(std::function<bool(uint64_t key, RECT* screen)> lookup);
  bool ForwardsContextMenu() const;
  DWORD OwnerPid(uint64_t key) const;  // 모르면 0
  // exe 경로에 대응하는 트레이 항목을 클릭한 것처럼 동작시킨다.
  // 대응 항목이 없으면 거짓을 돌려주고 아무것도 하지 않는다.
  bool InvokeByExe(const std::wstring& exe_path);
  std::vector<MenuItem> MenuItems() const;
  static uint64_t ParseId(const std::string& id);
  static std::string KeyText(uint64_t key);

 private:
  struct LastTip {
    std::wstring tip;
    GUID guid{};
  };

  struct ItemState {
    uint64_t key = 0;
    std::wstring tip;
    GUID guid{};
    int order = 0;
    bool visible = true;
    uint64_t icon_hash = 0;
    std::string id;
    DWORD owner_pid = 0;
  };

  struct FillOwner {
    std::wstring exe_path;
    HWND owner = nullptr;
    DWORD pid = 0;
  };

  void WorkerLoop();
  void StartWorkerLocked();
  void StopWorker();
  void StartIntercept();
  void StopIntercept();
  void DoRound(TrayBackend* backend, bool events_live, TrayBackend* fill_uia, bool refresh_fill);
  void RememberFillOwners(const std::vector<TrayIconInfo>& intercept);
  void PruneFillOwners();
  bool FillPngForExe(const std::wstring& exe_path, std::vector<uint8_t>* out);
  void RefreshUiaFill(TrayBackend* uia, const std::vector<TrayIconInfo>& intercept);
  void MergeUiaFill(std::vector<TrayIconInfo>* raw);
  void Publish(const TrayIconInfo& icon, int order);
  void DropAll();
  bool Include(const TrayIconInfo& icon, int overflow_order, const WidgetSettings& settings) const;
  void DrainInvoke(TrayBackend* backend, TrayBackend* fill_uia);
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
  std::unordered_map<uint64_t, LastTip> last_tips_;
  std::vector<uint64_t> key_rounds_[5];
  int key_round_n_ = 0;
  bool use_runtime_id_ = true;
  ULONGLONG last_enum_ms_ = 0;
  UINT interval_ms_ = 1000;
  int slow_streak_ = 0;
  ULONGLONG last_perf_log_ = 0;
  uint64_t pending_invoke_ = 0;
  bool pending_right_ = false;
  bool pending_dblclk_ = false;
  std::unique_ptr<TrayBackend> intercept_;
  std::function<bool(uint64_t, RECT*)> rect_lookup_;
  std::vector<TrayIconInfo> fill_icons_;
  std::unordered_set<uint64_t> fill_keys_;
  std::unordered_set<uint64_t> fill_logged_;
  std::unordered_map<std::wstring, FillOwner> fill_owners_;
  std::unordered_map<HWND, std::pair<DWORD, std::wstring>> fill_exe_by_owner_;
  std::unordered_map<HWND, std::wstring> fill_tip_by_owner_;
  std::unordered_map<std::wstring, std::vector<uint8_t>> fill_png_cache_;
  std::unordered_set<uint64_t> fill_skip_unknown_;
  bool fill_detail_logged_ = false;
};

}  // namespace bamti
