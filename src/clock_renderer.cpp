#include "clock_renderer.hpp"

#include "corner.hpp"
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
constexpr float kStartHoverInsetXDip = 3.0f;
constexpr float kStartHoverInsetYDip = 3.0f;
constexpr float kBatteryIconDip = 24.0f;
constexpr float kLogoView = 11.5f;
constexpr wchar_t kFluentFont[] = L"Segoe Fluent Icons";
constexpr wchar_t kCcFluent[] = L"\xE9E9";
constexpr wchar_t kFallbackFont[] = L"Segoe UI";
constexpr wchar_t kVolumeMuteFluent[] = L"\xE74F";
constexpr wchar_t kVolume0Fluent[] = L"\xE992";
constexpr wchar_t kVolume1Fluent[] = L"\xE993";
constexpr wchar_t kVolume2Fluent[] = L"\xE994";
constexpr wchar_t kVolume3Fluent[] = L"\xE995";
constexpr wchar_t kNetworkFluent[] = L"\xE839";
constexpr wchar_t kWifiFluent[] = L"\xE701";
constexpr wchar_t kVolumeFallback[] = L"\x266A";
constexpr wchar_t kNetFallback[] = L"\x21C5";
constexpr wchar_t kWifiFallback[] = L"Wi";

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

void AddRoundedPolygon(ID2D1GeometrySink* sink, const D2D1_POINT_2F* pts, int n, float radius) {
  if (sink == nullptr || pts == nullptr || n < 3) {
    return;
  }
  auto len = [](D2D1_POINT_2F a, D2D1_POINT_2F b) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    return std::sqrt(dx * dx + dy * dy);
  };
  for (int i = 0; i < n; ++i) {
    const D2D1_POINT_2F prev = pts[(i + n - 1) % n];
    const D2D1_POINT_2F cur = pts[i];
    const D2D1_POINT_2F next = pts[(i + 1) % n];
    const float d0 = len(cur, prev);
    const float d1 = len(cur, next);
    const float r0 = (std::min)(radius, d0 * 0.42f);
    const float r1 = (std::min)(radius, d1 * 0.42f);
    const D2D1_POINT_2F p0 =
        d0 > 0.001f ? D2D1::Point2F(cur.x + (prev.x - cur.x) * (r0 / d0), cur.y + (prev.y - cur.y) * (r0 / d0)) : cur;
    const D2D1_POINT_2F p1 =
        d1 > 0.001f ? D2D1::Point2F(cur.x + (next.x - cur.x) * (r1 / d1), cur.y + (next.y - cur.y) * (r1 / d1)) : cur;
    if (i == 0) {
      sink->BeginFigure(p0, D2D1_FIGURE_BEGIN_FILLED);
    } else {
      sink->AddLine(p0);
    }
    sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(cur, p1));
  }
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);
}

