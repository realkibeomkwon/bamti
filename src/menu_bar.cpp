#include "menu_bar.hpp"

#include "dwm.hpp"
#include "fullscreen.hpp"
#include "log.hpp"
#include "theme.hpp"
#include "watchdog.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <memory>
#include <string_view>

namespace bamti {
namespace {

constexpr UINT kAppBarCallback = WM_APP + 1;
constexpr UINT kToggleStartMsg = WM_APP + 7;
constexpr UINT kToggleSpotlightMsg = WM_APP + 8;
constexpr UINT kFullscreenWatchMsg = WM_APP + 9;
constexpr UINT_PTR kClockTimerId = 1;
constexpr UINT_PTR kRepaintTimerId = 2;
constexpr UINT_PTR kToggleTimerId = 3;
constexpr UINT kRepaintCoalesceMs = 16;
constexpr UINT kToggleTimeoutMs = 2000;
constexpr int kBarHeightDip = 32;
constexpr UINT kExitCommand = 1;

MenuBar* g_menu_bar = nullptr;
HHOOK g_key_hook = nullptr;
UINT g_status_msg_count = 0;
ULONGLONG g_status_msg_window = 0;
UINT g_toggle_start_count = 0;
ULONGLONG g_toggle_start_window = 0;
UINT g_toggle_spotlight_count = 0;
ULONGLONG g_toggle_spotlight_window = 0;
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

void InvalidateArea(HWND hwnd, RECT rc) {
  InvalidateRect(hwnd, &rc, FALSE);
}

double QpcMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  if (freq.QuadPart == 0) {
    return 0.0;
  }
  return (end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

}  // namespace

MenuBar::MenuBar()
    : status_panel_(std::make_unique<StatusPanelContent>()),
      overflow_panel_(std::make_unique<OverflowContent>()) {}

MenuBar::~MenuBar() {
  RemoveWinHook();
  status_.StopAll();
  start_menu_.Hide();
  spotlight_.Hide();
  status_popup_.Destroy();
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

  if (!layout_.Initialize() || !clock_.Initialize()) {
    return false;
  }

  dark_ = ShellUsesDarkMode();

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kMenuBarClass, L"bamti", WS_POPUP, 0, 0,
                          0, 0, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }

  layout_.SetDpi(Dpi());
  clock_.SetDpi(Dpi());
  ApplyBackdrop();
  if (!RegisterAppBar()) {
    return false;
  }
  CreateTooltip();
  status_.SetNotify(hwnd_);
  status_.Register(&pipe_);
  status_.Register(&widgets_);
  if (!status_.StartAll()) {
    return false;
  }
  session_notify_ = WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;
  display_notify_ = RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
  if (!status_popup_.Create(instance, hwnd_)) {
    return false;
  }
  status_popup_.SetDark(dark_);
  Layout();
  taskbar_.Restore();
  ShowWindow(hwnd_, SW_SHOWNA);
  taskbar_.Hide();
  Log(L"bar", L"ready hwnd=%p taskbar_hidden=%d", hwnd_, taskbar_.hidden() ? 1 : 0);
  start_menu_.Warmup(hwnd_, dark_);
  spotlight_.Warmup(hwnd_, dark_);
  InstallWinHook();
  SetTimer(hwnd_, kClockTimerId, 1000, nullptr);
  StartFullscreenWatch(hwnd_, kFullscreenWatchMsg);
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
      if (wparam == kRepaintTimerId) {
        KillTimer(hwnd_, kRepaintTimerId);
        repaint_armed_ = false;
        RefreshLayout();
        return 0;
      }
      if (wparam == kClockTimerId) {
        if (status_popup_.IsOpen()) {
          status_popup_.Tick();
        }
        status_.DropStale();
        taskbar_.EnsureHidden();
        RefreshLayout();
      }
      if (wparam == kToggleTimerId) {
        OnToggleTimeout();
      }
      return 0;
    case kPopupClosedMsg:
      if (!open_panel_id_.empty()) {
        StatusEvent ev;
        ev.id = std::move(open_panel_id_);
        ev.event = "panel_close";
        status_.Dispatch(ev);
        open_panel_id_.clear();
      }
      return 0;
    case kFullscreenWatchMsg:
      RefreshFullscreenState();
      return 0;
    case kStatusChangedMsg:
      NotePostedStorm(L"status", g_status_msg_count, g_status_msg_window);
      ArmRepaint();
      return 0;
    case WM_DPICHANGED:
      layout_.SetDpi(HIWORD(wparam));
      clock_.SetDpi(HIWORD(wparam));
      ApplyBackdrop();
      Layout();
      InvalidateRect(hwnd_, nullptr, FALSE);
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
      if (msg == WM_SETTINGCHANGE) {
        const wchar_t* area = reinterpret_cast<const wchar_t*>(lparam);
        if (area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0) {
          dark_ = ShellUsesDarkMode();
          ApplyBackdrop();
          status_popup_.SetDark(dark_);
        }
      }
      layout_.SetDpi(Dpi());
      clock_.SetDpi(Dpi());
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
        if (HitStart(pt) || HitSegment(pt) != nullptr) {
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
        InvalidateArea(hwnd_, StartRect());
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (HitStart(pt)) {
        status_popup_.Close();
        start_pressed_ = true;
        SetCapture(hwnd_);
        InvalidateArea(hwnd_, StartRect());
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
        InvalidateArea(hwnd_, StartRect());
      }
      return 0;
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool start_click = start_pressed_ && HitStart(pt);
      if (start_pressed_) {
        start_pressed_ = false;
        ReleaseCapture();
        InvalidateArea(hwnd_, StartRect());
      }
      if (start_click) {
        ToggleStartMenu();
        return 0;
      }
      if (const BarSegment* seg = HitSegment(pt)) {
        if (seg->kind == SegmentKind::kOverflow) {
          OpenOverflow();
          return 0;
        }
      }
      if (const auto hit = HitTest(pt)) {
        StatusEvent ev;
        ev.id = hit->id;
        ev.event = "click";
        ev.button = "left";
        status_.Dispatch(ev);
        OpenStatusPanel(*hit);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (const auto hit = HitTest(pt)) {
        StatusEvent ev;
        ev.id = hit->id;
        ev.event = "click";
        ev.button = "right";
        status_.Dispatch(ev);
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
        } else if (const BarSegment* seg = HitSegment(pt); seg != nullptr && seg->kind == SegmentKind::kOverflow) {
          tooltip_text_ = seg->tooltip.empty() ? std::wstring(L"접힌 항목") : seg->tooltip;
          info->lpszText = tooltip_text_.data();
        } else if (const auto hit = HitTest(pt)) {
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
      NotePostedStorm(L"toggle-start", g_toggle_start_count, g_toggle_start_window);
      ToggleStartMenu(true);
      return 0;
    case kToggleSpotlightMsg:
      NotePostedStorm(L"toggle-spotlight", g_toggle_spotlight_count, g_toggle_spotlight_window);
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
    case WM_POWERBROADCAST:
      if (wparam == PBT_APMPOWERSTATUSCHANGE) {
        widgets_.NotePowerEvent(false);
      } else if (wparam == PBT_APMRESUMEAUTOMATIC || wparam == PBT_APMRESUMESUSPEND) {
        widgets_.NotePowerEvent(true);
      } else if (wparam == PBT_POWERSETTINGCHANGE) {
        const auto* setting = reinterpret_cast<POWERBROADCAST_SETTING*>(lparam);
        if (setting != nullptr && setting->PowerSetting == GUID_CONSOLE_DISPLAY_STATE &&
            setting->DataLength >= sizeof(DWORD)) {
          const DWORD state = *reinterpret_cast<const DWORD*>(setting->Data);
          display_on_ = state != 0;
          UpdateProviderActive();
        }
      }
      return TRUE;
    case WM_WTSSESSION_CHANGE:
      if (wparam == WTS_SESSION_LOCK) {
        session_locked_ = true;
        UpdateProviderActive();
      } else if (wparam == WTS_SESSION_UNLOCK) {
        session_locked_ = false;
        UpdateProviderActive();
      }
      return 0;
    case WM_QUERYENDSESSION:
      return TRUE;
    case WM_ENDSESSION:
      if (wparam) {
        KillTimer(hwnd_, kClockTimerId);
        KillTimer(hwnd_, kRepaintTimerId);
        KillTimer(hwnd_, kToggleTimerId);
        StopFullscreenWatch(hwnd_);
        UnregisterSessionWatch();
        status_.StopAll();
        taskbar_.Restore();
        UnregisterAppBar();
      }
      return 0;
    case WM_DESTROY:
      RemoveWinHook();
      KillTimer(hwnd_, kClockTimerId);
      KillTimer(hwnd_, kRepaintTimerId);
      KillTimer(hwnd_, kToggleTimerId);
      StopFullscreenWatch(hwnd_);
      UnregisterSessionWatch();
      status_.StopAll();
      status_popup_.Destroy();
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

void MenuBar::ArmRepaint() {
  if (repaint_armed_ || hwnd_ == nullptr) {
    return;
  }
  repaint_armed_ = true;
  SetTimer(hwnd_, kRepaintTimerId, kRepaintCoalesceMs, nullptr);
}

void MenuBar::RefreshLayout() {
  if (hwnd_ == nullptr) {
    return;
  }
  const BarLayoutResult before = layout_.last();
  RECT client{};
  GetClientRect(hwnd_, &client);
  layout_.SetDpi(Dpi());
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  const BarLayoutResult& after =
      layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), status_.Snapshot());
  QueryPerformanceCounter(&t1);
  last_compute_ms_ = QpcMs(t0, t1);

  if (before.segments.size() != after.segments.size() || before.dpi != after.dpi ||
      EqualRect(&before.client, &after.client) == FALSE) {
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  for (size_t i = 0; i < after.segments.size(); ++i) {
    const BarSegment& a = before.segments[i];
    const BarSegment& b = after.segments[i];
    if (a.kind != b.kind || a.id != b.id || EqualRect(&a.rect, &b.rect) == FALSE) {
      InvalidateRect(hwnd_, nullptr, FALSE);
      return;
    }
    if (a.text != b.text || a.accent != b.accent || a.icon_key != b.icon_key) {
      InvalidateRect(hwnd_, &b.rect, FALSE);
    }
  }
}

void MenuBar::NotePerf(double compute_ms, double draw_ms, const RECT& dirty, const RECT& client, const DrawTimings& draw,
                      double bpbegin_ms, double bpend_ms) {
  perf_compute_.Add(compute_ms);
  if (EqualRect(&dirty, &client) != FALSE) {
    perf_full_.Add(draw_ms);
  } else {
    perf_seg_.Add(draw_ms);
  }
  perf_bind_.Add(draw.bind_ms);
  perf_brush_.Add(draw.brush_ms);
  perf_begin_.Add(draw.begin_ms);
  perf_draw_.Add(draw.draw_ms);
  perf_end_.Add(draw.end_ms);
  perf_bpbegin_.Add(bpbegin_ms);
  perf_bpend_.Add(bpend_ms);
  ++perf_frames_;
  if (perf_frames_ % 100 != 0) {
    return;
  }

  const BarLayoutResult& last = layout_.last();
  auto format_acc = [](wchar_t* out, size_t cap, const wchar_t* name, const PerfAcc& acc) {
    if (acc.n == 0) {
      swprintf_s(out, cap, L"%s[n=0]ms", name);
    } else {
      swprintf_s(out, cap, L"%s[n=%u avg=%.1f max=%.1f]ms", name, acc.n, acc.sum / static_cast<double>(acc.n),
                 acc.maxv);
    }
  };
  auto avg = [](const PerfAcc& acc) { return acc.n == 0 ? 0.0 : acc.sum / static_cast<double>(acc.n); };
  wchar_t full_s[64]{};
  wchar_t seg_s[64]{};
  wchar_t compute_s[64]{};
  format_acc(full_s, 64, L"full", perf_full_);
  format_acc(seg_s, 64, L"seg", perf_seg_);
  format_acc(compute_s, 64, L"compute", perf_compute_);
  Log(L"perf", L"bar %s %s %s segments=%u overflow=%u%s", full_s, seg_s, compute_s,
      static_cast<unsigned>(last.segments.size()), static_cast<unsigned>(last.overflow.size()),
      perf_cold_ ? L" cold" : L"");
  Log(L"perf", L"draw bind=%.2f brush=%.2f begin=%.2f draw=%.2f end=%.2f bpbegin=%.2f bpend=%.2f (ms, avg)",
      avg(perf_bind_), avg(perf_brush_), avg(perf_begin_), avg(perf_draw_), avg(perf_end_), avg(perf_bpbegin_),
      avg(perf_bpend_));
  perf_full_.Reset();
  perf_seg_.Reset();
  perf_compute_.Reset();
  perf_bind_.Reset();
  perf_brush_.Reset();
  perf_begin_.Reset();
  perf_draw_.Reset();
  perf_end_.Reset();
  perf_bpbegin_.Reset();
  perf_bpend_.Reset();
  perf_cold_ = false;
}

void MenuBar::Paint() {
  WatchdogStage(L"bar.paint");
  PAINTSTRUCT ps{};
  const HDC hdc = BeginPaint(hwnd_, &ps);
  RECT client{};
  GetClientRect(hwnd_, &client);

  RECT dirty = ps.rcPaint;
  if (IsRectEmpty(&dirty) == FALSE) {
    if (layout_.last().dpi != Dpi() || EqualRect(&layout_.last().client, &client) == FALSE) {
      layout_.SetDpi(Dpi());
      LARGE_INTEGER t0{};
      LARGE_INTEGER t1{};
      QueryPerformanceCounter(&t0);
      layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), status_.Snapshot());
      QueryPerformanceCounter(&t1);
      last_compute_ms_ = QpcMs(t0, t1);
    }

    BP_PAINTPARAMS params{};
    params.cbSize = sizeof(params);
    params.dwFlags = BPPF_ERASE;
    HDC buffer_dc = nullptr;
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t0);
    const HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &dirty, BPBF_TOPDOWNDIB, &params, &buffer_dc);
    if (buffer != nullptr && buffer_dc != nullptr) {
      BufferedPaintClear(buffer, &dirty);
      QueryPerformanceCounter(&t1);
      const double bpbegin_ms = QpcMs(t0, t1);

      DrawTimings draw{};
      QueryPerformanceCounter(&t0);
      clock_.Draw(buffer_dc, client, dirty, dark_, layout_.last(), &layout_,
                  start_hot_ || start_menu_.visible(), start_pressed_ || start_menu_.visible(), &draw);
      QueryPerformanceCounter(&t1);
      const double draw_ms = QpcMs(t0, t1);

      QueryPerformanceCounter(&t0);
      EndBufferedPaint(buffer, TRUE);
      QueryPerformanceCounter(&t1);
      NotePerf(last_compute_ms_, draw_ms, dirty, client, draw, bpbegin_ms, QpcMs(t0, t1));
    }
  }
  EndPaint(hwnd_, &ps);
}

