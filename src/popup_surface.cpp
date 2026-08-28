#include "popup_surface.hpp"

#include "dwm.hpp"
#include "log.hpp"
#include "theme.hpp"
#include "watchdog.hpp"

#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace bamti {
namespace {

constexpr UINT_PTR kPopupGuardTimer = 1;
constexpr UINT kPopupGuardMs = 50;

const wchar_t* MouseMsgName(UINT msg) {
  switch (msg) {
    case WM_LBUTTONDOWN:
      return L"lbuttondown";
    case WM_RBUTTONDOWN:
      return L"rbuttondown";
    case WM_LBUTTONUP:
      return L"lbuttonup";
    case WM_RBUTTONUP:
      return L"rbuttonup";
    case WM_MOUSEMOVE:
      return L"mousemove";
    default:
      return L"mouse";
  }
}

const wchar_t* ReasonName(PopupSurface::DismissReason reason) {
  switch (reason) {
    case PopupSurface::DismissReason::kInvoke:
      return L"invoke";
    case PopupSurface::DismissReason::kOutsideClick:
      return L"outside-click";
    case PopupSurface::DismissReason::kOutsidePoll:
      return L"outside-poll";
    case PopupSurface::DismissReason::kCaptureLost:
      return L"capture-lost";
    case PopupSurface::DismissReason::kCaptureGone:
      return L"capture-gone";
    case PopupSurface::DismissReason::kEscape:
      return L"escape";
    case PopupSurface::DismissReason::kWinKey:
      return L"win";
    case PopupSurface::DismissReason::kForeground:
      return L"foreground";
    case PopupSurface::DismissReason::kReopen:
      return L"reopen";
    case PopupSurface::DismissReason::kExplicit:
      return L"explicit";
    default:
      return L"unknown";
  }
}

IDWriteFactory* WriteFactory() {
  static Microsoft::WRL::ComPtr<IDWriteFactory> factory;
  if (!factory) {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(factory.GetAddressOf()));
  }
  return factory.Get();
}

IDWriteTextFormat* TextFormat(UINT dpi) {
  static Microsoft::WRL::ComPtr<IDWriteTextFormat> format;
  static UINT format_dpi = 0;
  if (format && format_dpi == dpi) {
    return format.Get();
  }
  format.Reset();
  format_dpi = dpi;
  IDWriteFactory* factory = WriteFactory();
  if (factory == nullptr) {
    return nullptr;
  }
  wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
  if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
    lstrcpynW(locale, L"en-US", LOCALE_NAME_MAX_LENGTH);
  }
  const float px = 13.0f * static_cast<float>(dpi) / 96.0f;
  const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
  for (const wchar_t* family : families) {
    if (SUCCEEDED(factory->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, px, locale, format.ReleaseAndGetAddressOf()))) {
      break;
    }
  }
  if (!format) {
    return nullptr;
  }
  format->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  format->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  return format.Get();
}

bool SameProcess(HWND hwnd) {
  if (hwnd == nullptr) {
    return false;
  }
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

}  // namespace

float PopupTextWidth(UINT dpi, const std::wstring& text) {
  if (text.empty()) {
    return 0.0f;
  }
  IDWriteFactory* factory = WriteFactory();
  IDWriteTextFormat* format = TextFormat(dpi);
  if (factory == nullptr || format == nullptr) {
    return 0.0f;
  }
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, 4096.0f, 256.0f,
                                       layout.GetAddressOf()))) {
    return 0.0f;
  }
  DWRITE_TEXT_METRICS metrics{};
  if (FAILED(layout->GetMetrics(&metrics))) {
    return 0.0f;
  }
  return metrics.widthIncludingTrailingWhitespace;
}

