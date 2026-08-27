#include "menu_bar.hpp"

#include "dwm.hpp"
#include "fullscreen.hpp"
#include "theme.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace bamti {
namespace {

constexpr UINT kAppBarCallback = WM_APP + 1;
constexpr UINT_PTR kClockTimerId = 1;
constexpr int kBarHeightDip = 32;
constexpr UINT kExitCommand = 1;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

}  // namespace

MenuBar::~MenuBar() {
  status_.Stop();
  if (hwnd_) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

bool MenuBar::Create(HINSTANCE instance) {
  INITCOMMONCONTROLSEX icc{};
  icc.dwSize = sizeof(icc);
  icc.dwICC = ICC_WIN95_CLASSES;
  InitCommonControlsEx(&icc);

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kMenuBarClass;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  if (!clock_.Initialize()) {
    return false;
  }

  dark_ = ShellUsesDarkMode();

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kMenuBarClass, L"bamti", WS_POPUP, 0, 0,
                          0, 0, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }

  clock_.SetDpi(Dpi());
  ApplyBackdrop();
  if (!RegisterAppBar()) {
    return false;
  }
  CreateTooltip();
  status_.Start(hwnd_);
  Layout();
  taskbar_.Restore();
  ShowWindow(hwnd_, SW_SHOWNA);
  taskbar_.Hide();
  SetTimer(hwnd_, kClockTimerId, 1000, nullptr);
  RefreshFullscreenState();
  return true;
}

bool MenuBar::CreateTooltip() {
  tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0,
                             0, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (tooltip_ == nullptr) {
    return false;
  }
  SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 320);
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
  info.hwnd = hwnd_;
  info.uId = 1;
  GetClientRect(hwnd_, &info.rect);
  info.lpszText = LPSTR_TEXTCALLBACKW;
  SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  return true;
}

LRESULT CALLBACK MenuBar::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  MenuBar* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<MenuBar*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<MenuBar*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->HandleMessage(msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT MenuBar::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_TIMER:
      if (wparam == kClockTimerId) {
        status_.DropStale();
        RefreshFullscreenState();
        taskbar_.EnsureHidden();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case kStatusChangedMsg:
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_DPICHANGED:
      clock_.SetDpi(HIWORD(wparam));
      ApplyBackdrop();
      Layout();
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
      if (msg == WM_SETTINGCHANGE) {
        const wchar_t* area = reinterpret_cast<const wchar_t*>(lparam);
        if (area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0) {
          dark_ = ShellUsesDarkMode();
          ApplyBackdrop();
        }
      }
      Layout();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_WINDOWPOSCHANGED: {
      APPBARDATA abd{};
      abd.cbSize = sizeof(abd);
      abd.hWnd = hwnd_;
      SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);
      return 0;
    }
    case WM_ACTIVATE: {
      APPBARDATA abd{};
      abd.cbSize = sizeof(abd);
      abd.hWnd = hwnd_;
      SHAppBarMessage(ABM_ACTIVATE, &abd);
      return 0;
    }
    case WM_SETCURSOR: {
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (HitStart(pt) || HitTest(pt) != nullptr) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      break;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      UpdateStartChrome(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (start_hot_ || start_pressed_) {
        start_hot_ = false;
        start_pressed_ = false;
        InvalidateRect(hwnd_, &start_rect_, FALSE);
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (HitStart(pt)) {
        start_pressed_ = true;
        SetCapture(hwnd_);
        InvalidateRect(hwnd_, &start_rect_, FALSE);
        return 0;
      }
      break;
    }
    case WM_CAPTURECHANGED:
      if (start_pressed_) {
        start_pressed_ = false;
        InvalidateRect(hwnd_, &start_rect_, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool start_click = start_pressed_ && HitStart(pt);
      if (start_pressed_) {
        start_pressed_ = false;
        ReleaseCapture();
        InvalidateRect(hwnd_, &start_rect_, FALSE);
      }
      if (start_click) {
        ToggleStartMenu();
        return 0;
      }
      if (const StatusHit* hit = HitTest(pt)) {
        status_.SendClick(hit->id, "left");
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (const StatusHit* hit = HitTest(pt)) {
        status_.SendClick(hit->id, "right");
        return 0;
      }
      ClientToScreen(hwnd_, &pt);
      ShowContextMenu(pt);
      return 0;
    }
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->hwndFrom == tooltip_ && header->code == TTN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (HitStart(pt)) {
          tooltip_text_ = L"시작";
          info->lpszText = tooltip_text_.data();
        } else if (const StatusHit* hit = HitTest(pt)) {
          tooltip_text_ = hit->tooltip.empty() ? std::wstring(hit->id.begin(), hit->id.end()) : hit->tooltip;
          info->lpszText = tooltip_text_.data();
        } else {
          info->lpszText = const_cast<wchar_t*>(L"");
        }
      }
      return 0;
    }
    case WM_COMMAND:
      if (LOWORD(wparam) == kExitCommand) {
        DestroyWindow(hwnd_);
      }
      return 0;
    case kAppBarCallback:
      switch (wparam) {
        case ABN_POSCHANGED:
          Layout();
          break;
        case ABN_FULLSCREENAPP:
          RefreshFullscreenState();
          break;
        default:
          break;
      }
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wparam) {
        KillTimer(hwnd_, kClockTimerId);
        status_.Stop();
        taskbar_.Restore();
        UnregisterAppBar();
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kClockTimerId);
      status_.Stop();
      taskbar_.Restore();
      UnregisterAppBar();
      hwnd_ = nullptr;
      PostQuitMessage(0);
      return 0;
    default:
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

bool MenuBar::RegisterAppBar() {
  if (appbar_registered_) {
    return true;
  }
  APPBARDATA abd{};
  abd.cbSize = sizeof(abd);
  abd.hWnd = hwnd_;
  abd.uCallbackMessage = kAppBarCallback;
  if (SHAppBarMessage(ABM_NEW, &abd) == FALSE) {
    return false;
  }
  appbar_registered_ = true;
  return true;
}

void MenuBar::UnregisterAppBar() {
  if (!appbar_registered_ || hwnd_ == nullptr) {
    appbar_registered_ = false;
    return;
  }
  APPBARDATA abd{};
  abd.cbSize = sizeof(abd);
  abd.hWnd = hwnd_;
  SHAppBarMessage(ABM_REMOVE, &abd);
  appbar_registered_ = false;
}

void MenuBar::Layout() {
  if (hwnd_ == nullptr || fullscreen_occluded_ || !appbar_registered_) {
    return;
  }

  const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    return;
  }

  const int height = BarHeightPx();
  APPBARDATA abd{};
  abd.cbSize = sizeof(abd);
  abd.hWnd = hwnd_;
  abd.uEdge = ABE_TOP;
  abd.rc = info.rcMonitor;
  abd.rc.bottom = abd.rc.top + height;
  SHAppBarMessage(ABM_QUERYPOS, &abd);
  abd.rc.bottom = abd.rc.top + height;
  SHAppBarMessage(ABM_SETPOS, &abd);

  SetWindowPos(hwnd_, HWND_TOPMOST, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left, abd.rc.bottom - abd.rc.top,
               SWP_NOACTIVATE);

  if (tooltip_ != nullptr) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = 1;
    GetClientRect(hwnd_, &ti.rect);
    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
  }
}

