#include "menu_bar.hpp"

#include "autostart.hpp"
#include "clock_flyout.hpp"
#include "control_center.hpp"
#include "dwm.hpp"
#include "fullscreen.hpp"
#include "log.hpp"
#include "settings.hpp"
#include "theme.hpp"
#include "tray_popup_guard.hpp"
#include "start_menu.hpp"
#include "watchdog.hpp"
#include "winx_menu.hpp"

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
constexpr UINT kTrayHideIconCmd = 21;
constexpr UINT kTrayMirrorOffCmd = 22;
constexpr UINT kWidgetBluetoothCmd = 23;
constexpr UINT kTrayPeekCmd = 24;
constexpr UINT kSettingsCmd = 25;
constexpr UINT kMenuWidgetsSubCmd = 30;
constexpr UINT kMenuTraySubCmd = 31;
// 값 순서는 StartAction과 같아야 한다.
constexpr UINT kStartExplorerCmd = 40;
constexpr UINT kStartSettingsCmd = 41;
constexpr UINT kStartRunCmd = 42;
constexpr UINT kStartSleepCmd = 43;
constexpr UINT kStartRestartCmd = 44;
constexpr UINT kStartShutdownCmd = 45;
constexpr UINT kTrayItemCmdBase = 4000;
constexpr UINT kWinXCmdBase = 5000;
constexpr UINT kPowerSubCmd = 5199;
constexpr UINT kPowerCmdBase = 5200;
constexpr UINT_PTR kPeekTimerId = 4;
constexpr UINT kPeekMs = 10000;
constexpr UINT_PTR kDesktopPeekDwellTimerId = 5;
constexpr UINT_PTR kTransitionsTimerId = 6;
constexpr UINT_PTR kCornerWatchTimerId = 7;
constexpr UINT_PTR kCtrlPollTimerId = 9;
constexpr UINT kDesktopPeekDwellMs = 120;
constexpr UINT kTransitionsRestoreMs = 400;
constexpr UINT kCornerWatchMs = 30;
constexpr UINT kCtrlPollMs = 200;
constexpr ULONGLONG kCtrlHookGraceMs = 600;
constexpr int kPeekZoneDip = 14;  // bar_layout.cpp의 kPadRightDip과 같다.

enum AccentState : DWORD {
  kAccentDisabled = 0,
  kAccentEnableGradient = 1,
  kAccentEnableTransparentGradient = 2,
  kAccentEnableBlurBehind = 3,
  kAccentEnableAcrylicBlurBehind = 4,
  kAccentEnableHostBackdrop = 5,
};

// 화면이 검으면 실패다. 4 → 3 → 2 순으로 시험한다.
constexpr DWORD kBarAccentState = kAccentEnableAcrylicBlurBehind;

struct AccentPolicy {
  DWORD state;
  DWORD flags;
  DWORD gradient_color;  // AABBGGRR 이다. RGB 가 아니라 BGR 순서인 것에 주의한다.
  DWORD animation_id;
};

struct WindowCompositionAttributeData {
  DWORD attrib;  // 19 = WCA_ACCENT_POLICY
  PVOID data;
  SIZE_T size;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

SetWindowCompositionAttributeFn LoadSetWindowCompositionAttribute() {
  static const SetWindowCompositionAttributeFn fn = []() -> SetWindowCompositionAttributeFn {
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
      Log(L"bar", L"composition user32 missing err=%lu", GetLastError());
      return nullptr;
    }
    auto found = reinterpret_cast<SetWindowCompositionAttributeFn>(
        GetProcAddress(user32, "SetWindowCompositionAttribute"));
    Log(L"bar", L"composition GetProcAddress %s err=%lu", found != nullptr ? L"ok" : L"failed",
        found != nullptr ? 0ul : GetLastError());
    return found;
  }();
  return fn;
}

void ApplyAccentPolicy(HWND hwnd, bool dark) {
  const auto fn = LoadSetWindowCompositionAttribute();
  if (fn == nullptr) {
    return;
  }
  AccentPolicy policy{};
  policy.state = kBarAccentState;
  policy.flags = 0;
  policy.gradient_color = dark ? 0x99000000u : 0x99FFFFFFu;
  policy.animation_id = 0;
  WindowCompositionAttributeData data{};
  data.attrib = 19;
  data.data = &policy;
  data.size = sizeof(policy);
  SetLastError(ERROR_SUCCESS);
  const BOOL ok = fn(hwnd, &data);
  const DWORD err = GetLastError();
  Log(L"bar", L"composition set ok=%d err=%lu state=%lu color=0x%08lx", ok ? 1 : 0, err, policy.state,
      policy.gradient_color);
}
constexpr int kPeekRearmZoneDip = 96;  // 걸쇠를 다시 걸 수 있게 되는 거리.
constexpr char kSpotlightItemId[] = "bamti.widget/spotlight";
constexpr char kControlCenterItemId[] = "bamti.widget/control_center";
constexpr char kNetworkItemId[] = "bamti.widget/network";
constexpr char kVolumeItemId[] = "bamti.widget/volume";
constexpr char kBluetoothItemId[] = "bamti.widget/bluetooth";
constexpr char kBatteryItemId[] = "bamti.widget/battery";
constexpr char kCpuItemId[] = "bamti.widget/cpu";

struct WidgetPage {
  const char* id;
  ControlCenterPage page;
  bool WidgetSettings::* enabled;
};

constexpr WidgetPage kWidgetPages[] = {
    {kNetworkItemId, ControlCenterPage::kWifi, &WidgetSettings::network},
    {kVolumeItemId, ControlCenterPage::kVolume, &WidgetSettings::volume},
    {kBluetoothItemId, ControlCenterPage::kBluetooth, &WidgetSettings::bluetooth},
    {kBatteryItemId, ControlCenterPage::kBattery, &WidgetSettings::battery},
    {kCpuItemId, ControlCenterPage::kCpu, &WidgetSettings::cpu},
};

const WidgetPage* FindWidgetPage(const std::string& id) {
  for (const WidgetPage& page : kWidgetPages) {
    if (id == page.id) {
      return &page;
    }
  }
  return nullptr;
}

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
bool g_ctrl_held = false;
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

bool IsChromeItemId(const std::string& id) {
  return id == kSpotlightItemId || id == kControlCenterItemId;
}

StatusItem MakeChromeItem(const char* id, VectorIcon vector, const wchar_t* tip) {
  StatusItem item;
  item.id = id;
  item.source = "bamti";
  item.icon.kind = IconKind::kVector;
  item.icon.vector = vector;
  item.icon.cache_key = HashStatusIcon(item.icon);
  item.tooltip = tip;
  return item;
}

