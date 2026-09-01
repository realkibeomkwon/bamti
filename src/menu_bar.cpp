#include "menu_bar.hpp"

#include "autostart.hpp"
#include "control_center.hpp"
#include "dwm.hpp"
#include "fullscreen.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "watchdog.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <wtsapi32.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string_view>

namespace bamti {
namespace {

constexpr UINT kAppBarCallback = WM_APP + 1;
constexpr UINT kToggleStartMsg = WM_APP + 7;
constexpr UINT kFullscreenWatchMsg = WM_APP + 9;
constexpr UINT_PTR kClockTimerId = 1;
constexpr UINT_PTR kRepaintTimerId = 2;
constexpr UINT_PTR kToggleTimerId = 3;
constexpr UINT kRepaintCoalesceMs = 16;
constexpr UINT kToggleTimeoutMs = 2000;
constexpr int kBarHeightDip = 32;
constexpr UINT kExitCommand = 1;
constexpr UINT kWidgetBatteryCmd = 10;
constexpr UINT kWidgetCpuCmd = 11;
constexpr UINT kWidgetNetworkCmd = 12;
constexpr UINT kWidgetBoardCmd = 13;
constexpr UINT kTrayMirrorToggleCmd = 14;
constexpr UINT kTraySystemIconsCmd = 15;
constexpr UINT kTrayOverflowIconsCmd = 16;
constexpr UINT kTrayInterceptCmd = 17;
constexpr UINT kAutostartCmd = 18;
constexpr UINT kWidgetVolumeCmd = 19;
constexpr UINT kWidgetControlCenterCmd = 20;
constexpr UINT kTrayPeekCmd = 20;
constexpr UINT kTrayHideIconCmd = 21;
constexpr UINT kTrayMirrorOffCmd = 22;
constexpr UINT kTrayItemCmdBase = 4000;
constexpr UINT_PTR kPeekTimerId = 4;
constexpr UINT kPeekMs = 10000;

UINT g_taskbar_created = 0;

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

std::vector<RowType> PanelRowTypes(const StatusPanel& panel) {
  std::vector<RowType> types;
  types.reserve(panel.rows.size());
  for (const StatusRow& row : panel.rows) {
    types.push_back(row.type);
  }
  return types;
}

bool OrderContains(const std::vector<std::string>& ids, const std::string& id) {
  for (const std::string& one : ids) {
    if (one == id) {
      return true;
    }
  }
  return false;
}

std::vector<std::string> ApplyVisiblePermutation(const std::vector<std::string>& full,
                                                 const std::vector<std::string>& visible_rtl) {
  std::vector<std::string> out;
  out.reserve(full.size() + visible_rtl.size());
  size_t vi = 0;
  for (const std::string& id : full) {
    bool visible = false;
    for (const std::string& v : visible_rtl) {
      if (v == id) {
        visible = true;
        break;
      }
    }
    if (visible) {
      if (vi < visible_rtl.size()) {
        out.push_back(visible_rtl[vi++]);
      }
    } else {
      out.push_back(id);
    }
  }
  while (vi < visible_rtl.size()) {
    if (!OrderContains(out, visible_rtl[vi])) {
      out.push_back(visible_rtl[vi]);
    }
    ++vi;
  }
  if (out.size() > kBarOrderMax) {
    out.resize(kBarOrderMax);
  }
  return out;
}

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
      overflow_panel_(std::make_unique<OverflowContent>()),
      cc_panel_(std::make_unique<ControlCenterContent>()) {}

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
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
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
  status_.Register(&tray_);
  status_.Register(&widgets_);
  if (g_taskbar_created == 0) {
    g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
  }
  if (g_taskbar_created != 0) {
    ChangeWindowMessageFilterEx(hwnd_, g_taskbar_created, MSGFLT_ALLOW, nullptr);
  }
  if (!status_.StartAll()) {
    return false;
  }
  bar_order_ = widgets_.settings().bar_order;
  tray_.SetRectLookup([this](uint64_t key, RECT* out) {
    if (hwnd_ == nullptr || out == nullptr) {
      return false;
    }
    for (const BarSegment& seg : layout_.last().segments) {
      if (seg.kind == SegmentKind::kStatus && TrayMirror::ParseId(seg.id) == key) {
        *out = seg.rect;
        MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(out), 2);
        return true;
      }
    }
    return false;
  });
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
        RefreshOpenPanel();
      }
      if (wparam == kToggleTimerId) {
        OnToggleTimeout();
      }
      if (wparam == kPeekTimerId) {
        EndTrayPeek();
      }
      return 0;
    case kPopupClosedMsg:
      cc_open_ = false;
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
      RefreshOpenPanel();
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
        if (ReorderCursor(pt)) {
          SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
          return TRUE;
        }
        if (HitStart(pt) || HitSpotlight(pt) || HitControlCenter(pt) || HitSegment(pt) != nullptr) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      break;
    }
    case WM_MOUSEMOVE: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (reorder_active_) {
        if (!reorder_moved_) {
          const LONG dx = pt.x >= reorder_start_.x ? pt.x - reorder_start_.x : reorder_start_.x - pt.x;
          if (dx < GetSystemMetrics(SM_CXDRAG)) {
            return 0;
          }
          reorder_moved_ = true;
        }
        if (UpdateReorder(pt)) {
          RefreshLayout();
        }
        return 0;
      }
      UpdateChrome(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (start_hot_ || start_pressed_) {
        start_hot_ = false;
        start_pressed_ = false;
        InvalidateArea(hwnd_, StartRect());
      }
      if (spotlight_hot_ || spotlight_pressed_) {
        spotlight_hot_ = false;
        spotlight_pressed_ = false;
        InvalidateArea(hwnd_, SpotlightRect());
      }
      if (cc_hot_ || cc_pressed_) {
        cc_hot_ = false;
        cc_pressed_ = false;
        InvalidateArea(hwnd_, ControlCenterRect());
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
      if (HitSpotlight(pt)) {
        status_popup_.Close();
        spotlight_pressed_ = true;
        SetCapture(hwnd_);
        InvalidateArea(hwnd_, SpotlightRect());
        return 0;
      }
      if (HitControlCenter(pt)) {
        status_popup_.Close();
        cc_pressed_ = true;
        SetCapture(hwnd_);
        InvalidateArea(hwnd_, ControlCenterRect());
        return 0;
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (const BarSegment* seg = HitSegment(pt)) {
          if (seg->kind == SegmentKind::kStatus) {
            BeginReorder(seg->id, pt);
            return 0;
          }
        }
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
      if (spotlight_pressed_) {
        spotlight_pressed_ = false;
        InvalidateArea(hwnd_, SpotlightRect());
      }
      if (cc_pressed_) {
        cc_pressed_ = false;
        InvalidateArea(hwnd_, ControlCenterRect());
      }
      if (reorder_active_ && reinterpret_cast<HWND>(lparam) != hwnd_) {
        CancelReorder();
      }
      return 0;
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd_, &pt);
      wheel_accum_ += GET_WHEEL_DELTA_WPARAM(wparam);
      const int notches = wheel_accum_ / WHEEL_DELTA;
      wheel_accum_ -= notches * WHEEL_DELTA;
      const auto hit = HitTest(pt);
      Log(L"bar", L"wheel notches=%d hit=%hs", notches, hit ? hit->id.c_str() : "none");
      if (notches == 0 || !hit) {
        return 0;
      }
      // 휠을 받는 항목은 볼륨뿐이다. 다른 세그먼트는 scroll을 무시하지만
      // 이벤트를 아예 보내지 않아 로그와 디스패치를 줄인다.
      if (hit->id != "bamti.widget/volume") {
        return 0;
      }
      StatusEvent ev;
      ev.id = hit->id;
      ev.event = "scroll";
      ev.row_id = "volume_level";
      ev.value = static_cast<float>(notches) * 0.02f;
      status_.Dispatch(ev);
      return 0;
    }
    case WM_LBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (reorder_active_) {
        FinishReorder();
        return 0;
      }
      const bool start_click = start_pressed_ && HitStart(pt);
      const bool spotlight_click = spotlight_pressed_ && HitSpotlight(pt);
      const bool cc_click = cc_pressed_ && HitControlCenter(pt);
      if (start_pressed_) {
        start_pressed_ = false;
        ReleaseCapture();
        InvalidateArea(hwnd_, StartRect());
      }
      if (spotlight_pressed_) {
        spotlight_pressed_ = false;
        ReleaseCapture();
        InvalidateArea(hwnd_, SpotlightRect());
      }
      if (cc_pressed_) {
        cc_pressed_ = false;
        ReleaseCapture();
        InvalidateArea(hwnd_, ControlCenterRect());
      }
      if (start_click) {
        ToggleStartMenu();
        return 0;
      }
      if (spotlight_click) {
        ToggleSpotlight();
        return 0;
      }
      if (cc_click) {
        ToggleControlCenter();
        return 0;
      }
      if (skip_left_up_) {
        skip_left_up_ = false;
        return 0;
      }
      if (const BarSegment* seg = HitSegment(pt)) {
        if (seg->kind == SegmentKind::kOverflow) {
          OpenOverflow();
          return 0;
        }
      }
      if (const auto hit = HitTest(pt)) {
        status_popup_.Close();
        StatusEvent ev;
        ev.id = hit->id;
        ev.event = "click";
        ev.button = "left";
        status_.Dispatch(ev);
        OpenStatusPanel(*hit);
      }
      return 0;
    }
    case WM_LBUTTONDBLCLK: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (HitStart(pt)) {
        return 0;
      }
      if (const auto hit = HitTest(pt)) {
        skip_left_up_ = true;
        status_popup_.Close();
        StatusEvent ev;
        ev.id = hit->id;
        ev.event = "dblclick";
        ev.button = "left";
        status_.Dispatch(ev);
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (const auto hit = HitTest(pt)) {
        if (hit->id.rfind("bamti.tray/", 0) == 0) {
          POINT screen = pt;
          ClientToScreen(hwnd_, &screen);
          if (tray_.ForwardsContextMenu()) {
            status_popup_.Close();
            StatusEvent ev;
            ev.id = hit->id;
            ev.event = "click";
            ev.button = "right";
            status_.Dispatch(ev);
            return 0;
          }
          ShowTrayIconMenu(screen, hit->id);
          return 0;
        }
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
    case WM_COMMAND: {
      const UINT cmd = LOWORD(wparam);
      if (cmd == kExitCommand) {
        DestroyWindow(hwnd_);
        return 0;
      }
      if (cmd == kWidgetBatteryCmd || cmd == kWidgetCpuCmd || cmd == kWidgetNetworkCmd ||
          cmd == kWidgetVolumeCmd || cmd == kWidgetBoardCmd || cmd == kWidgetControlCenterCmd) {
        WidgetSettings next = widgets_.settings();
        const WidgetSettings tray = tray_.settings();
        next.tray_mirror = tray.tray_mirror;
        next.tray_system_icons = tray.tray_system_icons;
        next.tray_overflow_icons = tray.tray_overflow_icons;
        next.tray_backend = tray.tray_backend;
        next.tray_hidden_keys = tray.tray_hidden_keys;
        if (cmd == kWidgetBatteryCmd) {
          next.battery = !next.battery;
        } else if (cmd == kWidgetCpuCmd) {
          next.cpu = !next.cpu;
        } else if (cmd == kWidgetNetworkCmd) {
          next.network = !next.network;
        } else if (cmd == kWidgetVolumeCmd) {
          next.volume = !next.volume;
        } else if (cmd == kWidgetControlCenterCmd) {
          next.control_center = !next.control_center;
        } else if (cmd == kWidgetBoardCmd) {
          next.widget_board = !next.widget_board;
        }
        ApplySettings(next);
      }
      if (cmd == kTrayMirrorToggleCmd || cmd == kTraySystemIconsCmd || cmd == kTrayOverflowIconsCmd) {
        WidgetSettings next = widgets_.settings();
        const WidgetSettings tray = tray_.settings();
        next.tray_mirror = tray.tray_mirror;
        next.tray_system_icons = tray.tray_system_icons;
        next.tray_overflow_icons = tray.tray_overflow_icons;
        next.tray_backend = tray.tray_backend;
        next.tray_hidden_keys = tray.tray_hidden_keys;
        if (cmd == kTrayMirrorToggleCmd) {
          next.tray_mirror = !next.tray_mirror;
        } else if (cmd == kTraySystemIconsCmd) {
          next.tray_system_icons = !next.tray_system_icons;
        } else {
          next.tray_overflow_icons = !next.tray_overflow_icons;
        }
        ApplySettings(next);
      }
      if (cmd == kTrayInterceptCmd) {
        WidgetSettings next = widgets_.settings();
        const WidgetSettings tray = tray_.settings();
        next.tray_mirror = tray.tray_mirror;
        next.tray_system_icons = tray.tray_system_icons;
        next.tray_overflow_icons = tray.tray_overflow_icons;
        next.tray_backend = tray.tray_backend == "intercept" ? "uia" : "intercept";
        next.tray_hidden_keys = tray.tray_hidden_keys;
        ApplySettings(next);
      }
      if (cmd == kTrayPeekCmd) {
        StartTrayPeek();
      }
      if (cmd == kAutostartCmd) {
        if (!SetAutostart(!AutostartEnabled())) {
          Log(L"bar", L"autostart toggle failed");
        }
      }
      if (cmd == kTrayHideIconCmd) {
        const uint64_t key = TrayMirror::ParseId(tray_menu_id_);
        if (key != 0) {
          WidgetSettings next = widgets_.settings();
          const WidgetSettings tray = tray_.settings();
          next.tray_mirror = tray.tray_mirror;
          next.tray_system_icons = tray.tray_system_icons;
          next.tray_overflow_icons = tray.tray_overflow_icons;
          next.tray_backend = tray.tray_backend;
          next.tray_hidden_keys = tray.tray_hidden_keys;
          const std::string hex = TrayMirror::KeyText(key);
          bool have = false;
          for (const std::string& one : next.tray_hidden_keys) {
            if (one == hex) {
              have = true;
              break;
            }
          }
          if (!have) {
            next.tray_hidden_keys.push_back(hex);
            if (next.tray_hidden_keys.size() > kTrayHiddenKeysMax) {
              next.tray_hidden_keys.erase(next.tray_hidden_keys.begin());
            }
          }
          ApplySettings(next);
        }
      }
      if (cmd >= kTrayItemCmdBase && cmd < kTrayItemCmdBase + kTrayHiddenKeysMax) {
        const size_t idx = static_cast<size_t>(cmd - kTrayItemCmdBase);
        if (idx < tray_menu_keys_.size()) {
          const uint64_t key = tray_menu_keys_[idx];
          WidgetSettings next = widgets_.settings();
          const WidgetSettings tray = tray_.settings();
          next.tray_mirror = tray.tray_mirror;
          next.tray_system_icons = tray.tray_system_icons;
          next.tray_overflow_icons = tray.tray_overflow_icons;
          next.tray_backend = tray.tray_backend;
          next.tray_hidden_keys = tray.tray_hidden_keys;
          const std::string hex = TrayMirror::KeyText(key);
          std::vector<std::string> kept;
          kept.reserve(next.tray_hidden_keys.size());
          bool have = false;
          for (const std::string& one : next.tray_hidden_keys) {
            uint64_t parsed = 0;
            if (one.size() >= 2 && one[0] == '0' && (one[1] == 'x' || one[1] == 'X')) {
              parsed = static_cast<uint64_t>(strtoull(one.c_str() + 2, nullptr, 16));
            } else {
              parsed = static_cast<uint64_t>(strtoull(one.c_str(), nullptr, 16));
            }
            if (parsed == key || one == hex) {
              have = true;
              continue;
            }
            kept.push_back(one);
          }
          if (have) {
            next.tray_hidden_keys = std::move(kept);
          } else {
            next.tray_hidden_keys.push_back(hex);
            if (next.tray_hidden_keys.size() > kTrayHiddenKeysMax) {
              next.tray_hidden_keys.erase(next.tray_hidden_keys.begin());
            }
          }
          ApplySettings(next);
        }
      }
      if (cmd == kTrayMirrorOffCmd) {
        WidgetSettings next = widgets_.settings();
        const WidgetSettings tray = tray_.settings();
        next.tray_mirror = false;
        next.tray_system_icons = tray.tray_system_icons;
        next.tray_overflow_icons = tray.tray_overflow_icons;
        next.tray_backend = tray.tray_backend;
        next.tray_hidden_keys = tray.tray_hidden_keys;
        ApplySettings(next);
      }
      return 0;
    }
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
        KillTimer(hwnd_, kPeekTimerId);
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
      KillTimer(hwnd_, kPeekTimerId);
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
      if (g_taskbar_created != 0 && msg == g_taskbar_created) {
        tray_.OnExplorerRestart();
        TaskbarController::RewatchTray();
        taskbar_.EnsureHidden();
        return 0;
      }
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
      layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), OrderedItems(),
                      widgets_.settings().control_center);
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
      layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), OrderedItems(),
                      widgets_.settings().control_center);
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
                  start_hot_ || start_menu_.visible(), start_pressed_ || start_menu_.visible(),
                  spotlight_hot_ || spotlight_.visible(), spotlight_pressed_ || spotlight_.visible(),
                  cc_hot_ || cc_open_, cc_pressed_ || cc_open_, &draw);
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