void MenuBar::ApplyBackdrop() {
  if (hwnd_ == nullptr) {
    return;
  }

  const BOOL dark = dark_ ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));

  const int backdrop = dwm::kBackdropMainWindow;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));

  const int corner = dwm::kCornerDoNotRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));

  const MARGINS margins{-1, -1, -1, -1};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);
}

void MenuBar::Paint() {
  PAINTSTRUCT ps{};
  const HDC hdc = BeginPaint(hwnd_, &ps);
  RECT client{};
  GetClientRect(hwnd_, &client);

  BP_PAINTPARAMS params{};
  params.cbSize = sizeof(params);
  params.dwFlags = BPPF_ERASE;
  HDC buffer_dc = nullptr;
  const HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &client, BPBF_TOPDOWNDIB, &params, &buffer_dc);
  if (buffer != nullptr && buffer_dc != nullptr) {
    BufferedPaintClear(buffer, &client);
    const auto items = status_.Snapshot();
    clock_.Draw(buffer_dc, client, dark_, taskbar_.warning(), items, &hits_, &start_rect_, start_hot_,
                start_pressed_);
    EndBufferedPaint(buffer, TRUE);
  }
  EndPaint(hwnd_, &ps);
}

const StatusHit* MenuBar::HitTest(POINT client) const {
  for (const auto& hit : hits_) {
    if (PtInRect(&hit.rect, client)) {
      return &hit;
    }
  }
  return nullptr;
}

bool MenuBar::HitStart(POINT client) const {
  return PtInRect(&start_rect_, client) != FALSE;
}

void MenuBar::UpdateStartChrome(POINT client) {
  ArmMouseLeave();
  const bool hot = HitStart(client);
  if (start_hot_ == hot) {
    return;
  }
  start_hot_ = hot;
  InvalidateRect(hwnd_, &start_rect_, FALSE);
}

void MenuBar::ArmMouseLeave() {
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  TrackMouseEvent(&track);
}

void MenuBar::ToggleStartMenu() {
  HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (tray != nullptr) {
    DWORD pid = 0;
    GetWindowThreadProcessId(tray, &pid);
    if (pid != 0) {
      AllowSetForegroundWindow(pid);
    }
    if (PostMessageW(tray, WM_SYSCOMMAND, SC_TASKLIST, 0)) {
      return;
    }
  }

  INPUT keys[2]{};
  keys[0].type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_LWIN;
  keys[1].type = INPUT_KEYBOARD;
  keys[1].ki.wVk = VK_LWIN;
  keys[1].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(2, keys, sizeof(INPUT));
}

void MenuBar::ShowContextMenu(POINT screen) {
  const HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  AppendMenuW(menu, MF_STRING, kExitCommand, L"종료");
  TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, screen.x, screen.y, hwnd_, nullptr);
  DestroyMenu(menu);
}

void MenuBar::RefreshFullscreenState() {
  SetFullscreenOccluded(IsTrueFullscreen(hwnd_));
}

void MenuBar::SetFullscreenOccluded(bool occluded) {
  if (fullscreen_occluded_ == occluded) {
    return;
  }
  fullscreen_occluded_ = occluded;
  if (occluded) {
    UnregisterAppBar();
    ShowWindow(hwnd_, SW_HIDE);
  } else {
    RegisterAppBar();
    ShowWindow(hwnd_, SW_SHOWNA);
    Layout();
  }
}

UINT MenuBar::Dpi() const {
  if (hwnd_ == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(hwnd_);
  return dpi == 0 ? 96 : dpi;
}

int MenuBar::BarHeightPx() const {
  return DipToPx(kBarHeightDip, Dpi());
}

}  // namespace bamti