void DrawPopupText(ID2D1RenderTarget* target, UINT dpi, const std::wstring& text, const D2D1_RECT_F& rect,
                   ID2D1Brush* brush) {
  if (target == nullptr || brush == nullptr || text.empty()) {
    return;
  }
  IDWriteFactory* factory = WriteFactory();
  IDWriteTextFormat* format = TextFormat(dpi);
  if (factory == nullptr || format == nullptr) {
    return;
  }
  const float width = (std::max)(0.0f, rect.right - rect.left);
  const float height = (std::max)(0.0f, rect.bottom - rect.top);
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(factory->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, width, height,
                                       layout.GetAddressOf()))) {
    return;
  }
  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
  if (SUCCEEDED(factory->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf()))) {
    layout->SetTrimming(&trim, ellipsis.Get());
  }
  target->DrawTextLayout(D2D1::Point2F(rect.left, rect.top), layout.Get(), brush,
                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

PopupSurface::~PopupSurface() {
  Destroy();
}

bool PopupSurface::Create(HINSTANCE instance, HWND owner) {
  Destroy();
  owner_ = owner;
  if (instance == nullptr) {
    return false;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DROPSHADOW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kPopupClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf())) || !d2d_) {
    return false;
  }

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE, kPopupClass, L"", WS_POPUP, 0, 0, 0, 0,
                          owner, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    return false;
  }
  ApplyChrome();
  // Build the render target now, while the window is still hidden. Creating it
  // lazily inside the first WM_PAINT stalls the first menu by the full device
  // setup cost. A later Open() only resizes what this call already made.
  const ULONGLONG started = GetTickCount64();
  SetWindowPos(hwnd_, nullptr, 0, 0, 8, 8, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  EnsureRenderTarget();
  Log(L"popup", L"render target warm=%d %ums", target_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
  return true;
}

void PopupSurface::Destroy() {
  Dismiss(-1, DismissReason::kExplicit);
  HWND w = hwnd_;
  hwnd_ = nullptr;
  owner_ = nullptr;
  target_.Reset();
  fill_.Reset();
  d2d_.Reset();
  if (w != nullptr && IsWindow(w)) {
    DestroyWindow(w);
  }
}

bool PopupSurface::Open(PopupContent* content, POINT anchor_screen, Anchor mode) {
  WatchdogStage(L"popup.open");
  const ULONGLONG started = GetTickCount64();
  if (hwnd_ == nullptr || content == nullptr) {
    return false;
  }
  Dismiss(-1, DismissReason::kReopen);
  content_ = content;
  mode_ = mode;
  anchor_ = anchor_screen;
  const UINT dpi = Dpi();
  const SIZE size = content_->Measure(dpi);
  if (size.cx <= 0 || size.cy <= 0) {
    content_ = nullptr;
    return false;
  }

  Place(size, anchor_screen, mode);
  hot_ = -1;
  open_ = true;
  last_fg_ = GetForegroundWindow();
  esc_down_ = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  win_down_ = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
  // Seed from the live button state so the guard timer does not read the press
  // that opened this popup as an outside click.
  mouse_down_ = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
  press_inside_ = false;
  saw_mousemove_ = false;
  armed_ = false;
  tick_ = 0;
  ApplyChrome();
  // Hidden windows do not receive WM_PAINT from UpdateWindow, so show first and
  // paint immediately afterwards. A one-frame flash beats a second of black.
  SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
  SetCapture(hwnd_);
  ArmGuardTimer();
  Log(L"popup", L"open rows=%d shown %ums", content_->RowCount(),
      static_cast<unsigned>(GetTickCount64() - started));
  return true;
}

void PopupSurface::Close() {
  Dismiss(-1, DismissReason::kExplicit);
}

void PopupSurface::SetDark(bool dark) {
  if (dark_ == dark) {
    return;
  }
  dark_ = dark;
  fill_.Reset();
  if (hwnd_ != nullptr) {
    ApplyChrome();
    if (open_) {
      InvalidateRect(hwnd_, nullptr, FALSE);
    }
  }
}

void PopupSurface::Dismiss(int invoke_index, DismissReason reason) {
  WatchdogStage(L"popup.dismiss");
  if (!open_) {
    return;
  }
  Log(L"popup", L"dismiss reason=%s index=%d", ReasonName(reason), invoke_index);
  open_ = false;
  hot_ = -1;
  press_inside_ = false;
  armed_ = false;
  if (hwnd_ != nullptr && GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kPopupGuardTimer);
    ShowWindow(hwnd_, SW_HIDE);
  }

  PopupContent* content = content_;
  content_ = nullptr;
  if (content != nullptr && invoke_index >= 0) {
    content->Invoke(invoke_index);
  }
  if (owner_ != nullptr) {
    PostMessageW(owner_, kPopupClosedMsg, 0, 0);
  }
}

void PopupSurface::Place(SIZE size, POINT anchor_screen, Anchor mode) {
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(MonitorFromPoint(anchor_screen, MONITOR_DEFAULTTONEAREST), &info);
  const UINT dpi = Dpi();
  const int gap = MulDiv(4, static_cast<int>(dpi), 96);
  const int inset = MulDiv(8, static_cast<int>(dpi), 96);
  int x = anchor_screen.x - inset;
  int y = mode == Anchor::AboveAt ? anchor_screen.y - size.cy - gap : anchor_screen.y + gap;
  if (x + size.cx > info.rcWork.right) {
    x = info.rcWork.right - size.cx;
  }
  if (x < info.rcWork.left) {
    x = info.rcWork.left;
  }
  if (y + size.cy > info.rcWork.bottom) {
    y = info.rcWork.bottom - size.cy;
  }
  if (y < info.rcWork.top) {
    y = info.rcWork.top;
  }
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, size.cx, size.cy, SWP_NOACTIVATE);
}