RECT MenuBar::SpotlightRect() const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kSpotlight) {
      return seg.rect;
    }
  }
  return {};
}

RECT MenuBar::ControlCenterRect() const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kControlCenter) {
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

std::vector<StatusItem> MenuBar::OrderedItems() const {
  std::vector<StatusItem> items = status_.Snapshot();
  const std::vector<std::string>& order = reorder_active_ ? reorder_order_ : bar_order_;
  if (order.empty()) {
    return items;
  }
  std::vector<StatusItem> out;
  out.reserve(items.size());
  std::vector<char> used(items.size(), 0);
  for (const std::string& id : order) {
    for (size_t i = 0; i < items.size(); ++i) {
      if (!used[i] && items[i].id == id) {
        used[i] = 1;
        out.push_back(std::move(items[i]));
        break;
      }
    }
  }
  for (size_t i = 0; i < items.size(); ++i) {
    if (!used[i]) {
      out.push_back(std::move(items[i]));
    }
  }
  return out;
}

bool MenuBar::ReorderCursor(POINT client) const {
  if (reorder_active_) {
    return true;
  }
  if ((GetKeyState(VK_CONTROL) & 0x8000) == 0) {
    return false;
  }
  const BarSegment* seg = HitSegment(client);
  return seg != nullptr && seg->kind == SegmentKind::kStatus;
}

void MenuBar::BeginReorder(const std::string& id, POINT pt) {
  reorder_active_ = true;
  reorder_moved_ = false;
  reorder_id_ = id;
  reorder_start_ = pt;
  reorder_order_ = bar_order_;
  const std::vector<StatusItem> snap = status_.Snapshot();
  for (const StatusItem& item : snap) {
    if (!OrderContains(reorder_order_, item.id)) {
      reorder_order_.push_back(item.id);
    }
  }
  if (reorder_order_.size() > kBarOrderMax) {
    reorder_order_.resize(kBarOrderMax);
  }
  status_popup_.Close();
  SetCapture(hwnd_);
}

bool MenuBar::UpdateReorder(POINT pt) {
  std::vector<const BarSegment*> slots;
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kStatus) {
      slots.push_back(&seg);
    }
  }
  if (slots.size() < 2) {
    return false;
  }
  size_t from = slots.size();
  for (size_t i = 0; i < slots.size(); ++i) {
    if (slots[i]->id == reorder_id_) {
      from = i;
      break;
    }
  }
  if (from == slots.size()) {
    return false;
  }
  size_t to = slots.size();
  for (size_t i = 0; i < slots.size(); ++i) {
    const LONG mid = slots[i]->rect.left + (slots[i]->rect.right - slots[i]->rect.left) / 2;
    if (pt.x < mid) {
      to = i;
      break;
    }
  }
  if (to > from) {
    --to;
  }
  if (to == from) {
    return false;
  }
  std::vector<std::string> visual_ltr;
  visual_ltr.reserve(slots.size());
  for (const BarSegment* seg : slots) {
    visual_ltr.push_back(seg->id);
  }
  const std::string id = visual_ltr[from];
  visual_ltr.erase(visual_ltr.begin() + static_cast<std::ptrdiff_t>(from));
  visual_ltr.insert(visual_ltr.begin() + static_cast<std::ptrdiff_t>(to), id);
  const std::vector<std::string> visual_rtl(visual_ltr.rbegin(), visual_ltr.rend());
  std::vector<std::string> next = ApplyVisiblePermutation(reorder_order_, visual_rtl);
  if (next == reorder_order_) {
    return false;
  }
  reorder_order_ = std::move(next);
  return true;
}

