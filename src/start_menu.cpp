#include "start_menu.hpp"

#include "corner.hpp"
#include "dwm.hpp"
#include "theme.hpp"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <powrprof.h>
#include <shellapi.h>
#include <windowsx.h>

namespace bamti {
namespace {

constexpr int kWidthDip = 280;
constexpr int kPadDip = 8;
constexpr int kRowHeightDip = 32;
constexpr int kSepHeightDip = 8;
constexpr int kGapBelowStartDip = 4;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

}  // namespace

StartMenu::~StartMenu() {
  Hide();
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
}

void StartMenu::Warmup(HWND owner, bool dark) {
  owner_ = owner;
  dark_ = dark;
  if (!EnsureWindow(owner)) {
    return;
  }
  ApplyChrome();
}

void StartMenu::Toggle(HWND owner, const RECT& start_screen, bool dark, bool from_keyboard) {
  if (visible_) {
    Hide();
    return;
  }
  if (!from_keyboard && closed_at_ != 0 && GetTickCount64() - closed_at_ < 250) {
    return;
  }
  owner_ = owner;
  dark_ = dark;
  anchor_ = start_screen;
  if (!EnsureWindow(owner)) {
    return;
  }
  hot_ = from_keyboard ? 0 : -1;
  ApplyChrome();
  LayoutWindow(anchor_);
  visible_ = true;
  ShowWindow(hwnd_, SW_SHOW);
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  HWND foreground = GetForegroundWindow();
  DWORD other_tid = 0;
  if (foreground != nullptr) {
    other_tid = GetWindowThreadProcessId(foreground, nullptr);
  }
  const DWORD self_tid = GetCurrentThreadId();
  if (other_tid != 0 && other_tid != self_tid) {
    AttachThreadInput(self_tid, other_tid, TRUE);
  }
  SetForegroundWindow(hwnd_);
  if (other_tid != 0 && other_tid != self_tid) {
    AttachThreadInput(self_tid, other_tid, FALSE);
  }
  SetFocus(hwnd_);
}

void StartMenu::Hide() {
  if (!visible_ && !closing_) {
    if (hwnd_ != nullptr) {
      ShowWindow(hwnd_, SW_HIDE);
    }
    return;
  }
  visible_ = false;
  closing_ = false;
  hot_ = -1;
  closed_at_ = GetTickCount64();
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
  if (owner_ != nullptr) {
    InvalidateRect(owner_, nullptr, FALSE);
  }
}

LRESULT CALLBACK StartMenu::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  StartMenu* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<StartMenu*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<StartMenu*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->HandleMessage(msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT StartMenu::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_MOUSEMOVE: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      UpdateHot(pt);
      return 0;
    }
    case WM_MOUSELEAVE:
      if (hot_ >= 0) {
        hot_ = -1;
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_LBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (const Row* row = HitTest(pt)) {
        ActivateRow(*row);
      }
      return 0;
    }
    case WM_SETCURSOR:
      if (LOWORD(lparam) == HTCLIENT) {
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        if (HitTest(pt) != nullptr) {
          SetCursor(LoadCursorW(nullptr, IDC_HAND));
          return TRUE;
        }
      }
      break;
    case WM_MOUSEACTIVATE:
      return MA_ACTIVATE;
    case WM_KEYDOWN:
      if (wparam == VK_ESCAPE) {
        Hide();
        return 0;
      }
      if (wparam == VK_RETURN && hot_ >= 0 && hot_ < static_cast<int>(rows_.size())) {
        ActivateRow(rows_[static_cast<size_t>(hot_)]);
        return 0;
      }
      if (wparam == VK_DOWN) {
        MoveHot(1);
        return 0;
      }
      if (wparam == VK_UP) {
        MoveHot(-1);
        return 0;
      }
      break;
    case WM_ACTIVATE:
      if (LOWORD(wparam) == WA_INACTIVE && visible_) {
        closing_ = true;
        Hide();
      }
      return 0;
    case WM_DESTROY:
      hwnd_ = nullptr;
      visible_ = false;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wparam, lparam);
}

bool StartMenu::EnsureWindow(HWND owner) {
  if (hwnd_ != nullptr) {
    return true;
  }
  HINSTANCE instance = nullptr;
  if (owner != nullptr) {
    instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
  }
  if (instance == nullptr) {
    instance = GetModuleHandleW(nullptr);
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH));
  wc.lpszClassName = kStartMenuClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kStartMenuClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
                          instance, this);
  return hwnd_ != nullptr;
}