void DrawBattery(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, bool dark,
                 float level, bool charging) {
  const float w = box.right - box.left;
  const float h = box.bottom - box.top;
  if (w < 4.0f || h < 4.0f) {
    return;
  }
  const D2D1_COLOR_F outline = ClockTextColor(dark);
  const float stroke = 1.0f;
  const float nub_w = (std::max)(1.25f, w * 0.07f);
  const float body_h = (std::min)(h * 0.68f, w * 0.48f);
  const float body_w = w - nub_w - stroke;
  const float nub_h = body_h * 0.38f;
  const float body_l = box.left + (w - body_w - nub_w) * 0.5f;
  const float body_t = box.top + (h - body_h) * 0.5f;
  const float body_r = body_l + body_w;
  const float body_b = body_t + body_h;
  const float radius = (std::min)(2.0f, body_h * 0.20f);
  const D2D1_ROUNDED_RECT body{D2D1::RectF(body_l, body_t, body_r, body_b), radius, radius};
  const D2D1_ROUNDED_RECT tip{
      D2D1::RectF(body_r, body_t + (body_h - nub_h) * 0.5f, body_r + nub_w, body_t + (body_h + nub_h) * 0.5f),
      nub_w * 0.45f, nub_w * 0.45f};
  brush->SetColor(outline);
  rt->DrawRoundedRectangle(body, brush, stroke);
  rt->FillRoundedRectangle(tip, brush);

  const float inset = stroke + 0.85f;
  const float inner_l = body_l + inset;
  const float inner_t = body_t + inset;
  const float inner_r = body_r - inset;
  const float inner_b = body_b - inset;
  const float inner_w = inner_r - inner_l;
  const float fill_w = inner_w * ClampUnit(level);
  if (fill_w >= 0.6f && inner_b > inner_t) {
    if (charging || level <= 0.20f) {
      brush->SetColor(BatteryFillColor(dark, level, charging));
    } else {
      brush->SetColor(outline);
    }
    const float fill_r = (std::min)(radius * 0.50f, (inner_b - inner_t) * 0.40f);
    const D2D1_ROUNDED_RECT fill{D2D1::RectF(inner_l, inner_t, inner_l + fill_w, inner_b), fill_r, fill_r};
    rt->FillRoundedRectangle(fill, brush);
  }

  if (charging) {
    const float cx = (body_l + body_r) * 0.5f;
    const float cy = (body_t + body_b) * 0.5f;
    const float s = body_h / 3.9f;
    const D2D1_POINT_2F pts[] = {
        D2D1::Point2F(cx + 0.55f * s, cy - 2.55f * s), D2D1::Point2F(cx - 1.60f * s, cy + 0.22f * s),
        D2D1::Point2F(cx - 0.62f * s, cy + 0.22f * s), D2D1::Point2F(cx - 0.55f * s, cy + 2.55f * s),
        D2D1::Point2F(cx + 1.60f * s, cy - 0.22f * s), D2D1::Point2F(cx + 0.62f * s, cy - 0.22f * s),
    };
    Microsoft::WRL::ComPtr<ID2D1Factory> factory;
    rt->GetFactory(factory.GetAddressOf());
    Microsoft::WRL::ComPtr<ID2D1PathGeometry> bolt;
    Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
    if (factory && SUCCEEDED(factory->CreatePathGeometry(bolt.GetAddressOf())) &&
        SUCCEEDED(bolt->Open(sink.GetAddressOf()))) {
      AddRoundedPolygon(sink.Get(), pts, 6, s * 0.50f);
      sink->Close();
      brush->SetColor(D2D1::ColorF(0xFFD400));
      rt->FillGeometry(bolt.Get(), brush);
      // 번개의 채움색은 테마와 무관하게 밝은 노랑으로 고정되어 있다. 테두리만 테마를
      // 따라가면 다크 테마에서 흰 테두리가 노란 면과 붙어 형태가 뭉개지므로, 테두리도
      // 채움색과 짝을 이루도록 검은색으로 고정한다.
      brush->SetColor(D2D1::ColorF(D2D1::ColorF::Black));
      rt->DrawGeometry(bolt.Get(), brush, stroke);
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

void DrawBatteryIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, bool dark, float level,
                     bool charging) {
  DrawBattery(rt, brush, box, dark, level, charging);
}

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

bool ClockRenderer::EnsureDcTarget() {
  if (rt_) {
    return true;
  }
  if (!d2d_) {
    return false;
  }
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
  return true;
}

void ClockRenderer::WarmTarget() {
  if (!d2d_ || rt_) {
    return;
  }
  const ULONGLONG started = GetTickCount64();
  if (!EnsureDcTarget()) {
    Log(L"bar", L"render target warm=%d %ums", 0, static_cast<unsigned>(GetTickCount64() - started));
    return;
  }

  HDC screen = GetDC(nullptr);
  HDC mem = CreateCompatibleDC(nullptr);
  HBITMAP bmp = nullptr;
  HGDIOBJ old = nullptr;
  if (mem != nullptr) {
    bmp = CreateCompatibleBitmap(screen != nullptr ? screen : mem, 8, 8);
  }
  if (mem != nullptr && bmp != nullptr) {
    old = SelectObject(mem, bmp);
    const RECT dirty{0, 0, 8, 8};
    if (SUCCEEDED(rt_->BindDC(mem, &dirty))) {
      rt_->BeginDraw();
      Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
      if (SUCCEEDED(rt_->CreateSolidColorBrush(D2D1::ColorF(0.0f, 0.0f, 0.0f, 1.0f),
                                               brush.ReleaseAndGetAddressOf()))) {
        rt_->FillRectangle(D2D1::RectF(0.0f, 0.0f, 1.0f, 1.0f), brush.Get());
      }
      rt_->EndDraw();
    }
    SelectObject(mem, old);
  }
  if (bmp != nullptr) {
    DeleteObject(bmp);
  }
  if (mem != nullptr) {
    DeleteDC(mem);
  }
  if (screen != nullptr) {
    ReleaseDC(nullptr, screen);
  }
  Log(L"bar", L"render target warm=%d %ums", rt_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
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
    case VectorIcon::kWifi:
      brush->SetColor(ClockTextColor(dark));
      DrawFluentOrFallback(brush, box, kWifiFluent, kWifiFallback);
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
    case VectorIcon::kSearch:
      brush->SetColor(ClockTextColor(dark));
      if (EnsureStroke()) {
        DrawSearchGlyph(brush, box);
      }
      break;
    case VectorIcon::kControlCenter:
      brush->SetColor(ClockTextColor(dark));
      DrawFluentOrFallback(brush, box, kCcFluent, L"\u2630");
      break;
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
    const float hover_r = (std::min)(static_cast<float>(corner::kOverlayDip),
                                     (std::max)(static_cast<float>(corner::kControlDip),
                                                (height_dip - kStartHoverInsetYDip * 2.0f) * 0.22f));
    const D2D1_ROUNDED_RECT hover{
        D2D1::RectF(hit_left + kStartHoverInsetXDip, kStartHoverInsetYDip, hit_right - kStartHoverInsetXDip,
                    height_dip - kStartHoverInsetYDip),
        hover_r, hover_r};
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

void ClockRenderer::DrawSearchGlyph(ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box) {
  if (rt_ == nullptr || brush == nullptr || !EnsureStroke()) {
    return;
  }
  const float s = (box.right - box.left) / 16.0f;
  if (s <= 0.0f) {
    return;
  }
  const float stroke = 1.50f * s;
  const D2D1_ELLIPSE ring = D2D1::Ellipse(D2D1::Point2F(box.left + 6.75f * s, box.top + 6.75f * s), 4.10f * s, 4.10f * s);
  rt_->DrawEllipse(ring, brush, stroke, round_stroke_.Get());
  rt_->DrawLine(D2D1::Point2F(box.left + 9.65f * s, box.top + 9.65f * s),
                D2D1::Point2F(box.left + 13.35f * s, box.top + 13.35f * s), brush, stroke, round_stroke_.Get());
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

  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  if (!rt_ && !EnsureDcTarget()) {
    return false;
  }
  icons_.SetDark(dark);
  QueryPerformanceCounter(&t1);
  if (timings != nullptr) {
    timings->create_ms = QpcMs(t0, t1);
  }

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
      const float icon_w = (seg.icon.vector == VectorIcon::kBattery) ? kBatteryIconDip : 16.0f;
      const float icon_h = 16.0f;
      const float icon_top = (height_dip - icon_h) * 0.5f;
      DrawVectorIcon(brush.Get(), seg.icon, D2D1::RectF(x0, icon_top, x0 + icon_w, icon_top + icon_h), dark);
      text_x = x0 + icon_w + 4.0f;
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
