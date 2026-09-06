#include "menu_style.hpp"

#include "corner.hpp"
#include "popup_surface.hpp"
#include "theme.hpp"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace bamti {
namespace {

// 맥 독 우클릭 메뉴는 일반 NSMenu(22)보다 행이 높다. 13pt 글자에 위아래
// 여유를 두면 행 30, 바깥 6, 구분 11 이 스크린샷과 맞는다.
constexpr int kMenuRowDip = 30;
constexpr int kMenuSepDip = 11;
constexpr int kMenuMinWidthDip = 160;
constexpr int kMenuCheckDip = 18;
constexpr int kMenuArrowDip = 16;
constexpr int kMenuHoverRadiusDip = 6;
// 강조 칠이 행을 꽉 채우지 않게 위아래로 물러나는 양.
// 맥 메뉴는 강조가 행의 70% 정도만 차지한다.
constexpr int kMenuHoverInsetDip = 4;
// 재는 폭과 그리는 폭이 정확히 같으면 반올림이 불리하게 떨어질 때
// 마지막 글자가 생략 부호로 바뀐다. 여유 2 DIP 를 준다.
constexpr int kMenuTextSlackDip = 2;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

D2D1_COLOR_F ScaleAlpha(D2D1_COLOR_F color, float mul) {
  color.a *= mul;
  return color;
}

void DrawDockMenuStroke(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* style,
                        const D2D1_POINT_2F* pts, UINT count, float stroke_px) {
  if (target == nullptr || brush == nullptr || pts == nullptr || count < 2) {
    return;
  }
  for (UINT i = 1; i < count; ++i) {
    target->DrawLine(pts[i - 1], pts[i], brush, stroke_px, style);
  }
}

void DrawDockMenuCheck(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* style,
                       const D2D1_RECT_F& col, UINT dpi) {
  const float s = static_cast<float>(dpi) / 96.0f;
  const float cx = (col.left + col.right) * 0.5f;
  const float cy = (col.top + col.bottom) * 0.5f;
  const D2D1_POINT_2F pts[] = {
      D2D1::Point2F(cx - 4.2f * s, cy + 0.3f * s),
      D2D1::Point2F(cx - 1.1f * s, cy + 3.2f * s),
      D2D1::Point2F(cx + 4.6f * s, cy - 3.5f * s),
  };
  DrawDockMenuStroke(target, brush, style, pts, 3, 1.7f * s);
}

void DrawDockMenuChevron(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* style,
                         const D2D1_RECT_F& col, UINT dpi) {
  const float s = static_cast<float>(dpi) / 96.0f;
  const float cx = col.right - 6.5f * s;
  const float cy = (col.top + col.bottom) * 0.5f;
  const D2D1_POINT_2F pts[] = {
      D2D1::Point2F(cx - 2.4f * s, cy - 4.0f * s),
      D2D1::Point2F(cx + 1.8f * s, cy),
      D2D1::Point2F(cx - 2.4f * s, cy + 4.0f * s),
  };
  DrawDockMenuStroke(target, brush, style, pts, 3, 1.5f * s);
}

}  // namespace

MenuMetrics MakeMenuMetrics(UINT dpi) {
  return MenuMetrics{
      DipToPx(kMenuPadDip, dpi),
      DipToPx(kMenuRowDip, dpi),
      DipToPx(kMenuSepDip, dpi),
      DipToPx(kMenuCheckDip, dpi),
      DipToPx(kMenuArrowDip, dpi),
  };
}

SIZE MeasureMenuRows(const std::vector<MenuRow>& rows, UINT dpi, int max_width_dip) {
  const MenuMetrics m = MakeMenuMetrics(dpi);
  int text_w = 0;
  for (const MenuRow& row : rows) {
    if (row.separator || row.text.empty()) {
      continue;
    }
    text_w = (std::max)(text_w, static_cast<int>(std::ceil(PopupTextWidth(dpi, row.text))));
  }
  int width = text_w + m.pad * 2 + m.check_w + m.arrow_w;
  width += DipToPx(kMenuTextSlackDip, dpi);
  width = (std::max)(width, DipToPx(kMenuMinWidthDip, dpi));
  width = (std::min)(width, DipToPx(max_width_dip, dpi));
  int height = m.pad * 2;
  for (const MenuRow& row : rows) {
    height += row.separator ? m.sep_h : m.row_h;
  }
  return SIZE{width, height};
}

