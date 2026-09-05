#include "panel_style.hpp"

#include "corner.hpp"
#include "theme.hpp"

#include <algorithm>
#include <wrl/client.h>

namespace bamti::panel {
namespace {

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

float DipToPxF(float dip, UINT dpi) {
  return dip * static_cast<float>(dpi) / 96.0f;
}

D2D1_COLOR_F ScaleAlpha(D2D1_COLOR_F color, float mul) {
  color.a *= mul;
  return color;
}

void DrawGlyphInked(ID2D1RenderTarget* target, IDWriteFactory* dwrite, IDWriteTextFormat* format, ID2D1Brush* brush,
                    const D2D1_RECT_F& box, const wchar_t* glyph) {
  if (target == nullptr || dwrite == nullptr || format == nullptr || brush == nullptr || glyph == nullptr) {
    return;
  }
  const float layout_w = box.right - box.left;
  const float layout_h = box.bottom - box.top;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(glyph, 1, format, layout_w, layout_h, layout.GetAddressOf()))) {
    return;
  }
  DWRITE_OVERHANG_METRICS oh{};
  DWRITE_TEXT_METRICS tm{};
  float dx = 0.0f;
  float dy = 0.0f;
  if (SUCCEEDED(layout->GetOverhangMetrics(&oh))) {
    const float ink_l = -oh.left;
    const float ink_t = -oh.top;
    const float ink_r = layout_w + oh.right;
    const float ink_b = layout_h + oh.bottom;
    if (ink_r > ink_l && ink_b > ink_t) {
      dx = layout_w * 0.5f - (ink_l + ink_r) * 0.5f;
      dy = layout_h * 0.5f - (ink_t + ink_b) * 0.5f;
    }
  }
  if (dx == 0.0f && dy == 0.0f && SUCCEEDED(layout->GetMetrics(&tm)) && tm.width > 0.0f && tm.height > 0.0f) {
    dx = (layout_w - tm.width) * 0.5f - tm.left;
    dy = (layout_h - tm.height) * 0.5f - tm.top;
  }
  target->DrawTextLayout(D2D1::Point2F(box.left + dx, box.top + dy), layout.Get(), brush,
                         D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
}

void DrawTrimmed(ID2D1RenderTarget* target, IDWriteFactory* dwrite, IDWriteTextFormat* format, ID2D1Brush* brush,
                 const D2D1_RECT_F& box, const wchar_t* text) {
  if (target == nullptr || dwrite == nullptr || format == nullptr || brush == nullptr || text == nullptr ||
      text[0] == L'\0') {
    return;
  }
  const float width = (std::max)(0.0f, box.right - box.left);
  const float height = (std::max)(0.0f, box.bottom - box.top);
  UINT32 n = 0;
  while (text[n] != L'\0') {
    ++n;
  }
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(text, n, format, width, height, layout.GetAddressOf()))) {
    return;
  }
  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
  if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf()))) {
    layout->SetTrimming(&trim, ellipsis.Get());
  }
  target->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace

void FillHover(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const RECT& rc, UINT dpi, bool dark) {
  if (rt == nullptr || brush == nullptr) {
    return;
  }
  brush->SetColor(MenuItemHoverFill(dark, false));
  const float hover_r = corner::HoverPx(static_cast<float>(rc.bottom - rc.top), dpi);
  const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                         static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                             hover_r, hover_r};
  rt->FillRoundedRectangle(rr, brush);
}

void DrawDivider(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark) {
  if (rt == nullptr || brush == nullptr) {
    return;
  }
  const float y = DipToPxF(static_cast<float>(y_dip), dpi);
  const float h = DipToPxF(static_cast<float>(kDivHDip), dpi);
  const int inset = DipToPx(kInsetDip, dpi);
  const int width = DipToPx(kWidthDip, dpi);
  brush->SetColor(ScaleAlpha(ClockTextColor(dark), 0.10f));
  rt->FillRectangle(D2D1::RectF(static_cast<float>(inset), y, static_cast<float>(width - inset), y + h), brush);
}