RECT MenuBar::StartRect() const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kStart) {
      return seg.rect;
    }
  }
  return {};
}

RECT MenuBar::ClockRect() const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kClock) {
      return seg.rect;
    }
  }
  return {};
}

const BarSegment* MenuBar::HitSegment(POINT client) const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (PtInRect(&seg.rect, client) != FALSE) {
      if (seg.kind == SegmentKind::kStatus || seg.kind == SegmentKind::kOverflow) {
        return &seg;
      }
    }
  }
  return nullptr;
}

void MenuBar::OpenOverflow() {
  if (overflow_panel_ == nullptr || hwnd_ == nullptr || layout_.last().overflow.empty()) {
    return;
  }
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateArea(hwnd_, StartRect());
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  OverflowHost host;
  host.dark = dark_;
  host.overflow_rect = {};
  host.open_panel = [this](const std::string& id) {
    StatusHit hit;
    hit.id = id;
    for (const BarSegment& seg : layout_.last().segments) {
      if (seg.kind == SegmentKind::kOverflow) {
        hit.rect = seg.rect;
        break;
      }
    }
    OpenStatusPanel(hit);
  };
  host.dispatch = [this](const StatusEvent& ev) { status_.Dispatch(ev); };
  overflow_panel_->Reset(layout_.last().overflow, std::move(host));
  RECT chevron{};
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kOverflow) {
      chevron = seg.rect;
      break;
    }
  }
  POINT anchor{chevron.left, chevron.bottom};
  ClientToScreen(hwnd_, &anchor);
  status_popup_.Open(overflow_panel_.get(), anchor, PopupSurface::Anchor::BelowAt);
}