void PopupSurface::ApplyChrome() {
  if (hwnd_ == nullptr || !IsWindow(hwnd_)) {
    return;
  }
  const BOOL dark = dark_ ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));
  const int backdrop = dwm::kBackdropNone;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));
  const int corner = dwm::kCornerRoundSmall;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));
}

void PopupSurface::ArmGuardTimer() {
  if (hwnd_ == nullptr) {
    return;
  }
  const UINT_PTR id = SetTimer(hwnd_, kPopupGuardTimer, kPopupGuardMs, nullptr);
  Log(L"popup", L"arm guard id=%llu err=%lu", static_cast<unsigned long long>(id), id == 0 ? GetLastError() : 0);
}

void PopupSurface::Tick() {
  Tick(L"owner");
}

void PopupSurface::Tick(const wchar_t* src) {
  WatchdogStage(L"popup.tick");
  if (!open_ || hwnd_ == nullptr || ticking_) {
    return;
  }
  struct TickGuard {
    bool& busy;
    explicit TickGuard(bool& flag) : busy(flag) { busy = true; }
    ~TickGuard() { busy = false; }
  } guard(ticking_);
  static_cast<void>(guard);

  const bool esc = (GetAsyncKeyState(VK_ESCAPE) & 0x8000) != 0;
  if (esc && !esc_down_) {
    Dismiss(-1, DismissReason::kEscape);
    return;
  }
  esc_down_ = esc;

  const bool win = (GetAsyncKeyState(VK_LWIN) & 0x8000) != 0 || (GetAsyncKeyState(VK_RWIN) & 0x8000) != 0;
  if (win && !win_down_) {
    Dismiss(-1, DismissReason::kWinKey);
    return;
  }
  win_down_ = win;

  // WS_EX_NOACTIVATE windows do not receive mouse messages, even over the
  // popup itself. Drive click and hover from the same poll that already works.
  POINT cursor{};
  RECT window{};
  const bool got_cursor = GetCursorPos(&cursor) != FALSE && GetWindowRect(hwnd_, &window) != FALSE;
  const bool inside = got_cursor && PtInRect(&window, cursor);

  const bool mouse = (GetAsyncKeyState(VK_LBUTTON) & 0x8000) != 0 || (GetAsyncKeyState(VK_RBUTTON) & 0x8000) != 0 ||
                     (GetAsyncKeyState(VK_MBUTTON) & 0x8000) != 0;
  if (mouse && !mouse_down_) {
    if (armed_ && got_cursor && !inside) {
      Dismiss(-1, DismissReason::kOutsidePoll);
      return;
    }
    if (inside) {
      press_inside_ = true;
    }
  } else if (!mouse && mouse_down_) {
    if (armed_ && press_inside_ && inside && content_ != nullptr) {
      POINT client = cursor;
      ScreenToClient(hwnd_, &client);
      const int row = content_->HitTest(client, Dpi());
      if (row >= 0) {
        Log(L"popup", L"poll invoke row=%d", row);
        press_inside_ = false;
        mouse_down_ = false;
        Dismiss(row, DismissReason::kInvoke);
        return;
      }
    }
    press_inside_ = false;
  }
  mouse_down_ = mouse;
  if (!mouse) {
    armed_ = true;
  }

  ++tick_;
  if (tick_ <= 5 || tick_ % 20 == 0) {
    Log(L"popup", L"alive tick=%u src=%s armed=%d hot=%d inside=%d", tick_, src != nullptr ? src : L"?",
        armed_ ? 1 : 0, hot_, inside ? 1 : 0);
  }

  if (content_ != nullptr) {
    int hot = -1;
    if (inside) {
      POINT client = cursor;
      ScreenToClient(hwnd_, &client);
      hot = content_->HitTest(client, Dpi());
    }
    if (hot != hot_) {
      hot_ = hot;
      InvalidateRect(hwnd_, nullptr, FALSE);
      UpdateWindow(hwnd_);
    }
  }

  const HWND fg = GetForegroundWindow();
  if (fg != last_fg_ && fg != nullptr && fg != hwnd_ && !SameProcess(fg)) {
    Dismiss(-1, DismissReason::kForeground);
    return;
  }
  last_fg_ = fg;
}

