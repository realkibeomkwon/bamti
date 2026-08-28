#pragma once

#include "bar_layout.hpp"
#include "clock_renderer.hpp"
#include "pipe_server.hpp"
#include "popup_surface.hpp"
#include "spotlight.hpp"
#include "start_menu.hpp"
#include "status_item.hpp"
#include "taskbar_controller.hpp"

#include <windows.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace bamti {

inline constexpr wchar_t kMenuBarClass[] = L"bamti.MenuBar";

class StatusPanelContent;

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
  bool InstallWinHook();
  void RemoveWinHook();
  UINT Dpi() const;
  int BarHeightPx() const;

  friend class StatusPanelContent;

  HWND hwnd_ = nullptr;
  HWND tooltip_ = nullptr;
  bool appbar_registered_ = false;
  bool fullscreen_occluded_ = false;
  bool dark_ = true;
  bool start_hot_ = false;
  bool start_pressed_ = false;
  std::wstring last_clock_text_;
  BarLayout layout_;
  ClockRenderer clock_;
  PipeServer status_;
  TaskbarController taskbar_;
  StartMenu start_menu_;
  Spotlight spotlight_;
  PopupSurface status_popup_;
  std::unique_ptr<StatusPanelContent> status_panel_;
  std::wstring tooltip_text_;
};

}  // namespace bamti