std::optional<StatusHit> MenuBar::HitTest(POINT client) const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kStatus && PtInRect(&seg.rect, client) != FALSE) {
      StatusHit hit;
      hit.id = seg.id;
      hit.rect = seg.rect;
      hit.tooltip = seg.tooltip;
      return hit;
    }
  }
  return std::nullopt;
}

bool MenuBar::HitStart(POINT client) const {
  const RECT start = StartRect();
  return PtInRect(&start, client) != FALSE;
}

void MenuBar::UpdateStartChrome(POINT client) {
  ArmMouseLeave();
  const bool hot = HitStart(client);
  if (start_hot_ == hot) {
    return;
  }
  start_hot_ = hot;
  InvalidateArea(hwnd_, StartRect());
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
  status_popup_.Close();
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  RECT start = StartRect();
  if (start.right <= start.left) {
    RECT client{};
    GetClientRect(hwnd_, &client);
    start = client;
    start.right = start.left + DipToPx(34, Dpi());
  }
  MapWindowPoints(hwnd_, nullptr, reinterpret_cast<LPPOINT>(&start), 2);
  start_menu_.Toggle(hwnd_, start, dark_, from_keyboard);
  InvalidateArea(hwnd_, StartRect());
}

void MenuBar::ToggleSpotlight() {
  if (fullscreen_occluded_) {
    return;
  }
  status_popup_.Close();
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateArea(hwnd_, StartRect());
  }
  spotlight_.Toggle(hwnd_, dark_);
}

