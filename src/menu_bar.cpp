#include "menu_bar.hpp"

#include "dwm.hpp"
#include "fullscreen.hpp"
#include "log.hpp"
#include "theme.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <uxtheme.h>
#include <windowsx.h>

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
constexpr int kBarHeightDip = 32;
constexpr UINT kExitCommand = 1;

constexpr int kPanelPadDip = 12;
constexpr int kPanelMinWidthDip = 280;
constexpr int kPanelMaxWidthDip = 360;
constexpr int kPanelTitleDip = 22;
constexpr int kPanelSubDip = 18;
constexpr int kPanelGaugeLabelDip = 18;
constexpr int kPanelGaugeBarDip = 6;
constexpr int kPanelGaugeNoteDip = 16;
constexpr int kPanelGaugeGapDip = 10;
constexpr int kPanelSepDip = 9;
constexpr int kPanelActionDip = 28;

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

std::string WideToUtf8(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                   nullptr);
  if (n <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

}  // namespace

class StatusPanelContent : public PopupContent {
 public:
  void Reset(MenuBar* owner, StatusItem item) {
    owner_ = owner;
    item_ = std::move(item);
    action_hits_.clear();
  }

  int RowCount() const override { return static_cast<int>(action_hits_.size()); }

  SIZE Measure(UINT dpi) override {
    action_hits_.clear();
    const StatusPanel* panel = item_.panel ? &*item_.panel : nullptr;
    if (panel == nullptr) {
      return SIZE{};
    }

    const int pad = DipToPx(kPanelPadDip, dpi);
    int inner = 0;
    auto consider = [&](const std::wstring& text) {
      if (!text.empty()) {
        inner = (std::max)(inner, static_cast<int>(PopupTextWidth(dpi, text) + 0.5f));
      }
    };
    consider(panel->title);
    consider(panel->subtitle);
    consider(panel->updated_text);
    for (const StatusGauge& gauge : panel->gauges) {
      const int label = static_cast<int>(PopupTextWidth(dpi, gauge.label) + 0.5f);
      const int detail = static_cast<int>(PopupTextWidth(dpi, gauge.detail) + 0.5f);
      inner = (std::max)(inner, label + DipToPx(12, dpi) + detail);
      consider(gauge.note);
    }
    for (const std::wstring& action : panel->actions) {
      consider(action);
    }

    int width = inner + pad * 2;
    width = (std::max)(width, DipToPx(kPanelMinWidthDip, dpi));
    width = (std::min)(width, DipToPx(kPanelMaxWidthDip, dpi));

    int y = pad;
    if (!panel->title.empty()) {
      y += DipToPx(kPanelTitleDip, dpi);
    }
    if (!panel->subtitle.empty()) {
      y += DipToPx(kPanelSubDip, dpi);
    }
    if (!panel->updated_text.empty()) {
      y += DipToPx(kPanelSubDip, dpi);
    }
    for (const StatusGauge& gauge : panel->gauges) {
      y += DipToPx(kPanelGaugeLabelDip, dpi);
      y += DipToPx(kPanelGaugeBarDip, dpi);
      if (!gauge.note.empty()) {
        y += DipToPx(kPanelGaugeNoteDip, dpi);
      }
      y += DipToPx(kPanelGaugeGapDip, dpi);
    }
    if (!panel->actions.empty()) {
      if (!panel->gauges.empty() || !panel->title.empty() || !panel->subtitle.empty() ||
          !panel->updated_text.empty()) {
        y += DipToPx(kPanelSepDip, dpi);
      }
      const int row = DipToPx(kPanelActionDip, dpi);
      for (int i = 0; i < static_cast<int>(panel->actions.size()); ++i) {
        RECT rc{pad, y, width - pad, y + row};
        action_hits_.push_back(rc);
        y += row;
      }
    }
    y += pad;
    return SIZE{width, y};
  }

  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override {
    if (target == nullptr || owner_ == nullptr || !item_.panel) {
      return;
    }
    const StatusPanel& panel = *item_.panel;
    const bool dark = owner_->dark_;
    const D2D1_SIZE_F sz = target->GetSize();
    const int width = static_cast<int>(sz.width);
    const int pad = DipToPx(kPanelPadDip, dpi);

    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
    const D2D1_COLOR_F text_c = ClockTextColor(dark);
    D2D1_COLOR_F muted_c = text_c;
    muted_c.a *= 0.65f;
    const D2D1_COLOR_F fill_c =
        item_.accent != 0 ? D2D1::ColorF(item_.accent) : DockIndicatorColor(dark);
    if (FAILED(target->CreateSolidColorBrush(text_c, text.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(muted_c, muted.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(MenuItemHoverFill(dark, false), hover.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(DockStrokeColor(dark), line.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(D2D1::ColorF(text_c.r, text_c.g, text_c.b, 0.18f),
                                            track.GetAddressOf())) ||
        FAILED(target->CreateSolidColorBrush(fill_c, fill.GetAddressOf()))) {
      return;
    }

    int y = pad;
    auto draw_line = [&](const std::wstring& s, int height, ID2D1Brush* brush) {
      if (s.empty()) {
        return;
      }
      DrawPopupText(target, dpi, s,
                    D2D1::RectF(static_cast<float>(pad), static_cast<float>(y),
                                static_cast<float>(width - pad), static_cast<float>(y + height)),
                    brush);
      y += height;
    };
    draw_line(panel.title, DipToPx(kPanelTitleDip, dpi), text.Get());
    draw_line(panel.subtitle, DipToPx(kPanelSubDip, dpi), muted.Get());
    draw_line(panel.updated_text, DipToPx(kPanelSubDip, dpi), muted.Get());

    const int label_h = DipToPx(kPanelGaugeLabelDip, dpi);
    const int bar_h = DipToPx(kPanelGaugeBarDip, dpi);
    const int note_h = DipToPx(kPanelGaugeNoteDip, dpi);
    const int gap = DipToPx(kPanelGaugeGapDip, dpi);
    for (const StatusGauge& gauge : panel.gauges) {
      const float left = static_cast<float>(pad);
      const float right = static_cast<float>(width - pad);
      DrawPopupText(target, dpi, gauge.label,
                    D2D1::RectF(left, static_cast<float>(y), right * 0.55f, static_cast<float>(y + label_h)),
                    text.Get());
      DrawPopupText(target, dpi, gauge.detail,
                    D2D1::RectF(right * 0.55f, static_cast<float>(y), right, static_cast<float>(y + label_h)),
                    muted.Get());
      y += label_h;
      const float bar_top = static_cast<float>(y);
      const float bar_bottom = bar_top + static_cast<float>(bar_h);
      target->FillRectangle(D2D1::RectF(left, bar_top, right, bar_bottom), track.Get());
      const float filled = left + (right - left) * gauge.value;
      if (filled > left) {
        target->FillRectangle(D2D1::RectF(left, bar_top, filled, bar_bottom), fill.Get());
      }
      y += bar_h;
      if (!gauge.note.empty()) {
        DrawPopupText(target, dpi, gauge.note,
                      D2D1::RectF(left, static_cast<float>(y), right, static_cast<float>(y + note_h)),
                      fill.Get());
        y += note_h;
      }
      y += gap;
    }

    if (!panel.actions.empty()) {
      if (!panel.gauges.empty() || !panel.title.empty() || !panel.subtitle.empty() ||
          !panel.updated_text.empty()) {
        const float mid = static_cast<float>(y + DipToPx(kPanelSepDip, dpi) / 2) + 0.5f;
        target->DrawLine(D2D1::Point2F(static_cast<float>(pad), mid),
                         D2D1::Point2F(static_cast<float>(width - pad), mid), line.Get(), 1.0f);
        y += DipToPx(kPanelSepDip, dpi);
      }
      const int row = DipToPx(kPanelActionDip, dpi);
      for (int i = 0; i < static_cast<int>(panel.actions.size()); ++i) {
        const float top = static_cast<float>(y);
        const float bottom = top + static_cast<float>(row);
        if (i == hot_index) {
          target->FillRectangle(
              D2D1::RectF(static_cast<float>(pad), top, static_cast<float>(width - pad), bottom), hover.Get());
        }
        DrawPopupText(target, dpi, panel.actions[static_cast<size_t>(i)],
                      D2D1::RectF(static_cast<float>(pad), top, static_cast<float>(width - pad), bottom),
                      text.Get());
        y += row;
      }
    }
  }

  int HitTest(POINT client, UINT dpi) const override {
    (void)dpi;
    for (int i = 0; i < static_cast<int>(action_hits_.size()); ++i) {
      if (PtInRect(&action_hits_[static_cast<size_t>(i)], client)) {
        return i;
      }
    }
    return -1;
  }

  void Invoke(int index) override {
    if (owner_ == nullptr || !item_.panel) {
      return;
    }
    const auto& actions = item_.panel->actions;
    if (index < 0 || index >= static_cast<int>(actions.size())) {
      return;
    }
    owner_->status_.SendClick(item_.id, WideToUtf8(actions[static_cast<size_t>(index)]));
  }

 private:
  MenuBar* owner_ = nullptr;
  StatusItem item_{};
  std::vector<RECT> action_hits_;
};

MenuBar::MenuBar() : status_panel_(std::make_unique<StatusPanelContent>()) {}

MenuBar::~MenuBar() {
  RemoveWinHook();
  status_.Stop();
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
      if (wparam == kClockTimerId) {
        if (status_popup_.IsOpen()) {
          status_popup_.Tick();
        }
        status_.DropStale();
        taskbar_.EnsureHidden();
        const std::wstring clock = clock_.CurrentTimeText();
        if (clock != last_clock_text_) {
          last_clock_text_ = clock;
          if (clock_rect_.right > clock_rect_.left) {
            InvalidateRect(hwnd_, &clock_rect_, FALSE);
          } else {
            InvalidateRect(hwnd_, nullptr, FALSE);
          }
        }
      }
      return 0;
    case kFullscreenWatchMsg:
      RefreshFullscreenState();
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
          status_popup_.SetDark(dark_);
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
        status_popup_.Close();
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
        const StatusHit copy = *hit;
        status_.SendClick(copy.id, "left");
        OpenStatusPanel(copy);
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
        StopFullscreenWatch(hwnd_);
        status_.Stop();
        taskbar_.Restore();
        UnregisterAppBar();
      }
      return 0;
    case WM_DESTROY:
      RemoveWinHook();
      KillTimer(hwnd_, kClockTimerId);
      StopFullscreenWatch(hwnd_);
      status_.Stop();
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
                    start_hot_ || start_menu_.visible(), start_pressed_ || start_menu_.visible(), &clock_rect_);
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
  status_popup_.Close();
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
  status_popup_.Close();
  if (start_menu_.visible()) {
    start_menu_.Hide();
    InvalidateRect(hwnd_, &start_rect_, FALSE);
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
    InvalidateRect(hwnd_, &start_rect_, FALSE);
  }
  if (spotlight_.visible()) {
    spotlight_.Hide();
  }
  status_panel_->Reset(this, *found);
  POINT anchor{hit.rect.left, hit.rect.bottom};
  ClientToScreen(hwnd_, &anchor);
  status_popup_.Open(status_panel_.get(), anchor, PopupSurface::Anchor::BelowAt);
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