std::vector<StatusItem> ChromeItems(bool show_control_center) {
  std::vector<StatusItem> items;
  if (show_control_center) {
    items.push_back(MakeChromeItem(kControlCenterItemId, VectorIcon::kControlCenter, L"제어 센터"));
  }
  items.push_back(MakeChromeItem(kSpotlightItemId, VectorIcon::kSearch, L"검색"));
  return items;
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
  if (code != HC_ACTION) {
    return CallNextHookEx(g_key_hook, code, wparam, lparam);
  }
  const auto* info = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
  if (info == nullptr || (info->flags & LLKHF_INJECTED) != 0) {
    return CallNextHookEx(g_key_hook, code, wparam, lparam);
  }

  const bool down = wparam == WM_KEYDOWN || wparam == WM_SYSKEYDOWN;
  const bool up = wparam == WM_KEYUP || wparam == WM_SYSKEYUP;
  const DWORD vk = info->vkCode;
  const bool is_ctrl = vk == VK_CONTROL || vk == VK_LCONTROL || vk == VK_RCONTROL;
  if (is_ctrl) {
    if (down) {
      if (!g_ctrl_held) {
        g_ctrl_held = true;
        if (g_menu_bar != nullptr && g_menu_bar->hwnd() != nullptr) {
          PostMessageW(g_menu_bar->hwnd(), kCornerWatchMsg, 1, 0);
        }
      }
    } else if (up) {
      g_ctrl_held = false;
    }
  }
  if (g_menu_bar == nullptr || g_menu_bar->hwnd() == nullptr || !g_menu_bar->win_key_enabled()) {
    return CallNextHookEx(g_key_hook, code, wparam, lparam);
  }
  if (g_win_held) {
    const bool win_really_down = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 ||
                                 (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
    if (!win_really_down) {
      g_win_held = false;
      g_win_combo = false;
      g_win_injected = false;
    }
  }
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
      cc_panel_(std::make_unique<ControlCenterContent>()),
      clock_panel_(std::make_unique<ClockFlyoutContent>()),
      clock_menu_(std::make_unique<ClockMenuContent>()) {}

MenuBar::~MenuBar() {
  RemoveWinHook();
  status_.StopAll();
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
  }
  spotlight_.Hide();
  status_popup_.Destroy();
  bar_submenu_popup_.Destroy();
  ReleaseLayeredTarget();
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
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kMenuBarClass;
  if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  if (!layout_.Initialize() || !clock_.Initialize()) {
    return false;
  }

  dark_ = ShellUsesDarkMode();

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED, kMenuBarClass, L"bamti",
                          WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  if (!hwnd_) {
    return false;
  }

  layout_.SetDpi(Dpi());
  clock_.SetDpi(Dpi());
  clock_.WarmTarget();  // SetDpi가 렌더 타깃을 버리므로 반드시 그 뒤에서 부른다
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
    for (const BarSegment& seg : layout_.last().segments) {
      if (seg.kind == SegmentKind::kStatus && TrayMirror::ParseId(seg.id) == key) {
        return SegmentScreenRect(seg.id, out);
      }
    }
    return false;
  });
  session_notify_ = WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION) != FALSE;
  display_notify_ = RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE);
  if (!status_popup_.Create(instance, hwnd_)) {
    return false;
  }
  if (!bar_submenu_popup_.Create(instance, hwnd_)) {
    Log(L"bar", L"submenu create failed err=%lu", GetLastError());
    return false;
  }
  status_popup_.SetDark(dark_);
  bar_submenu_popup_.SetDark(dark_);
  Layout();
  taskbar_.Restore();
  ShowWindow(hwnd_, SW_SHOWNA);
  ApplyBackdrop();
  Present();
  taskbar_.Hide();
  Log(L"bar", L"ready hwnd=%p taskbar_hidden=%d", hwnd_, taskbar_.hidden() ? 1 : 0);
  spotlight_.Warmup(hwnd_, dark_);
  InstallWinHook();
  SetTimer(hwnd_, kClockTimerId, 1000, nullptr);
  SetTimer(hwnd_, kCtrlPollTimerId, kCtrlPollMs, nullptr);
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
      if (wparam == kDesktopPeekDwellTimerId) {
        KillTimer(hwnd_, kDesktopPeekDwellTimerId);
        peek_dwell_armed_ = false;
        Log(L"peek", L"dwell fire");
        if (DesktopPeekWanted() && CornerHit()) {
          StartDesktopPeek();
        }
        return 0;
      }
      if (wparam == kTransitionsTimerId) {
        KillTimer(hwnd_, kTransitionsTimerId);
        desktop_toggle_.RestoreTransitions();
        return 0;
      }
      if (wparam == kCtrlPollTimerId) {
        if (CtrlHeld() && !corner_watch_on_) {
          StartCornerWatch();
        }
        return 0;
      }
      if (wparam == kCornerWatchTimerId) {
        if (!CtrlHeld()) {
          StopCornerWatch();
          return 0;
        }
        POINT pt{};
        RECT rc{};
        const bool got = GetCursorPos(&pt) != FALSE && GetWindowRect(hwnd_, &rc) != FALSE;
        const bool hit = got && PtInRect(&rc, pt) != FALSE && pt.x >= rc.right - DipToPx(kPeekZoneDip, Dpi());
        if (hit != last_corner_hit_) {
          last_corner_hit_ = hit;
          Log(L"peek", L"corner in=%d x=%ld y=%ld right=%ld", hit ? 1 : 0, pt.x, pt.y, rc.right);
        }
        if (hit) {
          UpdateDesktopPeek();
        } else {
          if (hwnd_ != nullptr) {
            KillTimer(hwnd_, kDesktopPeekDwellTimerId);
          }
          peek_dwell_armed_ = false;
          if (got && pt.x < rc.right - DipToPx(kPeekRearmZoneDip, Dpi())) {
            StopDesktopPeek(L"corner-left");
          }
        }
        return 0;
      }
      return 0;
    case kPopupClosedMsg:
      if (!status_popup_.IsOpen()) {
        CloseBarSubmenu(L"parent");
        status_popup_.SetAfterTick(nullptr, nullptr);
        if (cc_panel_ != nullptr) {
          cc_panel_->Dismissed();
        }
        cc_open_ = false;
        clock_open_ = false;
        const bool start_was_open = start_popup_open_;
        start_popup_open_ = false;
        if (start_was_open) {
          Present();
        }
        if (!open_panel_id_.empty()) {
          StatusEvent ev;
          ev.id = std::move(open_panel_id_);
          ev.event = "panel_close";
          status_.Dispatch(ev);
          open_panel_id_.clear();
        }
      }
      return 0;
    case kCornerWatchMsg:
      Log(L"peek", L"watch msg");
      StartCornerWatch();
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
      Present();
      return 0;
    case WM_DISPLAYCHANGE:
    case WM_SETTINGCHANGE:
      if (msg == WM_SETTINGCHANGE) {
        const wchar_t* area = reinterpret_cast<const wchar_t*>(lparam);
        if (area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0) {
          dark_ = ShellUsesDarkMode();
          ApplyBackdrop();
          status_popup_.SetDark(dark_);
          bar_submenu_popup_.SetDark(dark_);
          if (status_popup_.IsOpen() && bar_menu_ != nullptr) {
            bar_menu_->SetDark(dark_);
            status_popup_.Present();
          }
          if (bar_submenu_popup_.IsOpen() && bar_submenu_ != nullptr) {
            bar_submenu_->SetDark(dark_);
            bar_submenu_popup_.Present();
          }
        }
      }
      layout_.SetDpi(Dpi());
      clock_.SetDpi(Dpi());
      Layout();
      Present();
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
        if (HitStart(pt) || HitClock(pt) || HitSegment(pt) != nullptr) {
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
      if (hwnd_ != nullptr) {
        KillTimer(hwnd_, kDesktopPeekDwellTimerId);
      }
      peek_dwell_armed_ = false;
      if (start_hot_ || start_pressed_) {
        start_hot_ = false;
        start_pressed_ = false;
        Present();
      }
      return 0;
    case WM_LBUTTONDOWN: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (HitStart(pt)) {
        status_popup_.Close();
        start_pressed_ = true;
        SetCapture(hwnd_);
        Present();
        return 0;
      }
      if (HitClock(pt)) {
        if (clock_open_ && status_popup_.IsOpen()) {
          status_popup_.Close();
          clock_open_ = false;
          skip_left_up_ = true;
          return 0;
        }
      }
      if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
        if (const BarSegment* seg = HitSegment(pt)) {
          if (seg->kind == SegmentKind::kStatus) {
            BeginReorder(seg->id, pt);
            return 0;
          }
        }
      }
      if (start_popup_open_) {
        start_popup_open_ = false;
        status_popup_.Close();
        Present();
      }
      break;
    }
    case WM_CAPTURECHANGED:
      if (start_pressed_) {
        start_pressed_ = false;
        Present();
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
      if (hit->id != kVolumeItemId) {
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
      if (start_pressed_) {
        start_pressed_ = false;
        ReleaseCapture();
        Present();
      }
      if (start_click) {
        ToggleStartMenu();
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
      if (HitClock(pt)) {
        ToggleClockFlyout();
        return 0;
      }
      if (const auto hit = HitTest(pt)) {
        if (hit->id == kSpotlightItemId) {
          status_popup_.Close();
          ToggleSpotlight();
          return 0;
        }
        if (hit->id == kControlCenterItemId) {
          ToggleControlCenter();
          return 0;
        }
        if (FindWidgetPage(hit->id) != nullptr) {
          ToggleWidgetPage(*hit);
          return 0;
        }
        status_popup_.Close();
        if (hit->id.rfind("bamti.tray/", 0) == 0) {
          RECT anchor{};
          if (SegmentScreenRect(hit->id, &anchor)) {
            TrayPopupGuardArm(anchor, tray_.OwnerPid(TrayMirror::ParseId(hit->id)));
          }
        }
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
      if (HitClock(pt)) {
        skip_left_up_ = true;
        return 0;
      }
      if (const auto hit = HitTest(pt)) {
        if (IsChromeItemId(hit->id)) {
          return 0;
        }
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
      if (HitStart(pt)) {
        ClientToScreen(hwnd_, &pt);
        ShowStartContextMenu(pt);
        return 0;
      }
      if (HitClock(pt)) {
        ShowClockMenu();
        return 0;
      }
      if (const auto hit = HitTest(pt)) {
        if (IsChromeItemId(hit->id)) {
          ClientToScreen(hwnd_, &pt);
          ShowContextMenu(pt);
          return 0;
        }
        if (hit->id.rfind("bamti.tray/", 0) == 0) {
          POINT screen = pt;
          ClientToScreen(hwnd_, &screen);
          if (tray_.ForwardsContextMenu()) {
            status_popup_.Close();
            RECT anchor{};
            if (SegmentScreenRect(hit->id, &anchor)) {
              TrayPopupGuardArm(anchor, tray_.OwnerPid(TrayMirror::ParseId(hit->id)));
            }
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
        } else if (HitClock(pt)) {
          tooltip_text_ = clock_.CurrentTimeText();
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
          cmd == kWidgetVolumeCmd || cmd == kWidgetBluetoothCmd || cmd == kWidgetBoardCmd ||
          cmd == kWidgetControlCenterCmd) {
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
        } else if (cmd == kWidgetBluetoothCmd) {
          next.bluetooth = !next.bluetooth;
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
      if (cmd == kSettingsCmd) {
        // TODO: 설정 페이지
        Log(L"bar", L"settings page not implemented yet");
      }
      if (cmd >= kStartExplorerCmd && cmd <= kStartShutdownCmd) {
        InvokeStartAction(static_cast<StartAction>(cmd - kStartExplorerCmd));
      }
      if (cmd >= kWinXCmdBase && cmd < kPowerSubCmd) {
        const size_t idx = static_cast<size_t>(cmd - kWinXCmdBase);
        if (idx < winx_entries_.size()) {
          LaunchWinXEntry(winx_entries_[idx]);
        }
      }
      if (cmd >= kPowerCmdBase && cmd < kPowerCmdBase + 5) {
        InvokePowerAction(static_cast<PowerAction>(cmd - kPowerCmdBase));
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
      TrayPopupGuardShutdown();
      RemoveWinHook();
      KillTimer(hwnd_, kClockTimerId);
      KillTimer(hwnd_, kRepaintTimerId);
      KillTimer(hwnd_, kToggleTimerId);
      KillTimer(hwnd_, kPeekTimerId);
      KillTimer(hwnd_, kDesktopPeekDwellTimerId);
      KillTimer(hwnd_, kTransitionsTimerId);
      KillTimer(hwnd_, kCornerWatchTimerId);
      KillTimer(hwnd_, kCtrlPollTimerId);
      desktop_toggle_.RestoreTransitions();
      peek_dwell_armed_ = false;
      last_corner_hit_ = false;
      corner_watch_on_ = false;
      StopDesktopPeek(L"destroy");
      StopFullscreenWatch(hwnd_);
      UnregisterSessionWatch();
      status_.StopAll();
      status_popup_.Destroy();
      bar_submenu_popup_.Destroy();
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

  const int backdrop = dwm::kBackdropNone;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));

  const int corner = dwm::kCornerDoNotRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));

  const MARGINS margins{0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);

  ApplyAccentPolicy(hwnd_, dark_);
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
  const BarLayoutResult& after = layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), OrderedItems());
  QueryPerformanceCounter(&t1);
  last_compute_ms_ = QpcMs(t0, t1);

  bool changed = before.segments.size() != after.segments.size() || before.dpi != after.dpi ||
                 EqualRect(&before.client, &after.client) == FALSE;
  if (!changed) {
    for (size_t i = 0; i < after.segments.size(); ++i) {
      const BarSegment& a = before.segments[i];
      const BarSegment& b = after.segments[i];
      if (a.kind != b.kind || a.id != b.id || EqualRect(&a.rect, &b.rect) == FALSE || a.text != b.text ||
          a.accent != b.accent || a.icon_key != b.icon_key) {
        changed = true;
        break;
      }
    }
  }
  if (changed) {
    Present();
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
  perf_create_.Add(draw.create_ms);
  perf_bind_.Add(draw.bind_ms);
  perf_brush_.Add(draw.brush_ms);
  perf_begin_.Add(draw.begin_ms);
  perf_draw_.Add(draw.draw_ms);
  perf_end_.Add(draw.end_ms);
  perf_bpbegin_.Add(bpbegin_ms);
  perf_bpend_.Add(bpend_ms);
  // draw_ms는 clock_.Draw() 하나의 시간이다. bpbegin/bpend는 그 바깥에서 재므로 빼지 않는다.
  const double other_ms = draw_ms - (draw.create_ms + draw.bind_ms + draw.brush_ms + draw.begin_ms +
                                     draw.draw_ms + draw.end_ms);
  perf_other_.Add(other_ms);
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
  auto mx = [](const PerfAcc& acc) { return acc.maxv; };
  wchar_t full_s[64]{};
  wchar_t seg_s[64]{};
  wchar_t compute_s[64]{};
  format_acc(full_s, 64, L"full", perf_full_);
  format_acc(seg_s, 64, L"seg", perf_seg_);
  format_acc(compute_s, 64, L"compute", perf_compute_);
  Log(L"perf", L"bar %s %s %s segments=%u overflow=%u%s", full_s, seg_s, compute_s,
      static_cast<unsigned>(last.segments.size()), static_cast<unsigned>(last.overflow.size()),
      perf_cold_ ? L" cold" : L"");
  Log(L"perf",
      L"draw create=%.2f/%.1f bind=%.2f/%.1f brush=%.2f/%.1f begin=%.2f/%.1f draw=%.2f/%.1f end=%.2f/%.1f "
      L"bpbegin=%.2f/%.1f bpend=%.2f/%.1f other=%.2f/%.1f (ms, avg/max)",
      avg(perf_create_), mx(perf_create_), avg(perf_bind_), mx(perf_bind_), avg(perf_brush_), mx(perf_brush_),
      avg(perf_begin_), mx(perf_begin_), avg(perf_draw_), mx(perf_draw_), avg(perf_end_), mx(perf_end_),
      avg(perf_bpbegin_), mx(perf_bpbegin_), avg(perf_bpend_), mx(perf_bpend_), avg(perf_other_), mx(perf_other_));
  perf_full_.Reset();
  perf_seg_.Reset();
  perf_compute_.Reset();
  perf_create_.Reset();
  perf_bind_.Reset();
  perf_brush_.Reset();
  perf_begin_.Reset();
  perf_draw_.Reset();
  perf_end_.Reset();
  perf_bpbegin_.Reset();
  perf_bpend_.Reset();
  perf_other_.Reset();
  perf_cold_ = false;
}

void MenuBar::Paint() {
  PAINTSTRUCT ps{};
  BeginPaint(hwnd_, &ps);
  EndPaint(hwnd_, &ps);
}

void MenuBar::ReleaseLayeredTarget() {
  if (mem_dc_ != nullptr && old_dib_ != nullptr) {
    SelectObject(mem_dc_, old_dib_);
    old_dib_ = nullptr;
  }
  if (dib_ != nullptr) {
    DeleteObject(dib_);
    dib_ = nullptr;
  }
  if (mem_dc_ != nullptr) {
    DeleteDC(mem_dc_);
    mem_dc_ = nullptr;
  }
  dib_w_ = 0;
  dib_h_ = 0;
}

void MenuBar::EnsureLayeredTarget() {
  if (hwnd_ == nullptr) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int width = (std::max)(0L, client.right - client.left);
  const int height = (std::max)(0L, client.bottom - client.top);
  if (width == 0 || height == 0) {
    return;
  }
  if (dib_ != nullptr && mem_dc_ != nullptr && dib_w_ == width && dib_h_ == height) {
    return;
  }
  ReleaseLayeredTarget();
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  mem_dc_ = CreateCompatibleDC(nullptr);
  if (mem_dc_ == nullptr) {
    return;
  }
  dib_ = CreateDIBSection(mem_dc_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib_ == nullptr) {
    DeleteDC(mem_dc_);
    mem_dc_ = nullptr;
    return;
  }
  old_dib_ = SelectObject(mem_dc_, dib_);
  dib_w_ = width;
  dib_h_ = height;
}

void MenuBar::Present() {
  WatchdogStage(L"bar.present");
  if (presenting_ || hwnd_ == nullptr || fullscreen_occluded_) {
    return;
  }
  presenting_ = true;
  struct PresentGuard {
    bool& busy;
    explicit PresentGuard(bool& flag) : busy(flag) {}
    ~PresentGuard() { busy = false; }
  } guard(presenting_);

  RECT client{};
  GetClientRect(hwnd_, &client);
  if (client.right <= client.left || client.bottom <= client.top) {
    return;
  }
  if (layout_.last().dpi != Dpi() || EqualRect(&layout_.last().client, &client) == FALSE) {
    layout_.SetDpi(Dpi());
    LARGE_INTEGER t0{};
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t0);
    layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), OrderedItems());
    QueryPerformanceCounter(&t1);
    last_compute_ms_ = QpcMs(t0, t1);
  }

  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  EnsureLayeredTarget();
  QueryPerformanceCounter(&t1);
  const double bpbegin_ms = QpcMs(t0, t1);
  if (mem_dc_ == nullptr || dib_w_ <= 0 || dib_h_ <= 0) {
    return;
  }

  DrawTimings draw{};
  QueryPerformanceCounter(&t0);
  clock_.Draw(mem_dc_, client, client, dark_, layout_.last(), &layout_, start_hot_ || start_popup_open_,
              start_pressed_ || start_popup_open_, &draw);
  QueryPerformanceCounter(&t1);
  const double draw_ms = QpcMs(t0, t1);

  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT src{0, 0};
  SIZE size{dib_w_, dib_h_};
  QueryPerformanceCounter(&t0);
  const BOOL ok = UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
  QueryPerformanceCounter(&t1);
  const double ulw_ms = QpcMs(t0, t1);
  NotePerf(last_compute_ms_, draw_ms, client, client, draw, bpbegin_ms, ulw_ms);
  static unsigned layered_first = 0;
  if (layered_first < 3) {
    ++layered_first;
    Log(L"perf", L"layered present n=%u ok=%d end=%.2f ulw=%.2f %dx%d", layered_first, ok ? 1 : 0, draw.end_ms, ulw_ms,
        dib_w_, dib_h_);
  }
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
    if (seg.kind == SegmentKind::kStatus && seg.id == kSpotlightItemId) {
      return seg.rect;
    }
  }
  return {};
}