void PopupSurface::EnsureRenderTarget() {
  WatchdogStage(L"popup.target");
  if (hwnd_ == nullptr || !d2d_) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const UINT width = static_cast<UINT>((std::max)(0L, client.right - client.left));
  const UINT height = static_cast<UINT>((std::max)(0L, client.bottom - client.top));
  if (width == 0 || height == 0) {
    return;
  }
  if (target_) {
    const D2D1_SIZE_U pixels = target_->GetPixelSize();
    if (pixels.width == width && pixels.height == height) {
      return;
    }
    // Resize keeps the device. Recreating it here costs hundreds of milliseconds
    // and would run on every open, because each menu has its own size.
    if (SUCCEEDED(target_->Resize(D2D1::SizeU(width, height)))) {
      return;
    }
    target_.Reset();
    fill_.Reset();
  }
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT, D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE), 96.0f,
      96.0f);
  const D2D1_HWND_RENDER_TARGET_PROPERTIES hwnd_props =
      D2D1::HwndRenderTargetProperties(hwnd_, D2D1::SizeU(width, height), D2D1_PRESENT_OPTIONS_NONE);
  const ULONGLONG started = GetTickCount64();
  d2d_->CreateHwndRenderTarget(props, hwnd_props, target_.ReleaseAndGetAddressOf());
  Log(L"popup", L"create render target %ux%u ok=%d %ums", width, height, target_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
}

void PopupSurface::Render() {
  WatchdogStage(L"popup.render");
  if (!open_ || hwnd_ == nullptr || content_ == nullptr) {
    return;
  }
  EnsureRenderTarget();
  if (!target_) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const D2D1_COLOR_F fill = DockFillColor(dark_);
  if (!fill_) {
    target_->CreateSolidColorBrush(D2D1::ColorF(fill.r, fill.g, fill.b, 1.0f), fill_.ReleaseAndGetAddressOf());
  } else {
    fill_->SetColor(D2D1::ColorF(fill.r, fill.g, fill.b, 1.0f));
  }
  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(fill.r, fill.g, fill.b, 1.0f));
  if (fill_) {
    target_->FillRectangle(
        D2D1::RectF(0.0f, 0.0f, static_cast<float>(client.right), static_cast<float>(client.bottom)), fill_.Get());
  }
  content_->Render(target_.Get(), Dpi(), hot_);
  const HRESULT hr = target_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    target_.Reset();
    fill_.Reset();
  }
}

UINT PopupSurface::Dpi() const {
  HWND source = hwnd_ != nullptr ? hwnd_ : owner_;
  if (source == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(source);
  return dpi == 0 ? 96 : dpi;
}

LRESULT CALLBACK PopupSurface::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  PopupSurface* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lp);
    self = static_cast<PopupSurface*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<PopupSurface*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->Handle(msg, wp, lp);
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

LRESULT PopupSurface::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      BeginPaint(hwnd_, &ps);
      Render();
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wp == kPopupGuardTimer) {
        Tick(L"timer");
      }
      return 0;
    case WM_MOUSEMOVE: {
      if (!open_ || content_ == nullptr) {
        return 0;
      }
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT client{};
      GetClientRect(hwnd_, &client);
      const int inside = PtInRect(&client, pt) ? 1 : 0;
      if (!saw_mousemove_) {
        saw_mousemove_ = true;
        Log(L"popup", L"msg=%s pt=%d,%d inside=%d", MouseMsgName(msg), pt.x, pt.y, inside);
      }
      const int hot = inside ? content_->HitTest(pt, Dpi()) : -1;
      if (hot != hot_) {
        hot_ = hot;
        InvalidateRect(hwnd_, nullptr, FALSE);
        UpdateWindow(hwnd_);
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT client{};
      GetClientRect(hwnd_, &client);
      const int inside = PtInRect(&client, pt) ? 1 : 0;
      if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        Log(L"popup", L"msg=%s pt=%d,%d inside=%d", MouseMsgName(msg), pt.x, pt.y, inside);
      }
      if (!open_) {
        return 0;
      }
      if (armed_ && !inside) {
        Dismiss(-1, DismissReason::kOutsideClick);
      }
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      RECT client{};
      GetClientRect(hwnd_, &client);
      const int inside = PtInRect(&client, pt) ? 1 : 0;
      Log(L"popup", L"msg=%s pt=%d,%d inside=%d", MouseMsgName(msg), pt.x, pt.y, inside);
      if (!open_ || content_ == nullptr || !armed_) {
        return 0;
      }
      const int index = inside ? content_->HitTest(pt, Dpi()) : -1;
      Dismiss(index, index >= 0 ? DismissReason::kInvoke : DismissReason::kOutsideClick);
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (open_ && reinterpret_cast<HWND>(lp) != hwnd_) {
        Dismiss(-1, DismissReason::kCaptureLost);
      }
      return 0;
    case WM_MOUSEACTIVATE:
      return MA_NOACTIVATE;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
      if (open_ && content_ != nullptr) {
        Place(content_->Measure(Dpi()), anchor_, mode_);
        target_.Reset();
        fill_.Reset();
        InvalidateRect(hwnd_, nullptr, FALSE);
      }
      return 0;
    case WM_DESTROY:
      open_ = false;
      content_ = nullptr;
      hwnd_ = nullptr;
      target_.Reset();
      fill_.Reset();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace bamti
