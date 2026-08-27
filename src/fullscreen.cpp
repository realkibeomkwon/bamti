#include "fullscreen.hpp"

#include <dwmapi.h>
#include <shellapi.h>

#include <cstdlib>

namespace bamti {
namespace {

bool RectNearlyEqual(const RECT& a, const RECT& b) {
  return std::abs(a.left - b.left) <= 2 && std::abs(a.top - b.top) <= 2 &&
         std::abs(a.right - b.right) <= 2 && std::abs(a.bottom - b.bottom) <= 2;
}

bool IsShellOverlayClass(HWND hwnd) {
  wchar_t cls[256]{};
  GetClassNameW(hwnd, cls, 256);
  return lstrcmpiW(cls, L"Windows.UI.Core.CoreWindow") == 0 ||
         lstrcmpiW(cls, L"Xaml_WindowedPopupClass") == 0 ||
         lstrcmpiW(cls, L"ForegroundStaging") == 0;
}

}  // namespace

// Maximized apps (Windows Terminal included) are not fullscreen. They often
// ignore the work area and cover the monitor; overlays stay TOPMOST instead.
bool IsTrueFullscreen(HWND self) {
  QUERY_USER_NOTIFICATION_STATE state{};
  if (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN) {
    return true;
  }

  const HWND fg = GetForegroundWindow();
  if (fg == nullptr || fg == self) {
    return false;
  }
  if (fg == GetDesktopWindow() || fg == GetShellWindow()) {
    return false;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == GetCurrentProcessId()) {
    return false;
  }

  if (IsShellOverlayClass(fg)) {
    return false;
  }

  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
    return false;
  }

  const LONG style = GetWindowLongW(fg, GWL_STYLE);
  if ((style & WS_MAXIMIZE) != 0) {
    return false;
  }

  WINDOWPLACEMENT place{};
  place.length = sizeof(place);
  if (GetWindowPlacement(fg, &place) && place.showCmd == SW_SHOWMAXIMIZED) {
    return false;
  }

  RECT wr{};
  if (!GetWindowRect(fg, &wr)) {
    return false;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  if (!GetMonitorInfoW(MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST), &mi)) {
    return false;
  }
  if (!RectNearlyEqual(wr, mi.rcMonitor)) {
    return false;
  }

  const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
  if ((ex & WS_EX_NOACTIVATE) != 0 || (ex & WS_EX_TOOLWINDOW) != 0) {
    return false;
  }

  const bool captioned = (style & WS_CAPTION) == WS_CAPTION;
  const bool overlapped = (style & WS_OVERLAPPEDWINDOW) == WS_OVERLAPPEDWINDOW;
  return !captioned && !overlapped;
}

}  // namespace bamti
