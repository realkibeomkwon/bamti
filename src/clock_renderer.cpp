#include "clock_renderer.hpp"

#include "theme.hpp"

#include <d2d1helper.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace bamti {
namespace {

constexpr float kStartHitWidthDip = 34.0f;
constexpr float kStartLogoDip = 20.0f;
constexpr float kStartHoverInsetXDip = 2.0f;
constexpr float kStartHoverInsetYDip = 4.0f;
constexpr float kStartHoverRadiusDip = 6.0f;
// @WLOGO_96x96.png glyph is 80px with 38px tiles and a 4px gap, color #0078D4.
constexpr float kWindowsLogoGap = 4.0f / 80.0f;

D2D1_COLOR_F StatusItemColor(bool dark, uint32_t accent) {
  if (accent != 0) {
    return D2D1::ColorF(accent);
  }
  return ClockTextColor(dark);
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

void FillWindowsLogo(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float left_dip, float top_dip, float size_dip,
                     UINT dpi) {
  const float scale = static_cast<float>(dpi) / 96.0f;
  const int origin_x = static_cast<int>(std::lround(left_dip * scale));
  const int origin_y = static_cast<int>(std::lround(top_dip * scale));
  int size_px = std::max(8, static_cast<int>(std::lround(size_dip * scale)));
  int gap_px = std::max(1, static_cast<int>(std::lround(static_cast<float>(size_px) * kWindowsLogoGap)));
  if ((size_px - gap_px) % 2 != 0) {
    ++gap_px;
  }
  const int tile_px = (size_px - gap_px) / 2;
  const float inv = 96.0f / static_cast<float>(dpi);
  auto pixel_rect = [inv](int x, int y, int w, int h) {
    return D2D1::RectF(static_cast<float>(x) * inv, static_cast<float>(y) * inv,
                       static_cast<float>(x + w) * inv, static_cast<float>(y + h) * inv);
  };

  brush->SetColor(D2D1::ColorF(0x0078D4));
  const D2D1_ANTIALIAS_MODE previous = rt->GetAntialiasMode();
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  rt->FillRectangle(pixel_rect(origin_x, origin_y, tile_px, tile_px), brush);
  rt->FillRectangle(pixel_rect(origin_x + tile_px + gap_px, origin_y, tile_px, tile_px), brush);
  rt->FillRectangle(pixel_rect(origin_x, origin_y + tile_px + gap_px, tile_px, tile_px), brush);
  rt->FillRectangle(pixel_rect(origin_x + tile_px + gap_px, origin_y + tile_px + gap_px, tile_px, tile_px), brush);
  rt->SetAntialiasMode(previous);
}

}  // namespace

bool ClockRenderer::Initialize() {
  const HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf());
  return SUCCEEDED(hr);
}

void ClockRenderer::DropTarget() {
  icons_.Clear();
  rt_.Reset();
}

void ClockRenderer::SetDpi(UINT dpi) {
  if (dpi_ == dpi) {
    return;
  }
  dpi_ = dpi == 0 ? 96 : dpi;
  DropTarget();
}

void ClockRenderer::DrawStartButton(ID2D1SolidColorBrush* brush, bool dark, bool hot, bool pressed, float height_dip,
                                    const RECT& client, const RECT& start_rect) {
  const float px = static_cast<float>(dpi_) / 96.0f;
  const float hit_left = static_cast<float>(start_rect.left - client.left) / px;
  const float hit_right = static_cast<float>(start_rect.right - client.left) / px;
  if (hot || pressed) {
    brush->SetColor(MenuItemHoverFill(dark, pressed));
    const D2D1_ROUNDED_RECT hover{
        D2D1::RectF(hit_left + kStartHoverInsetXDip, kStartHoverInsetYDip, hit_right - kStartHoverInsetXDip,
                    height_dip - kStartHoverInsetYDip),
        kStartHoverRadiusDip, kStartHoverRadiusDip};
    rt_->FillRoundedRectangle(hover, brush);
  }

  const float logo_left = hit_left + (kStartHitWidthDip - kStartLogoDip) * 0.5f;
  const float logo_top = (height_dip - kStartLogoDip) * 0.5f;
  FillWindowsLogo(rt_.Get(), brush, logo_left, logo_top, kStartLogoDip, dpi_);
}

std::wstring ClockRenderer::CurrentTimeText() const {
  SYSTEMTIME local{};
  GetLocalTime(&local);

  wchar_t date[128]{};
  wchar_t time[64]{};
  const int date_ok =
      GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, nullptr, date, 128, nullptr);
  const int time_ok = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, time, 64);
  if (date_ok <= 1 || time_ok <= 1) {
    return L"";
  }

  std::wstring text;
  text.append(date);
  text.push_back(L' ');
  text.append(time);
  return text;
}

