#include "clock_renderer.hpp"

#include "log.hpp"
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
constexpr float kLogoView = 11.5f;
constexpr wchar_t kFluentFont[] = L"Segoe Fluent Icons";
constexpr wchar_t kFallbackFont[] = L"Segoe UI";
constexpr wchar_t kVolumeMuteFluent[] = L"\xE74F";
constexpr wchar_t kVolume0Fluent[] = L"\xE992";
constexpr wchar_t kVolume1Fluent[] = L"\xE993";
constexpr wchar_t kVolume2Fluent[] = L"\xE994";
constexpr wchar_t kVolume3Fluent[] = L"\xE995";
constexpr wchar_t kNetworkFluent[] = L"\xE839";
constexpr wchar_t kVolumeFallback[] = L"\x266A";
constexpr wchar_t kNetFallback[] = L"\x21C5";

float ClampUnit(float value) {
  if (!std::isfinite(value) || value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

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

bool HasFontFamily(IDWriteFactory* factory, const wchar_t* name) {
  if (factory == nullptr || name == nullptr) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDWriteFontCollection> fonts;
  if (FAILED(factory->GetSystemFontCollection(fonts.GetAddressOf())) || !fonts) {
    return false;
  }
  UINT32 index = 0;
  BOOL exists = FALSE;
  if (FAILED(fonts->FindFamilyName(name, &index, &exists))) {
    return false;
  }
  return exists != FALSE;
}

void AddRoundCornerTile(ID2D1GeometrySink* sink, float x, float y, float w, float r, int corner) {
  // corner: 0 TL, 1 TR, 2 BL, 3 BR
  if (sink == nullptr || w <= 0.0f) {
    return;
  }
  if (r > w * 0.5f) {
    r = w * 0.5f;
  }
  if (corner == 0) {
    sink->BeginFigure(D2D1::Point2F(x + r, y), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w, y));
    sink->AddLine(D2D1::Point2F(x + w, y + w));
    sink->AddLine(D2D1::Point2F(x, y + w));
    sink->AddLine(D2D1::Point2F(x, y + r));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + r, y), D2D1::SizeF(r, r), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  } else if (corner == 1) {
    sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w - r, y));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w, y + r), D2D1::SizeF(r, r), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(x + w, y + w));
    sink->AddLine(D2D1::Point2F(x, y + w));
    sink->AddLine(D2D1::Point2F(x, y));
  } else if (corner == 2) {
    sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w, y));
    sink->AddLine(D2D1::Point2F(x + w, y + w));
    sink->AddLine(D2D1::Point2F(x + r, y + w));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y + w - r), D2D1::SizeF(r, r), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(x, y));
  } else {
    sink->BeginFigure(D2D1::Point2F(x, y), D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(x + w, y));
    sink->AddLine(D2D1::Point2F(x + w, y + w - r));
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + w - r, y + w), D2D1::SizeF(r, r), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
    sink->AddLine(D2D1::Point2F(x, y + w));
    sink->AddLine(D2D1::Point2F(x, y));
  }
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

void DrawBattery(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, bool dark,
                 float level, bool charging) {
  const float ox = box.left;
  const float oy = box.top;
  const float body_l = ox + 0.5f;
  const float body_t = oy + 4.5f;
  const float body_r = ox + 13.5f;
  const float body_b = oy + 11.5f;
  const D2D1_ROUNDED_RECT body{D2D1::RectF(body_l, body_t, body_r, body_b), 1.5f, 1.5f};
  const D2D1_ROUNDED_RECT tip{D2D1::RectF(ox + 14.0f, oy + 6.75f, ox + 15.5f, oy + 9.25f), 0.75f, 0.75f};
  brush->SetColor(ClockTextColor(dark));
  rt->DrawRoundedRectangle(body, brush, 1.0f);
  rt->FillRoundedRectangle(tip, brush);

  const float inner_l = body_l + 1.0f;
  const float inner_t = body_t + 1.0f;
  const float inner_r = body_r - 1.0f;
  const float inner_b = body_b - 1.0f;
  const float inner_w = inner_r - inner_l;
  const float fill_w = inner_w * ClampUnit(level);
  if (fill_w >= 0.5f) {
    brush->SetColor(BatteryFillColor(dark, level, charging));
    const D2D1_ROUNDED_RECT fill{D2D1::RectF(inner_l, inner_t, inner_l + fill_w, inner_b), 0.75f, 0.75f};
    rt->FillRoundedRectangle(fill, brush);
  }

  if (charging) {
    const D2D1_POINT_2F pts[] = {
        D2D1::Point2F(ox + 7.6f, oy + 5.6f), D2D1::Point2F(ox + 5.4f, oy + 8.6f),
        D2D1::Point2F(ox + 6.9f, oy + 8.6f), D2D1::Point2F(ox + 6.4f, oy + 11.4f),
        D2D1::Point2F(ox + 8.6f, oy + 8.2f), D2D1::Point2F(ox + 7.1f, oy + 8.2f),
    };
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    rt->GetFactory(factory.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> bolt;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(bolt.GetAddressOf())) &&
        SUCCEEDED(bolt->Open(sink.GetAddressOf()))) {
      sink->BeginFigure(pts[0], D2D1_FIGURE_BEGIN_FILLED);
      for (int i = 1; i < 6; ++i) {
        sink->AddLine(pts[i]);
      }
      sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      sink->Close();
      brush->SetColor(dark ? D2D1::ColorF(0x0B2E0B) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f));
      rt->FillGeometry(bolt.Get(), brush);
    }
  }
}

