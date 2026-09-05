#pragma once

#include "bar_layout.hpp"
#include "bar_menu.hpp"
#include "clock_flyout.hpp"
#include "clock_renderer.hpp"
#include "pipe_server.hpp"
#include "popup_surface.hpp"
#include "spotlight.hpp"
#include "start_menu.hpp"
#include "status_item.hpp"
#include "status_panel.hpp"
#include "status_registry.hpp"
#include "control_center.hpp"
#include "taskbar_controller.hpp"
#include "tray_mirror.hpp"
#include "widgets/builtin.hpp"
#include "winx_menu.hpp"
#include "live_preview.hpp"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bamti {

inline constexpr wchar_t kMenuBarClass[] = L"bamti.MenuBar";
inline constexpr UINT kToggleSpotlightMsg = WM_APP + 8;

class MenuBar {
 public:
  MenuBar();
  MenuBar(const MenuBar&) = delete;
  MenuBar& operator=(const MenuBar&) = delete;
  ~MenuBar();

  bool Create(HINSTANCE instance);
  HWND hwnd() const { return hwnd_; }
  bool taskbar_hidden() const { return taskbar_.hidden(); }
  bool win_key_enabled() const { return !fullscreen_occluded_; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

  bool RegisterAppBar();
  void UnregisterAppBar();
  void Layout();
  void ApplyBackdrop();
  void Paint();
  void RefreshLayout();
  void ArmRepaint();
  void NotePerf(double compute_ms, double draw_ms, const RECT& dirty, const RECT& client, const DrawTimings& draw,
                double bpbegin_ms, double bpend_ms);
  void ShowContextMenu(POINT screen);
  void ShowStartContextMenu(POINT screen);
  void ShowTrayIconMenu(POINT screen, const std::string& id);
  void SyncBarSubmenu();
  void OpenBarSubmenu(UINT cmd);
  void CloseBarSubmenu(const wchar_t* reason);
  static void AfterBarPopupTick(void* ctx);
  void ApplySettings(const WidgetSettings& next);
  void StartTrayPeek();
  void EndTrayPeek();
  bool PeekZoneHit(POINT client) const;
  bool DesktopPeekWanted(POINT client) const;
  void UpdateDesktopPeek(POINT client, bool ctrl_down);
  void StartDesktopPeek();
  void StopDesktopPeek();
  void RefreshFullscreenState();
  void SetFullscreenOccluded(bool occluded);
  void UpdateProviderActive();
  void UnregisterSessionWatch();
  bool CreateTooltip();
  RECT StartRect() const;
  RECT SpotlightRect() const;
  RECT ControlCenterRect() const;
  RECT ClockRect() const;
  bool HitClock(POINT client) const;
  std::optional<StatusHit> HitTest(POINT client) const;
  bool HitStart(POINT client) const;
  void UpdateChrome(POINT client);
  void ArmMouseLeave();
  void ToggleStartMenu(bool from_keyboard = false);
  void ToggleSpotlight();
  void ToggleControlCenter();
  void ToggleClockFlyout();
  bool ShowClockFlyout();
  void ShowClockMenu();
  void ToggleWidgetPage(const StatusHit& hit);
  bool ShowControlCenter(const RECT& item_rect, ControlCenterPage page);
  void OpenStatusPanel(const StatusHit& hit);
  void OpenOverflow();
  void RefreshOpenPanel();
  StatusPanelHost MakePanelHost();
  std::vector<StatusItem> OrderedItems() const;
  void BeginReorder(const std::string& id, POINT pt);
  bool UpdateReorder(POINT pt);
  void CancelReorder();
  void FinishReorder();
  bool ReorderCursor(POINT client) const;
  const BarSegment* HitSegment(POINT client) const;
  bool SegmentScreenRect(const std::string& id, RECT* out) const;
  void ArmToggle(std::string id, std::string row_id, uint64_t revision, bool on);
  void ArmSlider(std::string id, std::string row_id, uint64_t revision, float value);
  void OnToggleTimeout();
  bool InstallWinHook();
  void RemoveWinHook();
  UINT Dpi() const;
  int BarHeightPx() const;

  HWND hwnd_ = nullptr;
  HWND tooltip_ = nullptr;
  bool appbar_registered_ = false;
  bool fullscreen_occluded_ = false;
  bool session_locked_ = false;
  bool display_on_ = true;
  bool providers_active_ = true;
  bool session_notify_ = false;
  HPOWERNOTIFY display_notify_ = nullptr;
  bool dark_ = true;
  bool start_hot_ = false;
  bool start_pressed_ = false;
  bool repaint_armed_ = false;
  struct PerfAcc {
    unsigned n = 0;
    double sum = 0;
    double maxv = 0;
    void Add(double v) {
      ++n;
      sum += v;
      if (n == 1 || v > maxv) {
        maxv = v;
      }
    }
    void Reset() {
      n = 0;
      sum = 0;
      maxv = 0;
    }
  };
  unsigned perf_frames_ = 0;
  bool perf_cold_ = true;
  double last_compute_ms_ = 0.0;
  PerfAcc perf_full_;
  PerfAcc perf_seg_;
  PerfAcc perf_compute_;
  PerfAcc perf_create_;
  PerfAcc perf_bind_;
  PerfAcc perf_brush_;
  PerfAcc perf_begin_;
  PerfAcc perf_draw_;
  PerfAcc perf_end_;
  PerfAcc perf_bpbegin_;
  PerfAcc perf_bpend_;
  PerfAcc perf_other_;
  BarLayout layout_;
  ClockRenderer clock_;
  PipeServer pipe_;
  TrayMirror tray_;
  BuiltinWidgets widgets_;
  StatusRegistry status_;
  TaskbarController taskbar_;
  StartMenu start_menu_;
  Spotlight spotlight_;
  PopupSurface status_popup_;
  std::unique_ptr<BarMenuContent> bar_menu_;
  std::unique_ptr<BarMenuContent> bar_submenu_;
  PopupSurface bar_submenu_popup_;
  std::unique_ptr<StatusPanelContent> status_panel_;
  std::unique_ptr<OverflowContent> overflow_panel_;
  std::unique_ptr<ControlCenterContent> cc_panel_;
  std::unique_ptr<ClockFlyoutContent> clock_panel_;
  std::unique_ptr<ClockMenuContent> clock_menu_;
  bool cc_open_ = false;
  bool clock_open_ = false;
  std::wstring tooltip_text_;
  std::string open_panel_id_;
  uint64_t open_panel_revision_ = 0;
  std::vector<RowType> open_panel_rows_;
  struct PendingToggle {
    std::string id;
    std::string row_id;
    uint64_t revision = 0;
    bool on = false;
  };
  PendingToggle pending_toggle_{};
  bool toggle_armed_ = false;
  struct PendingSlider {
    std::string id;
    std::string row_id;
    uint64_t revision = 0;
    float value = 0.0f;
  };
  PendingSlider pending_slider_{};
  bool slider_armed_ = false;
  bool tray_peeking_ = false;
  bool skip_left_up_ = false;
  bool reorder_active_ = false;
  bool reorder_moved_ = false;
  std::string reorder_id_;
  POINT reorder_start_{};
  std::vector<std::string> bar_order_;
  std::vector<std::string> reorder_order_;
  std::string tray_menu_id_;
  std::vector<uint64_t> tray_menu_keys_;
  std::vector<WinXEntry> winx_entries_;
  UINT open_submenu_cmd_ = 0;
  bool peek_active_ = false;
  bool peek_dwell_armed_ = false;
  POINT peek_pt_{};
  bool peek_ctrl_ = false;
  int wheel_accum_ = 0;
};

}  // namespace bamti