void MenuBar::CancelReorder() {
  if (!reorder_active_) {
    return;
  }
  reorder_active_ = false;
  reorder_moved_ = false;
  reorder_id_.clear();
  reorder_order_.clear();
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  RefreshLayout();
}

void MenuBar::FinishReorder() {
  if (!reorder_active_) {
    return;
  }
  const bool moved = reorder_moved_;
  std::vector<std::string> order = std::move(reorder_order_);
  reorder_active_ = false;
  reorder_moved_ = false;
  reorder_id_.clear();
  reorder_order_.clear();
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  if (moved) {
    if (order.size() > kBarOrderMax) {
      order.resize(kBarOrderMax);
    }
    bar_order_ = std::move(order);
    WidgetSettings next = widgets_.settings();
    const WidgetSettings tray = tray_.settings();
    next.tray_mirror = tray.tray_mirror;
    next.tray_system_icons = tray.tray_system_icons;
    next.tray_overflow_icons = tray.tray_overflow_icons;
    next.tray_backend = tray.tray_backend;
    next.tray_hidden_keys = tray.tray_hidden_keys;
    next.bar_order = bar_order_;
    SaveWidgetSettings(next);
  }
  RefreshLayout();
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
  cc_open_ = false;
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

bool MenuBar::HitSpotlight(POINT client) const {
  const RECT rect = SpotlightRect();
  return PtInRect(&rect, client) != FALSE;
}

bool MenuBar::HitControlCenter(POINT client) const {
  const RECT rect = ControlCenterRect();
  return PtInRect(&rect, client) != FALSE;
}

void MenuBar::UpdateChrome(POINT client) {
  ArmMouseLeave();
  const bool start_hot = HitStart(client);
  if (start_hot_ != start_hot) {
    start_hot_ = start_hot;
    InvalidateArea(hwnd_, StartRect());
  }
  const bool spotlight_hot = HitSpotlight(client);
  if (spotlight_hot_ != spotlight_hot) {
    spotlight_hot_ = spotlight_hot;
    InvalidateArea(hwnd_, SpotlightRect());
  }
  const bool cc_hot = HitControlCenter(client);
  if (cc_hot_ != cc_hot) {
    cc_hot_ = cc_hot;
    InvalidateArea(hwnd_, ControlCenterRect());
  }
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
  cc_open_ = false;
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateArea(hwnd_, StartRect());
  }
  spotlight_.Toggle(hwnd_, dark_);
}