void DrawCpuRing(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* stroke,
                 const D2D1_RECT_F& box, bool dark, float usage) {
  const float cx = box.left + 8.0f;
  const float cy = box.top + 8.0f;
  const float r = 5.5f;
  const float thickness = 2.0f;
  D2D1_COLOR_F track = ClockTextColor(dark);
  track.a *= 0.18f;
  brush->SetColor(track);
  rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, thickness, stroke);

  const float value = ClampUnit(usage);
  if (value < 0.005f) {
    return;
  }
  brush->SetColor(CpuRingColor(dark, value));
  if (value >= 0.995f) {
    rt->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush, thickness, stroke);
    return;
  }
  Microsoft::WRL::ComPtr<ID2D1Factory> factory;
  rt->GetFactory(factory.GetAddressOf());
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> arc;
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  if (!factory || FAILED(factory->CreatePathGeometry(arc.GetAddressOf())) || FAILED(arc->Open(sink.GetAddressOf()))) {
    return;
  }
  const float a = value * 6.28318530718f;
  const D2D1_POINT_2F start{cx, cy - r};
  const D2D1_POINT_2F end{cx + r * std::sin(a), cy - r * std::cos(a)};
  sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
  sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                                value > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
  sink->EndFigure(D2D1_FIGURE_END_OPEN);
  sink->Close();
  rt->DrawGeometry(arc.Get(), brush, thickness, stroke);
}

}  // namespace

bool ClockRenderer::Initialize() {
  if (FAILED(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf()))) {
    return false;
  }
  if (FAILED(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                 reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf())))) {
    return false;
  }
  const bool fluent = HasFontFamily(dwrite_.Get(), kFluentFont);
  if (fluent) {
    if (SUCCEEDED(dwrite_->CreateTextFormat(kFluentFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                            DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us",
                                            fluent_format_.ReleaseAndGetAddressOf()))) {
      fluent_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      fluent_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    } else {
      fluent_format_.Reset();
    }
  } else {
    fluent_format_.Reset();
  }
  if (!fluent_format_ && !fluent_missing_logged_) {
    fluent_missing_logged_ = true;
    Log(L"bar", L"Segoe Fluent Icons missing; using fallback glyphs");
  }
  if (SUCCEEDED(dwrite_->CreateTextFormat(kFallbackFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                          DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us",
                                          glyph_format_.ReleaseAndGetAddressOf()))) {
    glyph_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    glyph_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  }
  return true;
}

void ClockRenderer::DropTarget() {
  icons_.Clear();
  logo_brush_.Reset();
  logo_geom_.Reset();
  logo_geom_size_ = 0.0f;
  rt_.Reset();
}

void ClockRenderer::SetDpi(UINT dpi) {
  if (dpi_ == dpi) {
    return;
  }
  dpi_ = dpi == 0 ? 96 : dpi;
  DropTarget();
}

bool ClockRenderer::EnsureStroke() {
  if (round_stroke_) {
    return true;
  }
  if (!d2d_) {
    return false;
  }
  D2D1_STROKE_STYLE_PROPERTIES props{};
  props.startCap = D2D1_CAP_STYLE_ROUND;
  props.endCap = D2D1_CAP_STYLE_ROUND;
  props.dashCap = D2D1_CAP_STYLE_ROUND;
  props.lineJoin = D2D1_LINE_JOIN_ROUND;
  props.miterLimit = 1.0f;
  props.dashStyle = D2D1_DASH_STYLE_SOLID;
  return SUCCEEDED(d2d_->CreateStrokeStyle(props, nullptr, 0, round_stroke_.ReleaseAndGetAddressOf()));
}

