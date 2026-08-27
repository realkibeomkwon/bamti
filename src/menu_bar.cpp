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
constexpr UINT kToggleStartMsg = WM_APP + 7;
constexpr UINT kToggleSpotlightMsg = WM_APP + 8;
constexpr UINT_PTR kClockTimerId = 1;
constexpr UINT_PTR kFullscreenTimerId = 2;
constexpr UINT kFullscreenPollMs = 100;
constexpr int kBarHeightDip = 32;
constexpr UINT kExitCommand = 1;

MenuBar* g_menu_bar = nullptr;
HHOOK g_key_hook = nullptr;
bool g_win_held = false;
bool g_win_combo = false;
bool g_win_injected = false;
bool g_swallow_space = false;
DWORD g_win_vk = VK_LWIN;

void InjectWinKey(DWORD vk, bool up) {
  INPUT in{};
  in.type = INPUT_KEYBOARD;
  in.ki.wVk = static_cast<WORD>(vk);
  if (up) {
    in.ki.dwFlags |= KEYEVENTF_KEYUP;
  }
  if (vk == VK_RWIN) {
    in.ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  SendInput(1, &in, sizeof(INPUT));
}

LRESULT CALLBACK LowLevelKeyboardProc(int code, WPARAM wparam, LPARAM lparam) {
  if (code != HC_ACTION || g_menu_bar == nullptr || g_menu_bar->hwnd() == nullptr ||
      !g_menu_bar->win_key_enabled()) {
    return CallNextHookEx(g_key_hook, code, wparam, lparam);
  }
  const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
  if (info == nullptr || (info->flags & LLKHF_INJECTED) != 0) {
    return CallNextHookEx(g_key_hook, code, wparam, lparam);
  }

  const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
  const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
  const DWORD vk = info->vkCode;
  const bool is_win = vk == VK_LWIN || vk == VK_RWIN;

  if (is_win) {
    if (down) {
      g_win_held = true;
      g_win_combo = false;
      g_win_injected = false;
      g_win_vk = vk;
      return 1;
    }
    if (up) {
      g_win_held = false;
      if (g_win_combo) {
        if (g_win_injected) {
          InjectWinKey(g_win_vk, true);
        }
      } else {
        PostMessageW(g_menu_bar->hwnd(), kToggleStartMsg, 0, 0);
      }
      g_win_injected = false;
      return 1;
    }
  } else if (vk == VK_SPACE && up && g_swallow_space) {
    g_swallow_space = false;
    return 1;
  } else if (g_win_held && down) {
    const bool extra_mod = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0 ||
                           (GetAsyncKeyState(VK_MENU) & 0x8000) != 0 ||
                           (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
    if (vk == VK_SPACE && !extra_mod) {
      g_win_combo = true;
      g_swallow_space = true;
      PostMessageW(g_menu_bar->hwnd(), kToggleSpotlightMsg, 0, 0);
      return 1;
    }
    if (!g_win_combo) {
      g_win_combo = true;
      g_win_injected = true;
      InjectWinKey(g_win_vk, false);
    }
  }
  return CallNextHookEx(g_key_hook, code, wparam, lparam);
}

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

}  // namespace

MenuBar::~MenuBar() {
  RemoveWinHook();
  status_.Stop();
  start_menu_.Hide();
  spotlight_.Hide();
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
  start_menu_.Warmup(hwnd_, dark_);
  spotlight_.Warmup(hwnd_, dark_);
  InstallWinHook();
  SetTimer(hwnd_, kClockTimerId, 1000, nullptr);
  SetTimer(hwnd_, kFullscreenTimerId, kFullscreenPollMs, nullptr);
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
        taskbar_.EnsureHidden();
        InvalidateRect(hwnd_, nullptr, FALSE);
      } else if (wparam == kFullscreenTimerId) {
        RefreshFullscreenState();
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
      if (start_menu_.visible()) {
        start_menu_.Hide();
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
    case kToggleStartMsg:
      ToggleStartMenu(true);
      return 0;
    case kToggleSpotlightMsg:
      ToggleSpotlight();
      return 0;
    case kAppBarCallback:
      switch (wparam) {
        case ABN_POSCHANGED:
          Layout();
          break;
        case ABN_FULLSCREENAPP:
          // Explorer fires this when windows minimize or lose activation. Hiding
          // the AppBar on lparam=TRUE without a check unregisters it and the bar
          // flickers back on the next poll.
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
        KillTimer(hwnd_, kFullscreenTimerId);
        status_.Stop();
        taskbar_.Restore();
        UnregisterAppBar();
      }
      return 0;
    case WM_DESTROY:
      RemoveWinHook();
      KillTimer(hwnd_, kClockTimerId);
      KillTimer(hwnd_, kFullscreenTimerId);
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
        clock_.Draw(buffer_dc, client, dark_, taskbar_.warning(), items, &hits_, &start_rect_,
                    start_hot_ || start_menu_.visible(), start_pressed_ || start_menu_.visible());
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

void MenuBar::ToggleStartMenu(bool from_keyboard) {
  if (fullscreen_occluded_) {
    return;
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  RECT start = start_rect_;
  if (start.right <= start.left) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    start = client;
    start.right = start.left + DipToPx(34, Dpi());
  }
  MapWindowPoints(hwnd_, nullptr, reinterpret_cast<LPPOINT>(&start), 2);
  start_menu_.Toggle(hwnd_, start, dark_, from_keyboard);
  InvalidateRect(hwnd_, &start_rect_, FALSE);
}

void MenuBar::ToggleSpotlight() {
  if (fullscreen_occluded_) {
    return;
  }
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateRect(hwnd_, &start_rect_, FALSE);
  }
  spotlight_.Toggle(hwnd_, dark_);
}

bool MenuBar::InstallWinHook() {
  g_menu_bar = this;
  if (g_key_hook != nullptr) {
    return true;
  }
  g_key_hook = SetWindowsHookExW(WH_KEYBOARD_LL, LowLevelKeyboardProc, GetModuleHandleW(nullptr), 0);
  return g_key_hook != nullptr;
}

void MenuBar::RemoveWinHook() {
  if (g_key_hook != nullptr) {
    UnhookWindowsHookEx(g_key_hook);
    g_key_hook = nullptr;
  }
  if (g_menu_bar == this) {
    g_menu_bar = nullptr;
  }
  g_win_held = false;
  g_win_combo = false;
  g_win_injected = false;
  g_swallow_space = false;
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
    start_menu_.Hide();
    spotlight_.Hide();
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