void MenuBar::ToggleControlCenter() {
  if (fullscreen_occluded_ || !widgets_.settings().control_center) {
    return;
  }
  if (cc_open_ && status_popup_.IsOpen()) {
    status_popup_.Close();
    cc_open_ = false;
    InvalidateArea(hwnd_, ControlCenterRect());
    return;
  }
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateArea(hwnd_, StartRect());
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  if (cc_panel_ == nullptr) {
    return;
  }
  ControlCenterHost host;
  host.dark = dark_;
  host.dispatch = [this](const StatusEvent& ev) { status_.Dispatch(ev); };
  host.live = [this]() { return widgets_.LiveForControlCenter(); };
  cc_panel_->Reset(std::move(host));
  RECT rc = ControlCenterRect();
  POINT anchor{rc.left, rc.bottom};
  ClientToScreen(hwnd_, &anchor);
  cc_open_ = true;
  open_panel_id_.clear();
  status_popup_.SetDark(dark_);
  if (!status_popup_.Open(cc_panel_.get(), anchor, PopupSurface::Anchor::BelowAt)) {
    cc_open_ = false;
    Log(L"cc", L"open failed");
    return;
  }
  QueryPerformanceCounter(&t1);
  Log(L"cc", L"open to present %.2f ms", QpcMs(t0, t1));
  InvalidateArea(hwnd_, ControlCenterRect());
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
  cc_open_ = false;
  status_panel_->Reset(*found, MakePanelHost());
  POINT anchor{hit.rect.left, hit.rect.bottom};
  ClientToScreen(hwnd_, &anchor);
  open_panel_id_ = found->id;
  open_panel_revision_ = found->revision;
  open_panel_rows_ = PanelRowTypes(*found->panel);
  StatusEvent ev;
  ev.id = found->id;
  ev.event = "panel_open";
  status_.Dispatch(ev);
  status_popup_.Open(status_panel_.get(), anchor, PopupSurface::Anchor::BelowAt);
}