bool ClockRenderer::EnsureLogo(float size_dip) {
  if (!d2d_ || !rt_ || size_dip <= 0.0f) {
    return false;
  }
  if (logo_geom_ && logo_brush_ && logo_geom_size_ == size_dip) {
    return true;
  }
  logo_geom_.Reset();
  logo_brush_.Reset();
  const float tile = size_dip * 5.5f / kLogoView;
  const float gap = size_dip * 0.5f / kLogoView;
  const float r = size_dip * 0.5f / kLogoView;
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(d2d_->CreatePathGeometry(logo_geom_.ReleaseAndGetAddressOf())) ||
      FAILED(logo_geom_->Open(sink.GetAddressOf()))) {
    logo_geom_.Reset();
    return false;
  }
  AddRoundCornerTile(sink.Get(), 0.0f, 0.0f, tile, r, 0);
  AddRoundCornerTile(sink.Get(), tile + gap, 0.0f, tile, r, 1);
  AddRoundCornerTile(sink.Get(), 0.0f, tile + gap, tile, r, 2);
  AddRoundCornerTile(sink.Get(), tile + gap, tile + gap, tile, r, 3);
  if (FAILED(sink->Close())) {
    logo_geom_.Reset();
    return false;
  }

  D2D1_GRADIENT_STOP stops[2]{};
  stops[0].position = 0.0f;
  stops[0].color = D2D1::ColorF(0x4DD2FF);
  stops[1].position = 0.75f;
  stops[1].color = D2D1::ColorF(0x0078D4);
  Microsoft::WRL::ComPtr<ID2D1GradientStopCollection> collection;
  if (FAILED(rt_->CreateGradientStopCollection(stops, 2, collection.GetAddressOf()))) {
    logo_geom_.Reset();
    return false;
  }
  D2D1_LINEAR_GRADIENT_BRUSH_PROPERTIES lg{};
  lg.startPoint = D2D1::Point2F(size_dip * 1.74259f / kLogoView, 0.0f);
  lg.endPoint = D2D1::Point2F(size_dip * 9.23502f / kLogoView, size_dip);
  if (FAILED(rt_->CreateLinearGradientBrush(lg, collection.Get(), logo_brush_.ReleaseAndGetAddressOf()))) {
    logo_geom_.Reset();
    return false;
  }
  logo_geom_size_ = size_dip;
  return true;
}

void ClockRenderer::DrawFluentOrFallback(ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, const wchar_t* fluent,
                                         const wchar_t* fallback) {
  IDWriteTextFormat* format = fluent_format_.Get();
  const wchar_t* text = fluent;
  if (format == nullptr || fluent == nullptr) {
    format = glyph_format_.Get();
    text = fallback;
  }
  if (format == nullptr || text == nullptr || brush == nullptr || !dwrite_) {
    return;
  }
  const UINT32 n = static_cast<UINT32>(wcslen(text));
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite_->CreateTextLayout(text, n, format, box.right - box.left, box.bottom - box.top,
                                       layout.GetAddressOf()))) {
    return;
  }
  rt_->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush,
                      D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void ClockRenderer::DrawVectorIcon(ID2D1SolidColorBrush* brush, const StatusIcon& icon, const D2D1_RECT_F& box,
                                   bool dark) {
  if (!rt_ || brush == nullptr) {
    return;
  }
  const float value = ClampUnit(icon.value);
  switch (icon.vector) {
    case VectorIcon::kBattery:
      DrawBattery(rt_.Get(), brush, box, dark, value, (icon.flags & kVectorFlagCharging) != 0);
      break;
    case VectorIcon::kCpu:
      if (EnsureStroke()) {
        DrawCpuRing(rt_.Get(), brush, round_stroke_.Get(), box, dark, value);
      }
      break;
    case VectorIcon::kNetwork:
      brush->SetColor(ClockTextColor(dark));
      DrawFluentOrFallback(brush, box, kNetworkFluent, kNetFallback);
      break;
    case VectorIcon::kVolume: {
      brush->SetColor(ClockTextColor(dark));
      const wchar_t* fluent = kVolume3Fluent;
      if ((icon.flags & kVectorFlagMuted) != 0) {
        fluent = kVolumeMuteFluent;
      } else if (value <= 0.0f) {
        fluent = kVolume0Fluent;
      } else if (value <= 0.33f) {
        fluent = kVolume1Fluent;
      } else if (value <= 0.66f) {
        fluent = kVolume2Fluent;
      }
      DrawFluentOrFallback(brush, box, fluent, kVolumeFallback);
      break;
    }
    default:
      break;
  }
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
  if (!EnsureLogo(kStartLogoDip) || !logo_geom_ || !logo_brush_) {
    return;
  }
  D2D1_MATRIX_3X2_F saved{};
  rt_->GetTransform(&saved);
  rt_->SetTransform(D2D1::Matrix3x2F::Translation(logo_left, logo_top) * saved);
  rt_->FillGeometry(logo_geom_.Get(), logo_brush_.Get());
  rt_->SetTransform(saved);
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
    logo_brush_.Reset();
    logo_geom_size_ = 0.0f;
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
    const bool vector = seg.icon_kind == IconKind::kVector;
    if (seg.text.empty() && !bitmap && !vector) {
      continue;
    }
    const float x0 = static_cast<float>(seg.rect.left - client.left) / px;
    float text_x = x0;
    if (vector) {
      const float icon_top = (height_dip - 16.0f) * 0.5f;
      DrawVectorIcon(brush.Get(), seg.icon, D2D1::RectF(x0, icon_top, x0 + 16.0f, icon_top + 16.0f), dark);
      text_x = x0 + 16.0f + 4.0f;
    } else if (bitmap) {
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
