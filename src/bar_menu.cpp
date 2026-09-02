#include "bar_menu.hpp"

#include "theme.hpp"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <algorithm>

namespace bamti {
namespace {

constexpr int kMenuPadDip = 6;
constexpr int kMenuRowDip = 28;
constexpr int kMenuSepDip = 8;
constexpr int kMenuMinWidthDip = 168;
constexpr int kMenuMaxWidthDip = 520;
constexpr int kMenuTextPadDip = 12;
constexpr int kMenuCheckDip = 16;
constexpr int kMenuArrowDip = 14;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

D2D1_COLOR_F ScaleAlpha(D2D1_COLOR_F color, float mul) {
  color.a *= mul;
  return color;
}

}  // namespace

void BarMenuContent::Reset(HWND target, bool dark) {
  target_ = target;
  dark_ = dark;
  rows_.clear();
}

void BarMenuContent::Add(UINT id, std::wstring text, bool checked, bool enabled, bool submenu) {
  rows_.push_back({id, std::move(text), false, checked, enabled, submenu});
}

void BarMenuContent::AddSeparator() {
  rows_.push_back({0, L"", true});
}

void BarMenuContent::SetPopup(PopupSurface* popup) {
  popup_ = popup;
}

void BarMenuContent::SetDark(bool dark) {
  dark_ = dark;
}

bool BarMenuContent::empty() const {
  return rows_.empty();
}

int BarMenuContent::SubmenuIndex() const {
  for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
    if (rows_[static_cast<size_t>(i)].submenu) {
      return i;
    }
  }
  return -1;
}

bool BarMenuContent::RowScreenRect(int index, RECT* out) const {
  if (out == nullptr || popup_ == nullptr || popup_->hwnd() == nullptr) {
    return false;
  }
  HWND hwnd = popup_->hwnd();
  RECT client{};
  GetClientRect(hwnd, &client);
  const UINT dpi = GetDpiForWindow(hwnd);
  if (dpi == 0) {
    return false;
  }
  const RECT row = RowRect(index, dpi, client.right);
  POINT top_left{row.left, row.top};
  POINT bottom_right{row.right, row.bottom};
  ClientToScreen(hwnd, &top_left);
  ClientToScreen(hwnd, &bottom_right);
  *out = RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
  return true;
}

int BarMenuContent::RowCount() const {
  return static_cast<int>(rows_.size());
}

bool BarMenuContent::StickyRow(int index) const {
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    return false;
  }
  return rows_[static_cast<size_t>(index)].submenu;
}

SIZE BarMenuContent::Measure(UINT dpi) {
  const int pad = DipToPx(kMenuPadDip, dpi);
  const int row_h = DipToPx(kMenuRowDip, dpi);
  const int sep_h = DipToPx(kMenuSepDip, dpi);
  const int text_pad = DipToPx(kMenuTextPadDip, dpi);
  const int check_w = DipToPx(kMenuCheckDip, dpi);
  const int arrow_w = DipToPx(kMenuArrowDip, dpi);
  int text_w = 0;
  for (const BarMenuRow& row : rows_) {
    if (row.separator || row.text.empty()) {
      continue;
    }
    text_w = (std::max)(text_w, static_cast<int>(PopupTextWidth(dpi, row.text) + 0.5f));
  }
  int width = text_w + pad * 2 + text_pad * 2 + check_w + arrow_w;
  width = (std::max)(width, DipToPx(kMenuMinWidthDip, dpi));
  width = (std::min)(width, DipToPx(kMenuMaxWidthDip, dpi));
  int height = pad * 2;
  for (const BarMenuRow& row : rows_) {
    height += row.separator ? sep_h : row_h;
  }
  return SIZE{width, height};
}

void BarMenuContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr) {
    return;
  }
  const D2D1_SIZE_F sz = target->GetSize();
  const RECT client{0, 0, static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)};
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
  const D2D1_COLOR_F text_c = ClockTextColor(dark_);
  const D2D1_COLOR_F muted_c = ScaleAlpha(text_c, 0.38f);
  const D2D1_COLOR_F hover_c = MenuItemHoverFill(dark_, false);
  const D2D1_COLOR_F line_c = DockStrokeColor(dark_);
  target->CreateSolidColorBrush(D2D1::ColorF(text_c.r, text_c.g, text_c.b, 1.0f), text.GetAddressOf());
  target->CreateSolidColorBrush(muted_c, muted.GetAddressOf());
  target->CreateSolidColorBrush(hover_c, hover.GetAddressOf());
  target->CreateSolidColorBrush(D2D1::ColorF(line_c.r, line_c.g, line_c.b, line_c.a), line.GetAddressOf());
  const int text_pad = DipToPx(kMenuTextPadDip, dpi);
  const int check_w = DipToPx(kMenuCheckDip, dpi);
  const int arrow_w = DipToPx(kMenuArrowDip, dpi);
  for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
    const BarMenuRow& row = rows_[static_cast<size_t>(i)];
    const RECT rc = RowRect(i, dpi, client.right);
    if (row.separator) {
      if (!line) {
        continue;
      }
      const float y = static_cast<float>(rc.top + (rc.bottom - rc.top) / 2) + 0.5f;
      target->DrawLine(D2D1::Point2F(static_cast<float>(rc.left), y), D2D1::Point2F(static_cast<float>(rc.right), y),
                       line.Get(), 1.0f);
      continue;
    }
    if (i == hot_index && row.enabled && hover) {
      target->FillRectangle(D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                        static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                            hover.Get());
    }
    ID2D1SolidColorBrush* ink = row.enabled ? text.Get() : muted.Get();
    if (ink == nullptr) {
      continue;
    }
    if (row.checked) {
      DrawPopupText(target, dpi, L"\u2713",
                    D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                static_cast<float>(rc.left + check_w), static_cast<float>(rc.bottom)),
                    ink);
    }
    DrawPopupText(target, dpi, row.text,
                  D2D1::RectF(static_cast<float>(rc.left + check_w), static_cast<float>(rc.top),
                              static_cast<float>(rc.right - arrow_w), static_cast<float>(rc.bottom)),
                  ink);
    if (row.submenu) {
      DrawPopupText(target, dpi, L"\u203A",
                    D2D1::RectF(static_cast<float>(rc.right - arrow_w), static_cast<float>(rc.top),
                                static_cast<float>(rc.right - text_pad / 4), static_cast<float>(rc.bottom)),
                    ink);
    }
  }
}

int BarMenuContent::HitTest(POINT client, UINT dpi) const {
  int width = 0;
  if (popup_ != nullptr && popup_->hwnd() != nullptr) {
    RECT rc{};
    GetClientRect(popup_->hwnd(), &rc);
    width = rc.right;
  }
  for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
    const BarMenuRow& row = rows_[static_cast<size_t>(i)];
    if (row.separator || !row.enabled) {
      continue;
    }
    const RECT rc = RowRect(i, dpi, width);
    if (PtInRect(&rc, client)) {
      return i;
    }
  }
  return -1;
}

void BarMenuContent::Invoke(int index) {
  if (target_ == nullptr || index < 0 || index >= static_cast<int>(rows_.size())) {
    return;
  }
  const BarMenuRow& row = rows_[static_cast<size_t>(index)];
  if (row.id == 0 || !row.enabled || row.separator) {
    return;
  }
  PostMessageW(target_, WM_COMMAND, MAKEWPARAM(row.id, 0), 0);
}

RECT BarMenuContent::RowRect(int index, UINT dpi, int width) const {
  RECT result{};
  if (index < 0 || index >= static_cast<int>(rows_.size())) {
    return result;
  }
  const int pad = DipToPx(kMenuPadDip, dpi);
  const int row_h = DipToPx(kMenuRowDip, dpi);
  const int sep_h = DipToPx(kMenuSepDip, dpi);
  int y = pad;
  for (int i = 0; i < index; ++i) {
    y += rows_[static_cast<size_t>(i)].separator ? sep_h : row_h;
  }
  const int h = rows_[static_cast<size_t>(index)].separator ? sep_h : row_h;
  result = {pad, y, width - pad, y + h};
  return result;
}

}  // namespace bamti