bool StartMenu::EnsureRenderer() {
  if (!d2d_) {
    if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf()))) {
      return false;
    }
  }
  if (!dwrite_) {
    if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                   reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())))) {
      return false;
    }
  }
  if (!format_ || font_dpi_ != Dpi()) {
    font_dpi_ = Dpi();
    format_.Reset();
    wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
      wcscpy_s(locale, L"en-US");
    }
    const float size = 13.0f * static_cast<float>(font_dpi_) / 96.0f;
    const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
    HRESULT hr = E_FAIL;
    for (const wchar_t* family : families) {
      hr = dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                     DWRITE_FONT_STRETCH_NORMAL, size, locale, format_.ReleaseAndGetAddressOf());
      if (SUCCEEDED(hr)) {
        break;
      }
    }
    if (FAILED(hr) || !format_) {
      return false;
    }
    format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  }
  return true;
}

void StartMenu::LayoutWindow(const RECT& start_screen) {
  if (hwnd_ == nullptr) {
    return;
  }
  const UINT dpi = Dpi();
  const int width = DipToPx(kWidthDip, dpi);
  const int pad = DipToPx(kPadDip, dpi);
  const int row_h = DipToPx(kRowHeightDip, dpi);
  const int sep_h = DipToPx(kSepHeightDip, dpi);
  const int height = pad + row_h * 6 + sep_h + pad;

  HMONITOR monitor = MonitorFromWindow(owner_ != nullptr ? owner_ : hwnd_, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(monitor, &info);

  int x = start_screen.left;
  int y = start_screen.bottom + DipToPx(kGapBelowStartDip, dpi);
  if (x + width > info.rcWork.right) {
    x = info.rcWork.right - width;
  }
  if (x < info.rcWork.left) {
    x = info.rcWork.left;
  }
  if (y + height > info.rcWork.bottom) {
    y = start_screen.top - height - DipToPx(kGapBelowStartDip, dpi);
  }
  if (y < info.rcWork.top) {
    y = info.rcWork.top;
  }

  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  RebuildRows();
}

void StartMenu::RebuildRows() {
  rows_.clear();
  if (hwnd_ == nullptr) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int pad = Dip(kPadDip);
  const int row_h = Dip(kRowHeightDip);
  const int sep_h = Dip(kSepHeightDip);

  int y = pad;
  auto add_row = [&](Action action) {
    Row row{};
    row.rect = {pad, y, client.right - pad, y + row_h};
    row.action = action;
    rows_.push_back(row);
    y += row_h;
  };
  add_row(Action::Explorer);
  add_row(Action::Settings);
  add_row(Action::Run);
  y += sep_h;
  add_row(Action::Sleep);
  add_row(Action::Restart);
  add_row(Action::Shutdown);

  if (hot_ >= static_cast<int>(rows_.size())) {
    hot_ = rows_.empty() ? -1 : static_cast<int>(rows_.size()) - 1;
  }
}

void StartMenu::Paint() {
  PAINTSTRUCT ps{};
  const HDC hdc = BeginPaint(hwnd_, &ps);
  RECT client{};
  GetClientRect(hwnd_, &client);

  BP_PAINTPARAMS params{};
  params.cbSize = sizeof(params);
  params.dwFlags = BPPF_ERASE;
  HDC buffer_dc = nullptr;
  const HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &client, BPBF_TOPDOWNDIB, &params, &buffer_dc);
  if (buffer != nullptr && buffer_dc != nullptr && EnsureRenderer()) {
    BufferedPaintClear(buffer, &client);
    if (!rt_) {
      const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
          D2D1_RENDER_TARGET_TYPE_DEFAULT,
          D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
      d2d_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf());
    }
    if (rt_ && SUCCEEDED(rt_->BindDC(buffer_dc, &client))) {
      rt_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
      rt_->BeginDraw();
      Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
      rt_->CreateSolidColorBrush(D2D1::ColorF(0.94f, 0.94f, 0.94f, 1.0f), brush.GetAddressOf());
      if (brush) {
        rt_->FillRectangle(
            D2D1::RectF(0.0f, 0.0f, static_cast<float>(client.right), static_cast<float>(client.bottom)), brush.Get());
      }

      const int pad = Dip(kPadDip);
      const int row_h = Dip(kRowHeightDip);
      if (brush && rows_.size() >= 4) {
        brush->SetColor(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.12f));
        const float x1 = static_cast<float>(pad);
        const float x2 = static_cast<float>(client.right - pad);
        const float y = static_cast<float>(pad + row_h * 3 + Dip(kSepHeightDip) / 2);
        rt_->DrawLine(D2D1::Point2F(x1, y), D2D1::Point2F(x2, y), brush.Get(), 1.0f);
      }

      for (size_t i = 0; i < rows_.size(); ++i) {
        const Row& row = rows_[i];
        if (static_cast<int>(i) == hot_ && brush) {
          brush->SetColor(MenuItemHoverFill(false, false));
          const float hover_r =
              corner::HoverPx(static_cast<float>(row.rect.bottom - row.rect.top), Dpi());
          const D2D1_ROUNDED_RECT hover{
              D2D1::RectF(static_cast<float>(row.rect.left), static_cast<float>(row.rect.top),
                          static_cast<float>(row.rect.right), static_cast<float>(row.rect.bottom)),
              hover_r, hover_r};
          rt_->FillRoundedRectangle(hover, brush.Get());
        }

        std::wstring label;
        switch (row.action) {
          case Action::Explorer:
            label = L"파일 탐색기";
            break;
          case Action::Settings:
            label = L"설정";
            break;
          case Action::Run:
            label = L"실행";
            break;
          case Action::Sleep:
            label = L"절전";
            break;
          case Action::Restart:
            label = L"다시 시작";
            break;
          case Action::Shutdown:
            label = L"시스템 종료";
            break;
        }
        if (!label.empty() && format_ && brush) {
          Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
          const float w = static_cast<float>(row.rect.right - row.rect.left - Dip(16));
          const float h = static_cast<float>(row.rect.bottom - row.rect.top);
          if (SUCCEEDED(dwrite_->CreateTextLayout(label.c_str(), static_cast<UINT32>(label.size()), format_.Get(), w, h,
                                                  layout.GetAddressOf()))) {
            brush->SetColor(D2D1::ColorF(0.09f, 0.09f, 0.09f, 1.0f));
            rt_->DrawTextLayout(D2D1::Point2F(static_cast<float>(row.rect.left + Dip(8)), static_cast<float>(row.rect.top)),
                                layout.Get(), brush.Get(), D2D1_DRAW_TEXT_OPTIONS_NONE);
          }
        }
      }

      const HRESULT hr = rt_->EndDraw();
      if (hr == D2DERR_RECREATE_TARGET) {
        rt_.Reset();
      }
    }
    BufferedPaintSetAlpha(buffer, &client, 255);
    EndBufferedPaint(buffer, TRUE);
  }
  EndPaint(hwnd_, &ps);
}

