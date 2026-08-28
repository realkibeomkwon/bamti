#pragma once

#include "bar_layout.hpp"
#include "clock_renderer.hpp"
#include "pipe_server.hpp"
#include "popup_surface.hpp"
#include "spotlight.hpp"
#include "start_menu.hpp"
#include "status_item.hpp"
#include "status_panel.hpp"
#include "status_registry.hpp"
#include "taskbar_controller.hpp"
#include "widgets/builtin.hpp"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bamti {

inline constexpr wchar_t kMenuBarClass[] = L"bamti.MenuBar";

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
  void RefreshFullscreenState();
  void SetFullscreenOccluded(bool occluded);
  bool CreateTooltip();
  RECT StartRect() const;
  RECT ClockRect() const;
  std::optional<StatusHit> HitTest(POINT client) const;
  bool HitStart(POINT client) const;
  void UpdateStartChrome(POINT client);
  void ArmMouseLeave();
  void ToggleStartMenu(bool from_keyboard = false);
  void ToggleSpotlight();
  void OpenStatusPanel(const StatusHit& hit);
  void OpenOverflow();
  const BarSegment* HitSegment(POINT client) const;
  void ArmToggle(std::string id, std::string row_id, uint64_t revision, bool on);
  void OnToggleTimeout();
  bool InstallWinHook();
  void RemoveWinHook();
  UINT Dpi() const;
  int BarHeightPx() const;

  HWND hwnd_ = nullptr;
  HWND tooltip_ = nullptr;
  bool appbar_registered_ = false;
  bool fullscreen_occluded_ = false;
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
  PerfAcc perf_bind_;
  PerfAcc perf_brush_;
  PerfAcc perf_begin_;
  PerfAcc perf_draw_;
  PerfAcc perf_end_;
  PerfAcc perf_bpbegin_;
  PerfAcc perf_bpend_;
  BarLayout layout_;
  ClockRenderer clock_;
  PipeServer pipe_;
  BuiltinWidgets widgets_;
  StatusRegistry status_;
  TaskbarController taskbar_;
  StartMenu start_menu_;
  Spotlight spotlight_;
  PopupSurface status_popup_;
  std::unique_ptr<StatusPanelContent> status_panel_;
  std::unique_ptr<OverflowContent> overflow_panel_;
  std::wstring tooltip_text_;
  std::string open_panel_id_;
  struct PendingToggle {
    std::string id;
    std::string row_id;
    uint64_t revision = 0;
    bool on = false;
  };
  PendingToggle pending_toggle_{};
  bool toggle_armed_ = false;
};

}  // namespace bamti
