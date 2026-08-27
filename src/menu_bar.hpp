#pragma once

#include "clock_renderer.hpp"
#include "pipe_server.hpp"
#include "status_item.hpp"
#include "taskbar_controller.hpp"

#include <windows.h>

#include <vector>

namespace bamti {

inline constexpr wchar_t kMenuBarClass[] = L"bamti.MenuBar";

class MenuBar {
 public:
  MenuBar() = default;
  MenuBar(const MenuBar&) = delete;
  MenuBar& operator=(const MenuBar&) = delete;
  ~MenuBar();

  bool Create(HINSTANCE instance);
  HWND hwnd() const { return hwnd_; }
  bool taskbar_hidden() const { return taskbar_.hidden(); }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);

  bool RegisterAppBar();
  void UnregisterAppBar();
  void Layout();
  void ApplyBackdrop();
  void Paint();
  void ShowContextMenu(POINT screen);
  void RefreshFullscreenState();
  void SetFullscreenOccluded(bool occluded);
  bool CreateTooltip();
  const StatusHit* HitTest(POINT client) const;
  bool HitStart(POINT client) const;
  void UpdateStartChrome(POINT client);
  void ArmMouseLeave();
  static void ToggleStartMenu();
  UINT Dpi() const;
  int BarHeightPx() const;

  HWND hwnd_ = nullptr;
  HWND tooltip_ = nullptr;
  bool appbar_registered_ = false;
  bool fullscreen_occluded_ = false;
  bool dark_ = true;
  bool start_hot_ = false;
  bool start_pressed_ = false;
  RECT start_rect_{};
  ClockRenderer clock_;
  PipeServer status_;
  TaskbarController taskbar_;
  std::vector<StatusHit> hits_;
  std::wstring tooltip_text_;
};

}  // namespace bamti