void StartMenu::ApplyChrome() {
  if (hwnd_ == nullptr) {
    return;
  }
  const BOOL dark = FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));
  const int backdrop = dwm::kBackdropNone;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));
  const int corner = dwm::kCornerRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));
  const MARGINS margins{0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);
}

void StartMenu::UpdateHot(POINT client) {
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  TrackMouseEvent(&track);

  int next = -1;
  for (size_t i = 0; i < rows_.size(); ++i) {
    if (PtInRect(&rows_[i].rect, client)) {
      next = static_cast<int>(i);
      break;
    }
  }
  if (next == hot_) {
    return;
  }
  hot_ = next;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void StartMenu::MoveHot(int delta) {
  if (rows_.empty()) {
    return;
  }
  int next = hot_ + delta;
  if (hot_ < 0) {
    next = delta > 0 ? 0 : static_cast<int>(rows_.size()) - 1;
  }
  if (next < 0) {
    next = static_cast<int>(rows_.size()) - 1;
  }
  if (next >= static_cast<int>(rows_.size())) {
    next = 0;
  }
  if (next == hot_) {
    return;
  }
  hot_ = next;
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void StartMenu::ActivateRow(const Row& row) {
  switch (row.action) {
    case Action::Explorer:
      LaunchPath(L"explorer.exe");
      break;
    case Action::Settings:
      LaunchPath(L"ms-settings:");
      break;
    case Action::Run:
      Hide();
      SendWinChord(L'R');
      return;
    case Action::Sleep:
      Hide();
      SetSuspendState(FALSE, TRUE, FALSE);
      return;
    case Action::Restart:
      Hide();
      if (EnableShutdownPrivilege()) {
        ExitWindowsEx(EWX_REBOOT, 0);
      }
      return;
    case Action::Shutdown:
      Hide();
      if (EnableShutdownPrivilege()) {
        ExitWindowsEx(EWX_SHUTDOWN, 0);
      }
      return;
  }
  Hide();
}

void StartMenu::LaunchPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}

void StartMenu::SendWinChord(WORD vk) {
  INPUT keys[4]{};
  keys[0].type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_LWIN;
  keys[1].type = INPUT_KEYBOARD;
  keys[1].ki.wVk = vk;
  keys[2].type = INPUT_KEYBOARD;
  keys[2].ki.wVk = vk;
  keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
  keys[3].type = INPUT_KEYBOARD;
  keys[3].ki.wVk = VK_LWIN;
  keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, keys, sizeof(INPUT));
}

bool StartMenu::EnableShutdownPrivilege() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) == FALSE) {
    CloseHandle(token);
    return false;
  }
  AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  const bool ok = GetLastError() == ERROR_SUCCESS;
  CloseHandle(token);
  return ok;
}

const StartMenu::Row* StartMenu::HitTest(POINT client) const {
  for (const auto& row : rows_) {
    if (PtInRect(&row.rect, client)) {
      return &row;
    }
  }
  return nullptr;
}

UINT StartMenu::Dpi() const {
  HWND source = hwnd_ != nullptr ? hwnd_ : owner_;
  if (source == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(source);
  return dpi == 0 ? 96 : dpi;
}

int StartMenu::Dip(int value) const {
  return DipToPx(value, Dpi());
}

}  // namespace bamti