void MenuBar::OpenStatusPanel(const StatusHit& hit) {
  if (status_panel_ == nullptr || hwnd_ == nullptr) {
    return;
  }
  const auto items = status_.Snapshot();
  const StatusItem* found = nullptr;
  for (const auto& item : items) {
    if (item.id == hit.id) {
      found = &item;
      break;
    }
  }
  if (found == nullptr || !found->panel) {
    return;
  }
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateArea(hwnd_, StartRect());
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  StatusPanelHost host;
  host.dark = dark_;
  host.dispatch = [this](const StatusEvent& ev) { status_.Dispatch(ev); };
  host.arm_toggle = [this](std::string id, std::string row_id, uint64_t revision, bool on) {
    ArmToggle(std::move(id), std::move(row_id), revision, on);
  };
  status_panel_->Reset(*found, std::move(host));
  POINT anchor{hit.rect.left, hit.rect.bottom};
  ClientToScreen(hwnd_, &anchor);
  open_panel_id_ = found->id;
  StatusEvent ev;
  ev.id = found->id;
  ev.event = "panel_open";
  status_.Dispatch(ev);
  status_popup_.Open(status_panel_.get(), anchor, PopupSurface::Anchor::BelowAt);
}

void MenuBar::ArmToggle(std::string id, std::string row_id, uint64_t revision, bool on) {
  pending_toggle_.id = std::move(id);
  pending_toggle_.row_id = std::move(row_id);
  pending_toggle_.revision = revision;
  pending_toggle_.on = on;
  toggle_armed_ = true;
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kToggleTimerId, kToggleTimeoutMs, nullptr);
  }
}

void MenuBar::OnToggleTimeout() {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kToggleTimerId);
  }
  if (!toggle_armed_) {
    return;
  }
  toggle_armed_ = false;
  auto item = status_.Get(pending_toggle_.id);
  if (!item) {
    Log(L"status", L"toggle timeout missing id=%S", pending_toggle_.id.c_str());
    return;
  }
  if (item->revision != pending_toggle_.revision) {
    Log(L"status", L"toggle timeout acked id=%S pending_rev=%llu now_rev=%llu", pending_toggle_.id.c_str(),
        static_cast<unsigned long long>(pending_toggle_.revision), static_cast<unsigned long long>(item->revision));
    return;
  }
  Log(L"status", L"toggle timeout revert id=%S row=%S rev=%llu", pending_toggle_.id.c_str(),
      pending_toggle_.row_id.c_str(), static_cast<unsigned long long>(pending_toggle_.revision));
  if (item->panel) {
    for (StatusRow& row : item->panel->rows) {
      if (row.type == RowType::kToggle && row.row_id == pending_toggle_.row_id) {
        row.on = !pending_toggle_.on;
        break;
      }
    }
  }
  item->state = StatusState::kError;
  const std::string id = item->id;
  status_.Upsert(std::move(*item));
  if (status_popup_.IsOpen() && open_panel_id_ == id && status_panel_ != nullptr) {
    if (auto again = status_.Get(id)) {
      StatusPanelHost host;
      host.dark = dark_;
      host.dispatch = [this](const StatusEvent& ev) { status_.Dispatch(ev); };
      host.arm_toggle = [this](std::string iid, std::string row_id, uint64_t revision, bool on) {
        ArmToggle(std::move(iid), std::move(row_id), revision, on);
      };
      status_panel_->Reset(std::move(*again), std::move(host));
      if (status_popup_.hwnd() != nullptr) {
        InvalidateRect(status_popup_.hwnd(), nullptr, FALSE);
      }
    }
  }
  ArmRepaint();
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

void MenuBar::UpdateProviderActive() {
  const bool active = !fullscreen_occluded_ && !session_locked_ && display_on_;
  if (active == providers_active_) {
    return;
  }
  providers_active_ = active;
  status_.SetActive(active);
}

void MenuBar::UnregisterSessionWatch() {
  if (session_notify_ && hwnd_ != nullptr) {
    WTSUnRegisterSessionNotification(hwnd_);
    session_notify_ = false;
  }
  if (display_notify_ != nullptr) {
    UnregisterPowerSettingNotification(display_notify_);
    display_notify_ = nullptr;
  }
}

void MenuBar::SetFullscreenOccluded(bool occluded) {
  if (fullscreen_occluded_ == occluded) {
    return;
  }
  fullscreen_occluded_ = occluded;
  UpdateProviderActive();
  if (occluded) {
    start_menu_.Hide();
    spotlight_.Hide();
    status_popup_.Close();
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
