#include "popup_surface.hpp"

#include "dwm.hpp"
#include "log.hpp"
#include "theme.hpp"
#include "watchdog.hpp"

#include <d2d1helper.h>
#include <dwmapi.h>
#include <windowsx.h>

#include <algorithm>

namespace bamti {
namespace {

constexpr UINT_PTR kPopupGuardTimer = 1;
constexpr UINT kPopupGuardMs = 50;

struct AsyncKey {
  bool down = false;
  bool pressed_since = false;
};

AsyncKey ReadAsyncKey(int vk) {
  const SHORT s = GetAsyncKeyState(vk);
  return {(s & 0x8000) != 0, (s & 0x0001) != 0};
}

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

bool PointInWindow(HWND hwnd, POINT screen) {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return false;
  }
  RECT rc{};
  if (GetWindowRect(hwnd, &rc) == FALSE || !PtInRect(&rc, screen)) {
    return false;
  }
  // Layered ULW_ALPHA windows pass clicks through fully transparent pixels.
  // Treat those pixels as outside so a click on a rounded corner still dismisses.
  return WindowFromPoint(screen) == hwnd;
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
                   ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align) {
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
  layout->SetTextAlignment(align);
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
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kPopupClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf())) || !d2d_) {
    return false;
  }

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, kPopupClass, L"",
                          WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    return false;
  }
  ApplyChrome();
  // Build the render target now, while the window is still hidden. Creating it
  // lazily inside the first present stalls the first menu by the full device
  // setup cost. A later Open() only rebuilds the DIB; the DC target stays.
  const ULONGLONG started = GetTickCount64();
  SetWindowPos(hwnd_, nullptr, 0, 0, 8, 8, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  EnsureLayeredTarget();
  Log(L"popup", L"render target warm=%d %ums", target_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
  return true;
}

void PopupSurface::Destroy() {
  Dismiss(-1, DismissReason::kExplicit);
  HWND w = hwnd_;
  hwnd_ = nullptr;
  owner_ = nullptr;
  ReleaseLayeredTarget();
  d2d_.Reset();
  if (w != nullptr && IsWindow(w)) {
    DestroyWindow(w);
  }
}

bool PopupSurface::Open(PopupContent* content, POINT anchor_screen, Anchor mode, bool capture) {
  WatchdogStage(L"popup.open");
  const ULONGLONG started = GetTickCount64();
  if (hwnd_ == nullptr || content == nullptr) {
    return false;
  }
  Dismiss(-1, DismissReason::kReopen);
  content_ = content;
  mode_ = mode;
  anchor_ = anchor_screen;
  capture_ = capture;
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
  // Layered windows paint via UpdateLayeredWindow, not WM_PAINT. Present first
  // so the first visible frame is already filled; then show without activate.
  Present();
  ShowWindow(hwnd_, SW_SHOWNA);
  if (capture) {
    SetCapture(hwnd_);
    ArmGuardTimer();
  }
  if (capture) {
    Log(L"popup", L"open rows=%d shown %ums", content_->RowCount(),
        static_cast<unsigned>(GetTickCount64() - started));
  } else {
    Log(L"popup", L"submenu open rows=%d %ums", content_->RowCount(),
        static_cast<unsigned>(GetTickCount64() - started));
  }
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
  stroke_.Reset();
  if (hwnd_ != nullptr) {
    ApplyChrome();
    if (open_) {
      Present();
    }
  }
}

void PopupSurface::Dismiss(int invoke_index, DismissReason reason) {
  WatchdogStage(L"popup.dismiss");
  drag_index_ = -1;
  if (!open_) {
    return;
  }
  if (allied_ != nullptr && allied_->IsOpen()) {
    Log(L"popup", L"submenu close reason=%s", ReasonName(reason));
    PopupSurface* allied = allied_;
    allied_ = nullptr;
    allied->Close();
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
  if (capture_ && owner_ != nullptr) {
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
  int x = 0;
  int y = 0;
  if (mode == Anchor::RightOf) {
    x = anchor_screen.x;
    y = anchor_screen.y;
    if (x + size.cx > info.rcWork.right) {
      x = anchor_screen.x - size.cx;
    }
  } else {
    x = anchor_screen.x - inset;
    y = mode == Anchor::AboveAt ? anchor_screen.y - size.cy - gap : anchor_screen.y + gap;
  }
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
  const int corner = dwm::kCornerDoNotRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));
}

void PopupSurface::ArmGuardTimer() {
  if (hwnd_ == nullptr) {
    return;
  }
  const UINT_PTR id = SetTimer(hwnd_, kPopupGuardTimer, kPopupGuardMs, nullptr);
  Log(L"popup", L"arm guard id=%llu err=%lu", static_cast<unsigned long long>(id), id == 0 ? GetLastError() : 0);
}

void PopupSurface::HitTree(POINT screen, bool* in_self, bool* in_allied) const {
  if (in_self != nullptr) {
    *in_self = PointInWindow(hwnd_, screen);
  }
  if (in_allied != nullptr) {
    *in_allied = allied_ != nullptr && allied_->IsOpen() && PointInWindow(allied_->hwnd(), screen);
  }
}

int PopupSurface::HitTestScreen(POINT screen) const {
  if (!open_ || content_ == nullptr || hwnd_ == nullptr || !PointInWindow(hwnd_, screen)) {
    return -1;
  }
  POINT client = screen;
  ScreenToClient(hwnd_, &client);
  return content_->HitTest(client, Dpi());
}

void PopupSurface::TrackHotScreen(POINT screen) {
  if (!open_ || content_ == nullptr || hwnd_ == nullptr) {
    return;
  }
  const int hot = HitTestScreen(screen);
  if (hot == hot_) {
    return;
  }
  hot_ = hot;
  Present();
}

void PopupSurface::InvokeRow(int index) {
  Dismiss(index, DismissReason::kInvoke);
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

  const AsyncKey esc = ReadAsyncKey(VK_ESCAPE);
  const bool esc_hit = esc.down || esc.pressed_since;
  if (esc_hit && !esc_down_) {
    Dismiss(-1, DismissReason::kEscape);
    return;
  }
  esc_down_ = esc.down;

  const AsyncKey lwin = ReadAsyncKey(VK_LWIN);
  const AsyncKey rwin = ReadAsyncKey(VK_RWIN);
  const bool win_hit = lwin.down || lwin.pressed_since || rwin.down || rwin.pressed_since;
  if (win_hit && !win_down_) {
    Dismiss(-1, DismissReason::kWinKey);
    return;
  }
  win_down_ = lwin.down || rwin.down;

  // Capture on a WS_EX_NOACTIVATE popup only delivers messages while the
  // cursor is over that window. Poll dismisses outside clicks and keeps hover
  // in sync. Inner mouse-up reaches WM_LBUTTONUP, so this path must not invoke.
  POINT cursor{};
  RECT window{};
  const bool got_cursor = GetCursorPos(&cursor) != FALSE && GetWindowRect(hwnd_, &window) != FALSE;
  bool in_self = false;
  bool in_allied = false;
  if (got_cursor) {
    HitTree(cursor, &in_self, &in_allied);
  }
  const bool inside = in_self || in_allied;

  const AsyncKey left = ReadAsyncKey(VK_LBUTTON);
  const AsyncKey right = ReadAsyncKey(VK_RBUTTON);
  const AsyncKey middle = ReadAsyncKey(VK_MBUTTON);
  const bool down = left.down || right.down || middle.down;
  const bool pressed_since = left.pressed_since || right.pressed_since || middle.pressed_since;
  const bool saw_press = down || pressed_since;
  if (drag_index_ < 0 && saw_press && !mouse_down_) {
    if (armed_ && got_cursor && !inside) {
      Dismiss(-1, DismissReason::kOutsidePoll);
      return;
    }
    if (inside) {
      press_inside_ = true;
    }
  }
  if (!down && (mouse_down_ || (pressed_since && press_inside_))) {
    press_inside_ = false;
  }
  mouse_down_ = down;
  if (!down) {
    armed_ = true;
  }

  ++tick_;
  if (tick_ <= 5 || tick_ % 20 == 0) {
    Log(L"popup", L"alive tick=%u src=%s armed=%d hot=%d inside=%d", tick_, src != nullptr ? src : L"?",
        armed_ ? 1 : 0, hot_, inside ? 1 : 0);
  }

  if (drag_index_ < 0 && content_ != nullptr) {
    int hot = hot_;
    if (in_self) {
      POINT client = cursor;
      ScreenToClient(hwnd_, &client);
      hot = content_->HitTest(client, Dpi());
    } else if (!in_allied) {
      hot = -1;
    }
    if (hot != hot_) {
      hot_ = hot;
      Present();
    }
  }
  if (allied_ != nullptr && allied_->IsOpen()) {
    allied_->TrackHotScreen(cursor);
  }

  const HWND fg = GetForegroundWindow();
  if (fg != last_fg_ && fg != nullptr && fg != hwnd_ && !SameProcess(fg)) {
    Dismiss(-1, DismissReason::kForeground);
    return;
  }
  last_fg_ = fg;
  if (open_ && after_tick_ != nullptr) {
    after_tick_(after_tick_ctx_);
  }
}

void PopupSurface::ReleaseLayeredTarget() {
  fill_.Reset();
  stroke_.Reset();
  target_.Reset();
  squircle_.Reset();
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

void PopupSurface::EnsureLayeredTarget() {
  WatchdogStage(L"popup.target");
  if (hwnd_ == nullptr || !d2d_) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int width = (std::max)(0L, client.right - client.left);
  const int height = (std::max)(0L, client.bottom - client.top);
  if (width == 0 || height == 0) {
    return;
  }
  const bool size_ok = dib_ != nullptr && mem_dc_ != nullptr && dib_w_ == width && dib_h_ == height;
  if (!size_ok) {
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
  if (target_) {
    return;
  }
  fill_.Reset();
  stroke_.Reset();
  squircle_.Reset();
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  const ULONGLONG started = GetTickCount64();
  d2d_->CreateDCRenderTarget(&props, target_.ReleaseAndGetAddressOf());
  Log(L"popup", L"create render target %ux%u ok=%d %ums", static_cast<unsigned>(width),
      static_cast<unsigned>(height), target_ ? 1 : 0, static_cast<unsigned>(GetTickCount64() - started));
}

void PopupSurface::Present() {
  if (presenting_) {
    return;
  }
  presenting_ = true;
  struct PresentGuard {
    bool& busy;
    explicit PresentGuard(bool& flag) : busy(flag) {}
    ~PresentGuard() { busy = false; }
  } guard(presenting_);
  Render();
  if (hwnd_ == nullptr || mem_dc_ == nullptr || dib_w_ <= 0 || dib_h_ <= 0) {
    return;
  }
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT src{0, 0};
  SIZE size{dib_w_, dib_h_};
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
}

void PopupSurface::Render() {
  WatchdogStage(L"popup.render");
  if (!open_ || hwnd_ == nullptr || content_ == nullptr) {
    return;
  }
  EnsureLayeredTarget();
  if (!target_ || mem_dc_ == nullptr || dib_w_ <= 0 || dib_h_ <= 0) {
    return;
  }
  RECT client{0, 0, dib_w_, dib_h_};
  if (FAILED(target_->BindDC(mem_dc_, &client))) {
    target_.Reset();
    fill_.Reset();
    stroke_.Reset();
    squircle_.Reset();
    EnsureLayeredTarget();
    if (!target_ || FAILED(target_->BindDC(mem_dc_, &client))) {
      return;
    }
  }
  const float width = static_cast<float>(dib_w_);
  const float height = static_cast<float>(dib_h_);
  const bool own_chrome = content_->PaintsOwnChrome();
  const int tier = content_->CornerDip();
  const float radius = corner::ClampPx(corner::ToPx(tier, Dpi()), width, height);
  const D2D1_ROUNDED_RECT rounded{D2D1::RectF(0.5f, 0.5f, width - 0.5f, height - 0.5f), radius, radius};
  const D2D1_COLOR_F fill = DockFillColor(dark_);
  const D2D1_COLOR_F stroke = DockStrokeColor(dark_);
  if (!fill_) {
    target_->CreateSolidColorBrush(fill, fill_.ReleaseAndGetAddressOf());
  } else {
    fill_->SetColor(fill);
  }
  if (!stroke_) {
    target_->CreateSolidColorBrush(stroke, stroke_.ReleaseAndGetAddressOf());
  } else {
    stroke_->SetColor(stroke);
  }
  target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  if (!own_chrome) {
    ID2D1PathGeometry* squircle =
        corner::IsHero(tier) ? squircle_.Get(d2d_.Get(), rounded.rect, radius) : nullptr;
    if (fill_) {
      if (squircle != nullptr) {
        target_->FillGeometry(squircle, fill_.Get());
      } else {
        target_->FillRoundedRectangle(rounded, fill_.Get());
      }
    }
    if (stroke_) {
      if (squircle != nullptr) {
        target_->DrawGeometry(squircle, stroke_.Get(), 1.0f);
      } else {
        target_->DrawRoundedRectangle(rounded, stroke_.Get(), 1.0f);
      }
    }
  }
  content_->Render(target_.Get(), Dpi(), hot_);
  const HRESULT hr = target_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    target_.Reset();
    fill_.Reset();
    stroke_.Reset();
    squircle_.Reset();
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
      Log(L"popup", L"wm_paint");
      Present();
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_TIMER:
      if (wp == kPopupGuardTimer) {
        Tick(L"timer");
      }
      return 0;
    case WM_MOUSEMOVE: {
      WatchdogStage(L"popup.mousemove");
      if (!open_ || content_ == nullptr) {
        return 0;
      }
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      if (drag_index_ >= 0) {
        content_->DragTo(drag_index_, pt, Dpi());
        Present();
        return 0;
      }
      POINT screen = pt;
      ClientToScreen(hwnd_, &screen);
      bool in_self = false;
      bool in_allied = false;
      HitTree(screen, &in_self, &in_allied);
      const int inside = in_self ? 1 : 0;
      if (!saw_mousemove_) {
        saw_mousemove_ = true;
        Log(L"popup", L"msg=%s pt=%d,%d inside=%d", MouseMsgName(msg), pt.x, pt.y, inside);
      }
      int hot = hot_;
      if (in_self) {
        hot = content_->HitTest(pt, Dpi());
      } else if (!in_allied) {
        hot = -1;
      }
      if (hot != hot_) {
        hot_ = hot;
        Present();
      }
      if (in_allied && allied_ != nullptr) {
        allied_->TrackHotScreen(screen);
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
    case WM_MBUTTONDOWN: {
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      POINT screen = pt;
      ClientToScreen(hwnd_, &screen);
      bool in_self = false;
      bool in_allied = false;
      HitTree(screen, &in_self, &in_allied);
      const int inside = (in_self || in_allied) ? 1 : 0;
      if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN) {
        Log(L"popup", L"msg=%s pt=%d,%d inside=%d", MouseMsgName(msg), pt.x, pt.y, inside);
      }
      if (!open_) {
        return 0;
      }
      if (armed_ && !inside) {
        Dismiss(-1, DismissReason::kOutsideClick);
        return 0;
      }
      if (msg == WM_LBUTTONDOWN && in_self && content_ != nullptr) {
        const int index = content_->HitTest(pt, Dpi());
        if (index >= 0 && content_->DragRow(index)) {
          drag_index_ = index;
          content_->DragTo(index, pt, Dpi());
          Present();
          return 0;
        }
      }
      return 0;
    }
    case WM_LBUTTONUP:
    case WM_RBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
      POINT screen = pt;
      ClientToScreen(hwnd_, &screen);
      bool in_self = false;
      bool in_allied = false;
      HitTree(screen, &in_self, &in_allied);
      const int inside = (in_self || in_allied) ? 1 : 0;
      Log(L"popup", L"msg=%s pt=%d,%d inside=%d", MouseMsgName(msg), pt.x, pt.y, inside);
      if (!open_ || content_ == nullptr || !armed_) {
        return 0;
      }
      if (drag_index_ >= 0) {
        const int index = drag_index_;
        drag_index_ = -1;
        press_inside_ = false;
        mouse_down_ = false;
        content_->DragEnd(index);
        Present();
        if (after_tick_ != nullptr) {
          after_tick_(after_tick_ctx_);
        }
        return 0;
      }
      if (in_allied && allied_ != nullptr) {
        const int row = allied_->HitTestScreen(screen);
        if (row >= 0) {
          Log(L"popup", L"submenu close reason=%s", L"invoke");
          press_inside_ = false;
          mouse_down_ = false;
          allied_->InvokeRow(row);
          Dismiss(-1, DismissReason::kInvoke);
        }
        return 0;
      }
      if (!in_self) {
        Dismiss(-1, DismissReason::kOutsideClick);
        return 0;
      }
      const int index = content_->HitTest(pt, Dpi());
      if (index >= 0 && content_->StickyRow(index)) {
        press_inside_ = false;
        mouse_down_ = false;
        content_->StickyInvoke(index);
        if (open_ && content_ != nullptr) {
          Place(content_->Measure(Dpi()), anchor_, mode_);
          Present();
        }
        if (after_tick_ != nullptr) {
          after_tick_(after_tick_ctx_);
        }
        return 0;
      }
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
        Present();
      }
      return 0;
    case WM_DESTROY:
      open_ = false;
      content_ = nullptr;
      drag_index_ = -1;
      hwnd_ = nullptr;
      ReleaseLayeredTarget();
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace bamti