StatusPanelHost MenuBar::MakePanelHost() {
  StatusPanelHost host;
  host.dark = dark_;
  host.dispatch = [this](const StatusEvent& ev) { status_.Dispatch(ev); };
  host.arm_toggle = [this](std::string id, std::string row_id, uint64_t revision, bool on) {
    ArmToggle(std::move(id), std::move(row_id), revision, on);
  };
  host.arm_slider = [this](std::string id, std::string row_id, uint64_t revision, float value) {
    ArmSlider(std::move(id), std::move(row_id), revision, value);
  };
  return host;
}

void MenuBar::RefreshOpenPanel() {
  if (status_popup_.Dragging()) {
    return;
  }
  if (cc_open_) {
    if (!status_popup_.IsOpen() || cc_panel_ == nullptr) {
      cc_open_ = false;
      return;
    }
    cc_panel_->Refresh();
    if (status_popup_.hwnd() != nullptr) {
      status_popup_.Present();
    }
    return;
  }
  if (!status_popup_.IsOpen() || open_panel_id_.empty() || status_panel_ == nullptr) {
    return;
  }
  auto item = status_.Get(open_panel_id_);
  if (!item || !item->panel) {
    status_popup_.Close();
    return;
  }
  if (toggle_armed_ && item->revision == pending_toggle_.revision) {
    return;
  }
  if (slider_armed_ && item->revision == pending_slider_.revision) {
    return;
  }
  if (item->revision == open_panel_revision_) {
    return;
  }
  if (PanelRowTypes(*item->panel) != open_panel_rows_) {
    status_popup_.Close();
    return;
  }
  open_panel_revision_ = item->revision;
  slider_armed_ = false;
  status_panel_->Reset(std::move(*item), MakePanelHost());
  if (status_popup_.hwnd() != nullptr) {
    status_popup_.Present();
  }
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

void MenuBar::ArmSlider(std::string id, std::string row_id, uint64_t revision, float value) {
  pending_slider_.id = std::move(id);
  pending_slider_.row_id = std::move(row_id);
  pending_slider_.revision = revision;
  pending_slider_.value = value;
  slider_armed_ = true;
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
  item->revision += 1;
  status_.Upsert(std::move(*item));
  RefreshOpenPanel();
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
  // 7단계에서 항목 표시 설정 전체를 PopupSurface로 옮길 임시 메뉴다.
  const WidgetSettings s = widgets_.settings();
  AppendMenuW(menu, MF_STRING | (s.battery ? MF_CHECKED : 0), kWidgetBatteryCmd, L"배터리");
  AppendMenuW(menu, MF_STRING | (s.cpu ? MF_CHECKED : 0), kWidgetCpuCmd, L"CPU");
  AppendMenuW(menu, MF_STRING | (s.network ? MF_CHECKED : 0), kWidgetNetworkCmd, L"네트워크");
  AppendMenuW(menu, MF_STRING | (s.volume ? MF_CHECKED : 0), kWidgetVolumeCmd, L"볼륨");
  AppendMenuW(menu, MF_STRING | (s.control_center ? MF_CHECKED : 0), kWidgetControlCenterCmd, L"제어 센터");
  const bool board_ok = IsWidgetBoardAvailable();
  UINT board_flags = MF_STRING | (s.widget_board ? MF_CHECKED : 0);
  if (!board_ok) {
    board_flags |= MF_GRAYED;
  }
  AppendMenuW(menu, board_flags, kWidgetBoardCmd,
              board_ok ? L"위젯 보드 단추" : L"위젯 보드 단추 (이 PC에서 사용할 수 없습니다)");
  const WidgetSettings tray = tray_.settings();
  AppendMenuW(menu, MF_STRING | (tray.tray_mirror ? MF_CHECKED : 0), kTrayMirrorToggleCmd, L"트레이 미러");
  AppendMenuW(menu, MF_STRING | (tray.tray_system_icons ? MF_CHECKED : 0), kTraySystemIconsCmd, L"시스템 아이콘도 표시");
  AppendMenuW(menu, MF_STRING | (tray.tray_overflow_icons ? MF_CHECKED : 0), kTrayOverflowIconsCmd, L"숨긴 아이콘도 표시");
  AppendMenuW(menu, MF_STRING | (tray.tray_backend == "intercept" ? MF_CHECKED : 0), kTrayInterceptCmd,
              L"트레이 아이콘 가로채기(실험)");
  const HMENU tray_items = CreatePopupMenu();
  tray_menu_keys_.clear();
  if (tray_items != nullptr) {
    const std::vector<TrayMirror::MenuItem> entries = tray_.MenuItems();
    if (entries.empty()) {
      AppendMenuW(tray_items, MF_STRING | MF_GRAYED, 0, L"미러 중인 아이콘이 없습니다");
    } else {
      const size_t n = (std::min)(entries.size(), kTrayHiddenKeysMax);
      tray_menu_keys_.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        const UINT flags = MF_STRING | (entries[i].shown ? MF_CHECKED : 0);
        AppendMenuW(tray_items, flags, kTrayItemCmdBase + static_cast<UINT>(i), entries[i].label.c_str());
        tray_menu_keys_.push_back(entries[i].key);
      }
      if (entries.size() > kTrayHiddenKeysMax) {
        AppendMenuW(tray_items, MF_STRING | MF_GRAYED, 0, L"이하 생략");
      }
    }
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(tray_items), L"트레이 아이콘");
  }
  AppendMenuW(menu, MF_STRING, kTrayPeekCmd, L"알림 영역 잠시 표시");
  AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kAutostartCmd,
              L"로그인 시 bamti 시작");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING, kExitCommand, L"종료");
  const HWND prev = GetForegroundWindow();
  SetForegroundWindow(hwnd_);
  TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, screen.x, screen.y, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  if (GetForegroundWindow() == hwnd_ && prev != nullptr && prev != hwnd_) {
    SetForegroundWindow(prev);
  }
  DestroyMenu(menu);
}