void DrawToggle(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const RECT& rc, UINT dpi, bool on, bool enabled,
                bool dark) {
  if (rt == nullptr || brush == nullptr) {
    return;
  }
  const float th = static_cast<float>(rc.bottom - rc.top);
  const float toggle_pill = corner::PillPx(th);
  D2D1_COLOR_F track = on ? AccentFillColor(dark) : BadgeOffFill(dark);
  D2D1_COLOR_F knob_color = AccentOnColor(dark);
  if (!enabled) {
    track = ScaleAlpha(track, 0.40f);
    knob_color = ScaleAlpha(knob_color, 0.40f);
  }
  brush->SetColor(track);
  rt->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                                         static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                                             toggle_pill, toggle_pill},
                           brush);
  const float knob = DipToPxF(kKnobDip, dpi);
  const float knob_inset = DipToPxF(kKnobInsetDip, dpi);
  const float knob_x = on ? static_cast<float>(rc.right) - knob_inset - knob : static_cast<float>(rc.left) + knob_inset;
  brush->SetColor(knob_color);
  rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x + knob * 0.5f, static_cast<float>(rc.top) + th * 0.5f),
                               knob * 0.5f, knob * 0.5f),
                  brush);
}

void DrawRowCircle(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fluent,
                   ID2D1SolidColorBrush* brush, float cx, float cy, UINT dpi, bool dark, bool active,
                   const wchar_t* glyph) {
  if (rt == nullptr || brush == nullptr) {
    return;
  }
  const float circle = DipToPxF(static_cast<float>(kCircleDip), dpi);
  const float glyph_sz = circle * 0.55f;
  brush->SetColor(active ? AccentFillColor(dark) : BadgeOffFill(dark));
  rt->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), circle * 0.5f, circle * 0.5f), brush);
  brush->SetColor(active ? AccentOnColor(dark) : ClockTextColor(dark));
  if (dwrite != nullptr && fluent != nullptr && glyph != nullptr) {
    DrawGlyphInked(rt, dwrite, fluent, brush,
                   D2D1::RectF(cx - glyph_sz * 0.5f, cy - glyph_sz * 0.5f, cx + glyph_sz * 0.5f, cy + glyph_sz * 0.5f),
                   glyph);
  }
}

void DrawSectionHeader(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fmt,
                       ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, const wchar_t* title) {
  if (rt == nullptr || brush == nullptr || title == nullptr) {
    return;
  }
  const int inset = DipToPx(kInsetDip, dpi);
  const int width = DipToPx(kWidthDip, dpi);
  brush->SetColor(ScaleAlpha(ClockTextColor(dark), 0.55f));
  DrawTrimmed(rt, dwrite, fmt, brush,
              D2D1::RectF(static_cast<float>(inset), static_cast<float>(DipToPx(y_dip, dpi)),
                          static_cast<float>(width - inset), static_cast<float>(DipToPx(y_dip + kSectionHDip, dpi))),
              title);
}

void DrawSettingsRow(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fmt,
                     ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, bool hot, const wchar_t* label) {
  if (rt == nullptr || brush == nullptr || label == nullptr) {
    return;
  }
  const int inset = DipToPx(kInsetDip, dpi);
  const int width = DipToPx(kWidthDip, dpi);
  const RECT row{inset, DipToPx(y_dip, dpi), width - inset, DipToPx(y_dip + kSettingsHDip, dpi)};
  if (hot) {
    FillHover(rt, brush, row, dpi, dark);
  }
  brush->SetColor(ClockTextColor(dark));
  DrawTrimmed(rt, dwrite, fmt, brush,
              D2D1::RectF(static_cast<float>(row.left), static_cast<float>(row.top), static_cast<float>(row.right),
                          static_cast<float>(row.bottom)),
              label);
}

}  // namespace bamti::panel
