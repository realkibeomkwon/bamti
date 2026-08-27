#include "fullscreen.hpp"

#include <dwmapi.h>
#include <shellapi.h>

#include <cstdlib>

namespace bamti {
namespace {

bool RectCovers(const RECT& window, const RECT& monitor, int slop) {
  return window.left <= monitor.left + slop && window.top <= monitor.top + slop &&
         window.right >= monitor.right - slop && window.bottom >= monitor.bottom - slop;
}

int FrameSlopPx(HWND hwnd) {
  UINT dpi = GetDpiForWindow(hwnd);
  if (dpi == 0) {
    dpi = 96;
  }
  const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi);
  const int pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  const int slop = frame + pad;
  return slop < 8 ? 8 : slop;
}

bool IsShellForegroundClass(HWND hwnd) {
  wchar_t cls[256]{};
  GetClassNameW(hwnd, cls, 256);
  if (cls[0] != L'\0' && _wcsnicmp(cls, L"bamti.", 6) == 0) {
    return true;
  }
  static const wchar_t* kSkip[] = {
      L"Progman",
      L"WorkerW",
      L"Shell_TrayWnd",
      L"Shell_SecondaryTrayWnd",
      L"NotifyIconOverflowWindow",
      L"ForegroundStaging",
      L"Windows.UI.Core.CoreWindow",
      L"Xaml_WindowedPopupClass",
  };
  for (const wchar_t* skip : kSkip) {
    if (lstrcmpiW(cls, skip) == 0) {
      return true;
    }
  }
  return false;
}

bool HasCaptionFrame(LONG style) {
  return (style & WS_CAPTION) == WS_CAPTION;
}

}  // namespace

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

  if (!IsWindowVisible(fg) || IsIconic(fg)) {
    return false;
  }
  if (IsShellForegroundClass(fg)) {
    return false;
  }

  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
    return false;
  }

  const LONG style = GetWindowLongW(fg, GWL_STYLE);
  if ((style & WS_MINIMIZE) != 0) {
    return false;
  }

  RECT wr{};
  if (!GetWindowRect(fg, &wr)) {
    return false;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  const HMONITOR monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(monitor, &mi)) {
    return false;
  }
  if (!RectCovers(wr, mi.rcMonitor, FrameSlopPx(fg))) {
    return false;
  }

  const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
  if ((ex & WS_EX_NOACTIVATE) != 0 || (ex & WS_EX_TOOLWINDOW) != 0) {
    return false;
  }

  // Captioned maximized windows that ignore the work area (Windows Terminal) keep
  // the overlay. Chrome HTML5 fullscreen strips WS_CAPTION but often keeps
  // WS_MAXIMIZE when the browser was already maximized.
  return !HasCaptionFrame(style);
}

}  // namespace bamti