void MenuBar::ShowTrayIconMenu(POINT screen, const std::string& id) {
  const HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  tray_menu_id_ = id;
  AppendMenuW(menu, MF_STRING, kTrayPeekCmd, L"알림 영역 잠시 표시");
  AppendMenuW(menu, MF_STRING, kTrayHideIconCmd, L"이 아이콘 숨기기");
  AppendMenuW(menu, MF_STRING, kTrayMirrorOffCmd, L"트레이 미러 끄기");
  AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"앱 메뉴는 알림 영역 잠시 표시로 엽니다");
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"숨긴 아이콘도 미러합니다. 클릭 반응이 없으면 알림 영역 잠시 표시로 여세요");
  AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"지금은 글리프만 표시합니다");
  const HWND prev = GetForegroundWindow();
  SetForegroundWindow(hwnd_);
  TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, screen.x, screen.y, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  if (GetForegroundWindow() == hwnd_ && prev != nullptr && prev != hwnd_) {
    SetForegroundWindow(prev);
  }
  DestroyMenu(menu);
}

void MenuBar::ApplySettings(const WidgetSettings& next) {
  WidgetSettings merged = next;
  merged.bar_order = bar_order_;
  widgets_.SetSettings(merged);
  tray_.SetSettings(merged);
  if (!merged.control_center && cc_open_) {
    status_popup_.Close();
    cc_open_ = false;
  }
  RefreshLayout();
}

void MenuBar::StartTrayPeek() {
  status_popup_.Close();
  tray_peeking_ = true;
  tray_.SetActive(false);
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kPeekTimerId, kPeekMs, nullptr);
  }
  TaskbarController::BeginPeek(kPeekMs + 2000);
  taskbar_.Restore();
}

void MenuBar::EndTrayPeek() {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kPeekTimerId);
  }
  if (!tray_peeking_) {
    return;
  }
  tray_peeking_ = false;
  taskbar_.Hide();
  TaskbarController::EndPeek();
  if (providers_active_) {
    tray_.SetActive(true);
  }
}

void MenuBar::RefreshFullscreenState() {
  SetFullscreenOccluded(IsTrueFullscreen(hwnd_));
}

void MenuBar::UpdateProviderActive() {
  const bool active = !fullscreen_occluded_ && !session_locked_ && display_on_;
  if (active == providers_active_) {
    if (tray_peeking_ && active) {
      tray_.SetActive(false);
    }
    return;
  }
  providers_active_ = active;
  status_.SetActive(active);
  if (tray_peeking_) {
    tray_.SetActive(false);
  }
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