RECT MenuBar::ControlCenterRect() const {
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kStatus && seg.id == kControlCenterItemId) {
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

bool MenuBar::HitClock(POINT client) const {
  const RECT rc = ClockRect();
  return rc.right > rc.left && PtInRect(&rc, client) != FALSE;
}

std::vector<StatusItem> MenuBar::OrderedItems() const {
  std::vector<StatusItem> items = status_.Snapshot();
  const std::vector<StatusItem> chrome = ChromeItems(widgets_.settings().control_center);
  items.insert(items.begin(), chrome.begin(), chrome.end());
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
  std::vector<StatusItem> unused_chrome;
  std::vector<StatusItem> unused_rest;
  for (size_t i = 0; i < items.size(); ++i) {
    if (used[i]) {
      continue;
    }
    if (IsChromeItemId(items[i].id)) {
      unused_chrome.push_back(std::move(items[i]));
    } else {
      unused_rest.push_back(std::move(items[i]));
    }
  }
  out.insert(out.begin(), unused_chrome.begin(), unused_chrome.end());
  out.insert(out.end(), unused_rest.begin(), unused_rest.end());
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
  StopDesktopPeek(L"reorder");
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kDesktopPeekDwellTimerId);
  }
  peek_dwell_armed_ = false;
  reorder_active_ = true;
  reorder_moved_ = false;
  reorder_id_ = id;
  reorder_start_ = pt;
  reorder_order_ = bar_order_;
  const std::vector<StatusItem> chrome = ChromeItems(widgets_.settings().control_center);
  std::vector<std::string> missing_chrome;
  for (const StatusItem& item : chrome) {
    if (!OrderContains(reorder_order_, item.id)) {
      missing_chrome.push_back(item.id);
    }
  }
  reorder_order_.insert(reorder_order_.begin(), missing_chrome.begin(), missing_chrome.end());
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

