#include "dock_label.hpp"

#include "corner.hpp"
#include "popup_surface.hpp"
#include "theme.hpp"

#include <d2d1helper.h>

#include <algorithm>
#include <cmath>

namespace bamti {
namespace {

constexpr int kLabelPadXDip = 16;
constexpr int kLabelHeightDip = 26;
// 메뉴 꼬리와 같은 2.5 대 1 비율이되, 알약 높이 26 DIP 에 맞게 줄인 값이다.
constexpr int kLabelTailBaseDip = 20;
constexpr int kLabelTailHeightDip = 8;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

}  // namespace

DockLabel::~DockLabel() {
  Destroy();
}

bool DockLabel::Create(HINSTANCE instance, HWND owner) {
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
  wc.lpszClassName = kDockLabelClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf())) || !d2d_) {
    return false;
  }

  hwnd_ = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TOOLWINDOW, kDockLabelClass, L"",
      WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    return false;
  }
  SetWindowPos(hwnd_, nullptr, 0, 0, 8, 8, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  EnsureLayeredTarget();
  return true;
}

void DockLabel::Destroy() {
  Hide();
  HWND w = hwnd_;
  hwnd_ = nullptr;
  owner_ = nullptr;
  ReleaseLayeredTarget();
  d2d_.Reset();
  if (w != nullptr && IsWindow(w)) {
    DestroyWindow(w);
  }
}

void DockLabel::Show(const std::wstring& text, POINT icon_center_screen, int dock_top, bool dark) {
  if (hwnd_ == nullptr || text.empty()) {
    Hide();
    return;
  }
  text_ = text;
  dark_ = dark;

  const UINT dpi = Dpi();
  const int pad_x = DipToPx(kLabelPadXDip, dpi);
  const int body = DipToPx(kLabelHeightDip, dpi);
  const int tail = DipToPx(kLabelTailHeightDip, dpi);
  const int tail_base = DipToPx(kLabelTailBaseDip, dpi);
  const int gap = DipToPx(8, dpi);
  const int text_w = static_cast<int>(std::ceil(PopupTextWidth(dpi, text_)));
  int width = (std::max)(body, text_w + pad_x * 2);
  // 알약은 양 끝이 반원이라 밑변이 놓일 곳이 좁다. 짧은 이름에서 꼬리가
  // 조각으로 줄지 않게 최소 너비를 준다.
  width = (std::max)(width, tail_base + body + DipToPx(4, dpi));
  const int height = body + tail;
  if (width <= 0 || height <= 0) {
    Hide();
    return;
  }

  int x = icon_center_screen.x - width / 2;
  const int y = dock_top - gap - height;
  HMONITOR mon = MonitorFromPoint(icon_center_screen, MONITOR_DEFAULTTONEAREST);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(mon, &info) != FALSE) {
    if (x < info.rcMonitor.left) {
      x = info.rcMonitor.left;
    }
    if (x + width > info.rcMonitor.right) {
      x = info.rcMonitor.right - width;
    }
    if (x < info.rcMonitor.left) {
      x = info.rcMonitor.left;
    }
  }

  body_px_ = body;
  tail_px_ = tail;
  apex_px_ = static_cast<float>(icon_center_screen.x - x);

  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  Present();
  if (!shown_) {
    ShowWindow(hwnd_, SW_SHOWNA);
    shown_ = true;
  }
}

void DockLabel::Hide() {
  if (!shown_) {
    return;
  }
  shown_ = false;
  if (hwnd_ != nullptr) {
    ShowWindow(hwnd_, SW_HIDE);
  }
}

void DockLabel::ReleaseLayeredTarget() {
  target_.Reset();
  callout_.Reset();
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

void DockLabel::EnsureLayeredTarget() {
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
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  d2d_->CreateDCRenderTarget(&props, target_.ReleaseAndGetAddressOf());
}

void DockLabel::Present() {
  if (hwnd_ == nullptr) {
    return;
  }
  EnsureLayeredTarget();
  if (!target_ || mem_dc_ == nullptr || dib_w_ <= 0 || dib_h_ <= 0) {
    return;
  }
  RECT client{0, 0, dib_w_, dib_h_};
  if (FAILED(target_->BindDC(mem_dc_, &client))) {
    target_.Reset();
    callout_.Reset();
    EnsureLayeredTarget();
    if (!target_ || FAILED(target_->BindDC(mem_dc_, &client))) {
      return;
    }
  }

  const float width = static_cast<float>(dib_w_);
  const float height = static_cast<float>(dib_h_);
  const float body = height - static_cast<float>(tail_px_);
  const float radius = corner::PillPx(body);
  const D2D1_ROUNDED_RECT pill{D2D1::RectF(0.5f, 0.5f, width - 0.5f, body - 0.5f), radius, radius};
  const UINT dpi = Dpi();
  const float pad_x = static_cast<float>(DipToPx(kLabelPadXDip, dpi));

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
  target_->CreateSolidColorBrush(DockLabelFill(dark_), fill.GetAddressOf());
  target_->CreateSolidColorBrush(DockStrokeColor(dark_), stroke.GetAddressOf());
  target_->CreateSolidColorBrush(DockLabelText(dark_), text.GetAddressOf());

  target_->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  target_->BeginDraw();
  target_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  corner::Tail tail{};
  tail.apex_x = apex_px_;
  tail.base_px = static_cast<float>(DipToPx(kLabelTailBaseDip, dpi));
  tail.height_px = static_cast<float>(tail_px_);
  tail.tip_px = corner::ToPx(corner::kTailTipDip, dpi);
  ID2D1PathGeometry* shape = tail_px_ > 0 ? callout_.Get(d2d_.Get(), pill.rect, radius, tail) : nullptr;
  if (fill) {
    if (shape != nullptr) {
      target_->FillGeometry(shape, fill.Get());
    } else {
      target_->FillRoundedRectangle(pill, fill.Get());
    }
  }
  if (stroke) {
    if (shape != nullptr) {
      target_->DrawGeometry(shape, stroke.Get(), 1.0f);
    } else {
      target_->DrawRoundedRectangle(pill, stroke.Get(), 1.0f);
    }
  }
  if (text) {
    DrawPopupText(target_.Get(), dpi, text_, D2D1::RectF(pad_x, 0.0f, width - pad_x, body), text.Get(),
                  DWRITE_TEXT_ALIGNMENT_CENTER);
  }
  const HRESULT hr = target_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    target_.Reset();
  }

  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT src{0, 0};
  SIZE size{dib_w_, dib_h_};
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, mem_dc_, &src, 0, &blend, ULW_ALPHA);
}

UINT DockLabel::Dpi() const {
  HWND source = owner_ != nullptr ? owner_ : hwnd_;
  if (source == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(source);
  return dpi == 0 ? 96 : dpi;
}

LRESULT CALLBACK DockLabel::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  DockLabel* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<DockLabel*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<DockLabel*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->Handle(msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT DockLabel::Handle(UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_NCDESTROY:
      hwnd_ = nullptr;
      shown_ = false;
      return 0;
    default:
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
  }
}

}  // namespace bamti