bool ClockRenderer::Draw(HDC hdc, const RECT& client, const RECT& dirty, bool dark, const BarLayoutResult& layout,
                         BarLayout* text, bool start_hot, bool start_pressed, DrawTimings* timings) {
  if (!d2d_ || !hdc || text == nullptr) {
    return false;
  }

  if (!rt_) {
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));
    const HRESULT hr = d2d_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
    icons_.SetRenderTarget(rt_.Get());
  }
  icons_.SetDark(dark);

  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  HRESULT hr = rt_->BindDC(hdc, &dirty);
  QueryPerformanceCounter(&t1);
  if (timings != nullptr) {
    timings->bind_ms = QpcMs(t0, t1);
  }
  if (FAILED(hr)) {
    DropTarget();
    return false;
  }

  const float px = static_cast<float>(dpi_) / 96.0f;
  const float height_dip =
      static_cast<float>(client.bottom - client.top) * 96.0f / static_cast<float>(dpi_);

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  QueryPerformanceCounter(&t0);
  hr = rt_->CreateSolidColorBrush(ClockTextColor(dark), brush.ReleaseAndGetAddressOf());
  QueryPerformanceCounter(&t1);
  if (timings != nullptr) {
    timings->brush_ms = QpcMs(t0, t1);
  }
  if (FAILED(hr)) {
    return false;
  }

  QueryPerformanceCounter(&t0);
  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));
  rt_->SetTransform(D2D1::Matrix3x2F::Translation(-static_cast<float>(dirty.left - client.left) / px,
                                                  -static_cast<float>(dirty.top - client.top) / px));
  QueryPerformanceCounter(&t1);
  if (timings != nullptr) {
    timings->begin_ms = QpcMs(t0, t1);
  }

  QueryPerformanceCounter(&t0);
  for (const BarSegment& seg : layout.segments) {
    RECT overlap{};
    if (IntersectRect(&overlap, &seg.rect, &dirty) == FALSE) {
      continue;
    }
    if (seg.kind == SegmentKind::kStart) {
      DrawStartButton(brush.Get(), dark, start_hot, start_pressed, height_dip, client, seg.rect);
      continue;
    }
    const bool bitmap =
        seg.icon_kind == IconKind::kPng || seg.icon_kind == IconKind::kFile || seg.icon_kind == IconKind::kHicon;
    if (seg.text.empty() && !bitmap) {
      continue;
    }
    const float x0 = static_cast<float>(seg.rect.left - client.left) / px;
    float text_x = x0;
    if (bitmap) {
      const int icon_px = (std::max)(1, static_cast<int>(std::lround(16.0f * px)));
      if (ID2D1Bitmap* bmp = icons_.Get(seg.icon, icon_px)) {
        const float icon_top = (height_dip - 16.0f) * 0.5f;
        rt_->DrawBitmap(bmp, D2D1::RectF(x0, icon_top, x0 + 16.0f, icon_top + 16.0f), 1.0f,
                        D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        text_x = x0 + 16.0f + 4.0f;
      }
    }
    if (seg.text.empty()) {
      continue;
    }
    IDWriteTextLayout* layout_text = text->LayoutFor(seg.text);
    if (layout_text == nullptr) {
      continue;
    }
    DWRITE_TEXT_METRICS metrics{};
    if (FAILED(layout_text->GetMetrics(&metrics))) {
      continue;
    }
    const float y = (height_dip - metrics.height) * 0.5f;
    switch (seg.kind) {
      case SegmentKind::kWarning:
        brush->SetColor(WarningTextColor(dark));
        break;
      case SegmentKind::kStatus:
        brush->SetColor(StatusItemColor(dark, seg.accent));
        break;
      default:
        brush->SetColor(ClockTextColor(dark));
        break;
    }
    rt_->DrawTextLayout(D2D1::Point2F(text_x, y), layout_text, brush.Get(), D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
  }
  QueryPerformanceCounter(&t1);
  if (timings != nullptr) {
    timings->draw_ms = QpcMs(t0, t1);
  }

  QueryPerformanceCounter(&t0);
  hr = rt_->EndDraw();
  QueryPerformanceCounter(&t1);
  if (timings != nullptr) {
    timings->end_ms = QpcMs(t0, t1);
  }
  if (hr == D2DERR_RECREATE_TARGET) {
    DropTarget();
    return false;
  }
  return SUCCEEDED(hr);
}

}  // namespace bamti