bool MenuBar::SegmentScreenRect(const std::string& id, RECT* out) const {
  if (hwnd_ == nullptr || out == nullptr) {
    return false;
  }
  for (const BarSegment& seg : layout_.last().segments) {
    if (seg.kind == SegmentKind::kStatus && seg.id == id) {
      *out = seg.rect;
      MapWindowPoints(hwnd_, nullptr, reinterpret_cast<POINT*>(out), 2);
      return true;
    }
  }
  return false;
}

void MenuBar::OpenOverflow() {
  if (overflow_panel_ == nullptr || hwnd_ == nullptr || layout_.last().overflow.empty()) {
    return;
  }
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  cc_open_ = false;
  clock_open_ = false;
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
  CloseBarSubmenu(L"other-popup");
  status_popup_.SetAfterTick(nullptr, nullptr);
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

void MenuBar::UpdateChrome(POINT client) {
  ArmMouseLeave();
  const bool start_hot = HitStart(client);
  if (start_hot_ != start_hot) {
    start_hot_ = start_hot;
    Present();
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
  if (start_popup_open_) {
    if (status_popup_.IsOpen()) {
      status_popup_.Close();
    }
    start_popup_open_ = false;
    Present();
    return;
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  if (!bar_menu_) {
    bar_menu_ = std::make_unique<BarMenuContent>();
  }
  CloseBarSubmenu(L"reopen");
  bar_menu_->Reset(hwnd_, dark_);
  bar_menu_->SetPopup(&status_popup_);
  bar_menu_->Add(kStartExplorerCmd, L"파일 탐색기");
  bar_menu_->Add(kStartSettingsCmd, L"설정");
  bar_menu_->Add(kStartRunCmd, L"실행");
  bar_menu_->AddSeparator();
  bar_menu_->Add(kStartSleepCmd, L"절전");
  bar_menu_->Add(kStartRestartCmd, L"다시 시작");
  bar_menu_->Add(kStartShutdownCmd, L"시스템 종료");

  RECT start = StartRect();
  POINT anchor{start.left, start.bottom};
  ClientToScreen(hwnd_, &anchor);

  cc_open_ = false;
  clock_open_ = false;
  open_panel_id_.clear();
  status_popup_.SetDark(dark_);
  status_popup_.SetAfterTick(&MenuBar::AfterBarPopupTick, this);
  if (!status_popup_.Open(bar_menu_.get(), anchor, PopupSurface::Anchor::BelowAt)) {
    Log(L"bar", L"start menu open failed err=%lu", GetLastError());
    Present();
    return;
  }
  start_popup_open_ = true;
  if (from_keyboard) {
    status_popup_.SetHot(0);
  }
  Present();
}

void MenuBar::ToggleSpotlight() {
  if (fullscreen_occluded_) {
    return;
  }
  status_popup_.Close();
  cc_open_ = false;
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  spotlight_.Toggle(hwnd_, dark_);
}

bool MenuBar::ShowControlCenter(const RECT& item_rect, ControlCenterPage page) {
  if (fullscreen_occluded_ || cc_panel_ == nullptr) {
    return false;
  }
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  ControlCenterHost host;
  host.dark = dark_;
  host.popup_hwnd = status_popup_.hwnd();
  host.dispatch = [this](const StatusEvent& ev) { status_.Dispatch(ev); };
  host.live = [this]() { return widgets_.LiveForControlCenter(); };
  host.bt_scan_result = [this]() { return widgets_.BtScanResult(); };
  host.bt_connect = [this](std::wstring address, bool connect) {
    widgets_.RequestBtConnect(std::move(address), connect);
  };
  host.present = [this]() {
    if (status_popup_.IsOpen()) {
      status_popup_.Present();
    }
  };
  host.set_allied = [this](HWND hwnd) { status_popup_.SetAlliedHwnd(hwnd); };
  cc_panel_->Reset(std::move(host), page);
  POINT anchor{item_rect.left, item_rect.bottom};
  ClientToScreen(hwnd_, &anchor);
  cc_open_ = true;
  clock_open_ = false;
  open_panel_id_.clear();
  CloseBarSubmenu(L"other-popup");
  status_popup_.SetAfterTick(nullptr, nullptr);
  status_popup_.SetDark(dark_);
  if (!status_popup_.Open(cc_panel_.get(), anchor, PopupSurface::Anchor::BelowAt)) {
    cc_open_ = false;
    Log(L"cc", L"open failed");
    return false;
  }
  return true;
}

void MenuBar::ToggleControlCenter() {
  if (fullscreen_occluded_ || !widgets_.settings().control_center) {
    return;
  }
  if (cc_open_ && status_popup_.IsOpen()) {
    status_popup_.Close();
    cc_open_ = false;
    Present();
    return;
  }
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  if (!ShowControlCenter(ControlCenterRect(), ControlCenterPage::kHome)) {
    return;
  }
  QueryPerformanceCounter(&t1);
  Log(L"cc", L"open to present %.2f ms", QpcMs(t0, t1));
  Present();
}

void MenuBar::ToggleClockFlyout() {
  if (fullscreen_occluded_) {
    return;
  }
  if (clock_open_ && status_popup_.IsOpen()) {
    status_popup_.Close();
    clock_open_ = false;
    return;
  }
  ShowClockFlyout();
}

bool MenuBar::ShowClockFlyout() {
  if (fullscreen_occluded_ || clock_panel_ == nullptr) {
    return false;
  }
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  clock_panel_->Reset(dark_);
  const RECT item = ClockRect();
  POINT anchor{item.right, item.bottom};
  ClientToScreen(hwnd_, &anchor);
  cc_open_ = false;
  clock_open_ = true;
  open_panel_id_.clear();
  CloseBarSubmenu(L"other-popup");
  status_popup_.SetAfterTick(nullptr, nullptr);
  status_popup_.SetDark(dark_);
  if (!status_popup_.Open(clock_panel_.get(), anchor, PopupSurface::Anchor::BelowAt)) {
    clock_open_ = false;
    Log(L"clock", L"flyout open failed");
    return false;
  }
  return true;
}

void MenuBar::ShowClockMenu() {
  if (fullscreen_occluded_ || clock_menu_ == nullptr) {
    return;
  }
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  clock_menu_->Reset(dark_);
  const RECT item = ClockRect();
  POINT anchor{item.left, item.bottom};
  ClientToScreen(hwnd_, &anchor);
  cc_open_ = false;
  clock_open_ = false;
  open_panel_id_.clear();
  CloseBarSubmenu(L"other-popup");
  status_popup_.SetAfterTick(nullptr, nullptr);
  status_popup_.SetDark(dark_);
  if (!status_popup_.Open(clock_menu_.get(), anchor, PopupSurface::Anchor::BelowAt)) {
    Log(L"clock", L"menu open failed");
  }
}

void MenuBar::ToggleWidgetPage(const StatusHit& hit) {
  const WidgetPage* page = FindWidgetPage(hit.id);
  if (page == nullptr) {
    return;
  }
  const WidgetSettings s = widgets_.settings();
  if (fullscreen_occluded_ || !(s.*(page->enabled))) {
    return;
  }
  if (cc_open_ && status_popup_.IsOpen() && cc_panel_ != nullptr && cc_panel_->CurrentPage() == page->page) {
    status_popup_.Close();
    cc_open_ = false;
    return;
  }
  OpenStatusPanel(hit);
}

void MenuBar::OpenStatusPanel(const StatusHit& hit) {
  if (const WidgetPage* page = FindWidgetPage(hit.id)) {
    if (ShowControlCenter(hit.rect, page->page)) {
      return;
    }
  }
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
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  cc_open_ = false;
  clock_open_ = false;
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
  CloseBarSubmenu(L"other-popup");
  status_popup_.SetAfterTick(nullptr, nullptr);
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
  if (clock_open_) {
    if (!status_popup_.IsOpen() || clock_panel_ == nullptr) {
      clock_open_ = false;
      return;
    }
    clock_panel_->Refresh();
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
  g_ctrl_held = false;
}

void MenuBar::AfterBarPopupTick(void* ctx) {
  if (ctx != nullptr) {
    static_cast<MenuBar*>(ctx)->SyncBarSubmenu();
  }
}

void MenuBar::SyncBarSubmenu() {
  if (!status_popup_.IsOpen() || bar_menu_ == nullptr) {
    CloseBarSubmenu(L"parent");
    return;
  }
  POINT cursor{};
  const bool got_cursor = GetCursorPos(&cursor) != FALSE;
  RECT sub{};
  const bool over_sub = got_cursor && bar_submenu_popup_.IsOpen() && bar_submenu_popup_.hwnd() != nullptr &&
                        GetWindowRect(bar_submenu_popup_.hwnd(), &sub) != FALSE && PtInRect(&sub, cursor);
  if (over_sub) {
    return;
  }
  const UINT cmd = bar_menu_->SubmenuIdAt(status_popup_.Hot());
  if (cmd == 0) {
    CloseBarSubmenu(L"hover-leave");
    return;
  }
  if (cmd == open_submenu_cmd_) {
    return;
  }
  CloseBarSubmenu(L"switch");
  OpenBarSubmenu(cmd);
}

void MenuBar::OpenBarSubmenu(UINT cmd) {
  if (!status_popup_.IsOpen() || bar_menu_ == nullptr || bar_submenu_popup_.IsOpen()) {
    return;
  }
  const int row_index = bar_menu_->RowIndexOfCommand(cmd);
  if (row_index < 0) {
    return;
  }
  if (!bar_submenu_) {
    bar_submenu_ = std::make_unique<BarMenuContent>();
  }
  bar_submenu_->Reset(hwnd_, dark_);
  bar_submenu_->SetPopup(&bar_submenu_popup_);
  if (cmd == kMenuWidgetsSubCmd) {
    const WidgetSettings s = widgets_.settings();
    bar_submenu_->Add(kWidgetBatteryCmd, L"배터리", s.battery);
    bar_submenu_->Add(kWidgetCpuCmd, L"CPU", s.cpu);
    bar_submenu_->Add(kWidgetNetworkCmd, L"네트워크", s.network);
    bar_submenu_->Add(kWidgetBluetoothCmd, L"블루투스", s.bluetooth);
    bar_submenu_->Add(kWidgetVolumeCmd, L"볼륨", s.volume);
    bar_submenu_->Add(kWidgetControlCenterCmd, L"제어 센터", s.control_center);
    if (IsWidgetBoardAvailable()) {
      bar_submenu_->Add(kWidgetBoardCmd, L"위젯 보드 단추", s.widget_board);
    }
  } else if (cmd == kMenuTraySubCmd) {
    bar_submenu_->SetMaxWidthDip(360);
    tray_menu_keys_.clear();
    const std::vector<TrayMirror::MenuItem> entries = tray_.MenuItems();
    if (entries.empty()) {
      bar_submenu_->Add(0, L"미러 중인 아이콘이 없습니다", false, false);
    } else {
      const size_t n = (std::min)(entries.size(), kTrayHiddenKeysMax);
      tray_menu_keys_.reserve(n);
      for (size_t i = 0; i < n; ++i) {
        bar_submenu_->Add(kTrayItemCmdBase + static_cast<UINT>(i), entries[i].label, entries[i].shown);
        tray_menu_keys_.push_back(entries[i].key);
      }
      if (entries.size() > kTrayHiddenKeysMax) {
        bar_submenu_->Add(0, L"이하 생략", false, false);
      }
    }
    bar_submenu_->AddSeparator();
    const WidgetSettings tray = tray_.settings();
    bar_submenu_->Add(kTrayMirrorToggleCmd, L"트레이 미러", tray.tray_mirror);
    bar_submenu_->Add(kTraySystemIconsCmd, L"시스템 아이콘도 표시", tray.tray_system_icons);
    bar_submenu_->Add(kTrayOverflowIconsCmd, L"숨긴 아이콘도 표시", tray.tray_overflow_icons);
    bar_submenu_->AddSeparator();
    bar_submenu_->Add(kTrayPeekCmd, L"알림 영역 잠시 표시");
  } else if (cmd == kPowerSubCmd) {
    bar_submenu_->Add(kPowerCmdBase + static_cast<UINT>(PowerAction::kLogoff), L"로그아웃");
    bar_submenu_->Add(kPowerCmdBase + static_cast<UINT>(PowerAction::kSleep), L"절전");
    if (HibernateAvailable()) {
      bar_submenu_->Add(kPowerCmdBase + static_cast<UINT>(PowerAction::kHibernate), L"최대 절전 모드");
    }
    bar_submenu_->Add(kPowerCmdBase + static_cast<UINT>(PowerAction::kShutdown), L"시스템 종료");
    bar_submenu_->Add(kPowerCmdBase + static_cast<UINT>(PowerAction::kRestart), L"다시 시작");
  } else {
    return;
  }
  RECT row{};
  if (!bar_menu_->RowScreenRect(row_index, &row)) {
    return;
  }
  const POINT anchor{row.right, row.top - DipToPx(kMenuPadDip, Dpi())};
  status_popup_.SetAllied(&bar_submenu_popup_);
  bar_submenu_popup_.SetDark(dark_);
  if (!bar_submenu_popup_.Open(bar_submenu_.get(), anchor, PopupSurface::Anchor::RightOf, false)) {
    status_popup_.SetAllied(nullptr);
    Log(L"bar", L"submenu open failed err=%lu cmd=%u", GetLastError(), cmd);
    return;
  }
  open_submenu_cmd_ = cmd;
}

void MenuBar::CloseBarSubmenu(const wchar_t* reason) {
  open_submenu_cmd_ = 0;
  if (!bar_submenu_popup_.IsOpen()) {
    status_popup_.SetAllied(nullptr);
    return;
  }
  Log(L"bar", L"submenu close reason=%s", reason != nullptr ? reason : L"explicit");
  status_popup_.SetAllied(nullptr);
  bar_submenu_popup_.Close();
}

void MenuBar::ShowStartContextMenu(POINT screen) {
  StopDesktopPeek(L"popup");
  if (fullscreen_occluded_) {
    return;
  }
  if (start_popup_open_) {
    start_popup_open_ = false;
    status_popup_.Close();
    Present();
  }
  winx_entries_ = LoadWinXEntries();
  if (winx_entries_.empty()) {
    Log(L"winx", L"empty, falling back to context menu");
    ShowContextMenu(screen);
    return;
  }
  if (!bar_menu_) {
    bar_menu_ = std::make_unique<BarMenuContent>();
  }
  CloseBarSubmenu(L"reopen");
  bar_menu_->Reset(hwnd_, dark_);
  bar_menu_->SetMaxWidthDip(360);  // Win+X 항목 이름이 길다. 트레이 서브메뉴와 같은 값이다.
  bar_menu_->SetPopup(&status_popup_);

  int prev_group = 0;
  bool power_added = false;
  for (size_t i = 0; i < winx_entries_.size(); ++i) {
    const WinXEntry& entry = winx_entries_[i];
    if (prev_group != 0 && entry.group != prev_group) {
      bar_menu_->AddSeparator();
    }
    if (entry.group == 1 && !power_added) {
      bar_menu_->Add(kPowerSubCmd, L"종료 또는 로그아웃", false, true, true);
      power_added = true;
    }
    bar_menu_->Add(kWinXCmdBase + static_cast<UINT>(i), entry.label);
    prev_group = entry.group;
  }
  if (!power_added) {
    if (prev_group != 0) {
      bar_menu_->AddSeparator();
    }
    bar_menu_->Add(kPowerSubCmd, L"종료 또는 로그아웃", false, true, true);
  }

  RECT start = StartRect();
  POINT anchor{start.left, start.bottom};
  ClientToScreen(hwnd_, &anchor);

  cc_open_ = false;
  clock_open_ = false;
  open_panel_id_.clear();
  status_popup_.SetDark(dark_);
  status_popup_.SetAfterTick(&MenuBar::AfterBarPopupTick, this);
  if (!status_popup_.Open(bar_menu_.get(), anchor, PopupSurface::Anchor::BelowAt)) {
    Log(L"bar", L"start context menu open failed err=%lu", GetLastError());
  }
}

void MenuBar::ShowContextMenu(POINT screen) {
  StopDesktopPeek(L"popup");
  if (fullscreen_occluded_) {
    return;
  }
  if (!bar_menu_) {
    bar_menu_ = std::make_unique<BarMenuContent>();
  }
  CloseBarSubmenu(L"reopen");
  bar_menu_->Reset(hwnd_, dark_);
  bar_menu_->SetPopup(&status_popup_);

  bar_menu_->Add(kMenuWidgetsSubCmd, L"표시 항목", false, true, true);
  bar_menu_->Add(kMenuTraySubCmd, L"트레이 아이콘", false, true, true);
  bar_menu_->AddSeparator();
  bar_menu_->Add(kSettingsCmd, L"bamti 설정");
  bar_menu_->Add(kAutostartCmd, L"로그인 시 bamti 시작", AutostartEnabled());
  bar_menu_->AddSeparator();
  bar_menu_->Add(kExitCommand, L"bamti 종료");

  cc_open_ = false;
  clock_open_ = false;
  open_panel_id_.clear();
  status_popup_.SetDark(dark_);
  status_popup_.SetAfterTick(&MenuBar::AfterBarPopupTick, this);
  if (!status_popup_.Open(bar_menu_.get(), screen, PopupSurface::Anchor::BelowAt)) {
    Log(L"bar", L"context menu open failed err=%lu", GetLastError());
  }
}

void MenuBar::ShowTrayIconMenu(POINT screen, const std::string& id) {
  StopDesktopPeek(L"popup");
  if (fullscreen_occluded_) {
    return;
  }
  if (!bar_menu_) {
    bar_menu_ = std::make_unique<BarMenuContent>();
  }
  CloseBarSubmenu(L"reopen");
  tray_menu_id_ = id;
  bar_menu_->Reset(hwnd_, dark_);
  bar_menu_->SetPopup(&status_popup_);
  bar_menu_->Add(kTrayPeekCmd, L"알림 영역 잠시 표시");
  bar_menu_->Add(kTrayHideIconCmd, L"이 아이콘 숨기기");
  bar_menu_->Add(kTrayMirrorOffCmd, L"트레이 미러 끄기");

  cc_open_ = false;
  clock_open_ = false;
  open_panel_id_.clear();
  status_popup_.SetDark(dark_);
  status_popup_.SetAfterTick(nullptr, nullptr);
  if (!status_popup_.Open(bar_menu_.get(), screen, PopupSurface::Anchor::BelowAt)) {
    Log(L"bar", L"tray icon menu open failed err=%lu", GetLastError());
  }
}

void MenuBar::ApplySettings(const WidgetSettings& next) {
  WidgetSettings merged = next;
  merged.bar_order = bar_order_;
  widgets_.SetSettings(merged);
  tray_.SetSettings(merged);
  if (cc_open_) {
    const ControlCenterPage cc_page =
        cc_panel_ != nullptr ? cc_panel_->CurrentPage() : ControlCenterPage::kHome;
    bool keep = merged.control_center;
    for (const WidgetPage& page : kWidgetPages) {
      if (cc_page == page.page && merged.*(page.enabled)) {
        keep = true;
        break;
      }
    }
    if (!keep) {
      status_popup_.Close();
      cc_open_ = false;
    }
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

bool MenuBar::CornerHit() const {
  if (hwnd_ == nullptr) {
    return false;
  }
  POINT pt{};
  RECT rc{};
  if (GetCursorPos(&pt) == FALSE || GetWindowRect(hwnd_, &rc) == FALSE) {
    return false;
  }
  if (PtInRect(&rc, pt) == FALSE) {
    return false;
  }
  return pt.x >= rc.right - DipToPx(kPeekZoneDip, Dpi());
}

bool MenuBar::DesktopPeekWanted() {
  if (fullscreen_occluded_ || reorder_active_) {
    return false;
  }
  if (status_popup_.IsOpen() || bar_submenu_popup_.IsOpen()) {
    return false;
  }
  return CtrlHeld();
}

bool MenuBar::CtrlHeld() {
  const bool async = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
  if (async) {
    ctrl_hook_only_since_ = 0;
    return true;
  }
  if (!g_ctrl_held) {
    ctrl_hook_only_since_ = 0;
    return false;
  }
  if (ctrl_hook_only_since_ == 0) {
    ctrl_hook_only_since_ = GetTickCount64();
    const int lbtn = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 ? 1 : 0;
    Log(L"peek", L"ctrl split async=0 hook=1 lbtn=%d", lbtn);
  }
  if (GetTickCount64() - ctrl_hook_only_since_ >= kCtrlHookGraceMs) {
    g_ctrl_held = false;
    ctrl_hook_only_since_ = 0;
    Log(L"peek", L"ctrl hook stale");
    return false;
  }
  return true;
}

void MenuBar::UpdateDesktopPeek() {
  if (!DesktopPeekWanted() || !CornerHit()) {
    if (hwnd_ != nullptr) {
      KillTimer(hwnd_, kDesktopPeekDwellTimerId);
    }
    peek_dwell_armed_ = false;
    StopDesktopPeek(L"ctrl-up");
    return;
  }
  if (peek_latched_ || peek_dwell_armed_) {
    return;
  }
  peek_dwell_armed_ = true;
  SetTimer(hwnd_, kDesktopPeekDwellTimerId, kDesktopPeekDwellMs, nullptr);
  Log(L"peek", L"dwell arm");
}

void MenuBar::StartDesktopPeek() {
  if (peek_latched_) {
    return;
  }
  desktop_toggle_.Toggle();
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kTransitionsTimerId, kTransitionsRestoreMs, nullptr);
  }
  peek_latched_ = true;
}

void MenuBar::StopDesktopPeek(const wchar_t* reason) {
  if (peek_latched_) {
    Log(L"peek", L"unlatch reason=%s", reason != nullptr ? reason : L"?");
  }
  peek_latched_ = false;
}

void MenuBar::StartCornerWatch() {
  if (corner_watch_on_ || hwnd_ == nullptr) {
    return;
  }
  corner_watch_on_ = true;
  SetTimer(hwnd_, kCornerWatchTimerId, kCornerWatchMs, nullptr);
  Log(L"peek", L"watch on=1");
}

void MenuBar::StopCornerWatch() {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kCornerWatchTimerId);
    KillTimer(hwnd_, kDesktopPeekDwellTimerId);
  }
  const bool was_on = corner_watch_on_;
  corner_watch_on_ = false;
  last_corner_hit_ = false;
  peek_dwell_armed_ = false;
  StopDesktopPeek(L"ctrl-up");
  if (was_on) {
    Log(L"peek", L"watch on=0");
  }
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
  if (occluded) {
    StopDesktopPeek(L"fullscreen");
    if (hwnd_ != nullptr) {
      KillTimer(hwnd_, kDesktopPeekDwellTimerId);
    }
    peek_dwell_armed_ = false;
  }
  fullscreen_occluded_ = occluded;
  UpdateProviderActive();
  if (occluded) {
    if (start_popup_open_) {
      start_popup_open_ = false;
      status_popup_.Close();
      Present();
    }
    spotlight_.Hide();
    status_popup_.Close();
    UnregisterAppBar();
    ShowWindow(hwnd_, SW_HIDE);
  } else {
    RegisterAppBar();
    ShowWindow(hwnd_, SW_SHOWNA);
    Layout();
    Present();
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