RECT MenuRowRectOf(const std::vector<MenuRow>& rows, int index, UINT dpi, int width) {
  RECT result{};
  if (index < 0 || index >= static_cast<int>(rows.size())) {
    return result;
  }
  const MenuMetrics m = MakeMenuMetrics(dpi);
  int y = m.pad;
  for (int i = 0; i < index; ++i) {
    y += rows[static_cast<size_t>(i)].separator ? m.sep_h : m.row_h;
  }
  const int h = rows[static_cast<size_t>(index)].separator ? m.sep_h : m.row_h;
  result = {m.pad, y, width - m.pad, y + h};
  return result;
}

int MenuHitTest(const std::vector<MenuRow>& rows, POINT client, UINT dpi, int width) {
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    const MenuRow& row = rows[static_cast<size_t>(i)];
    if (row.separator || !row.enabled) {
      continue;
    }
    const RECT rc = MenuRowRectOf(rows, i, dpi, width);
    if (PtInRect(&rc, client)) {
      return i;
    }
  }
  return -1;
}

void RenderMenuRows(ID2D1RenderTarget* target, UINT dpi, int hot, bool dark, const std::vector<MenuRow>& rows) {
  if (target == nullptr) {
    return;
  }
  const D2D1_SIZE_F sz = target->GetSize();
  const int width = static_cast<int>(sz.width);
  const MenuMetrics m = MakeMenuMetrics(dpi);
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> ink;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
  const D2D1_COLOR_F text_c = ClockTextColor(dark);
  const D2D1_COLOR_F muted_c = ScaleAlpha(text_c, 0.38f);
  const D2D1_COLOR_F hot_text_c = AccentOnColor(dark);
  const D2D1_COLOR_F hover_c = AccentFillColor(dark);
  const D2D1_COLOR_F line_c = DockStrokeColor(dark);
  target->CreateSolidColorBrush(text_c, ink.GetAddressOf());
  target->CreateSolidColorBrush(hover_c, hover.GetAddressOf());
  target->CreateSolidColorBrush(D2D1::ColorF(line_c.r, line_c.g, line_c.b, line_c.a), line.GetAddressOf());
  ID2D1Factory* factory = nullptr;
  target->GetFactory(&factory);
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke_style;
  if (factory != nullptr) {
    factory->CreateStrokeStyle(
        D2D1::StrokeStyleProperties(D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND, D2D1_CAP_STYLE_ROUND,
                                    D2D1_LINE_JOIN_ROUND, 2.0f),
        nullptr, 0, stroke_style.GetAddressOf());
  }
  const float hover_r = corner::ToPx(kMenuHoverRadiusDip, dpi);
  for (int i = 0; i < static_cast<int>(rows.size()); ++i) {
    const MenuRow& row = rows[static_cast<size_t>(i)];
    const RECT rc = MenuRowRectOf(rows, i, dpi, width);
    const D2D1_RECT_F box = D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                        static_cast<float>(rc.right), static_cast<float>(rc.bottom));
    if (row.separator) {
      if (!line) {
        continue;
      }
      const float y = static_cast<float>(rc.top + (rc.bottom - rc.top) / 2) + 0.5f;
      target->DrawLine(D2D1::Point2F(box.left, y), D2D1::Point2F(box.right, y), line.Get(), 1.0f);
      continue;
    }
    const bool selected = i == hot && row.enabled;
    if (selected && hover) {
      const float inset = corner::ToPx(kMenuHoverInsetDip, dpi);
      const D2D1_RECT_F fill_box{box.left, box.top + inset, box.right, box.bottom - inset};
      const float rr = corner::ClampPx(hover_r, fill_box.right - fill_box.left, fill_box.bottom - fill_box.top);
      target->FillRoundedRectangle(D2D1_ROUNDED_RECT{fill_box, rr, rr}, hover.Get());
    }
    if (!ink) {
      continue;
    }
    ink->SetColor(!row.enabled ? muted_c : (selected ? hot_text_c : text_c));
    if (row.checked) {
      DrawDockMenuCheck(target, ink.Get(), stroke_style.Get(),
                        D2D1::RectF(box.left, box.top, box.left + static_cast<float>(m.check_w), box.bottom), dpi);
    }
    DrawPopupText(target, dpi, row.text,
                  D2D1::RectF(box.left + static_cast<float>(m.check_w), box.top,
                              box.right - static_cast<float>(m.arrow_w), box.bottom),
                  ink.Get());
    if (row.submenu) {
      DrawDockMenuChevron(target, ink.Get(), stroke_style.Get(),
                          D2D1::RectF(box.right - static_cast<float>(m.arrow_w), box.top, box.right, box.bottom), dpi);
    }
  }
}

}  // namespace bamti
