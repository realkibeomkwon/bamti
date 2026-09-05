#include "bar_menu.hpp"

namespace bamti {

void BarMenuContent::Reset(HWND target, bool dark) {
  target_ = target;
  dark_ = dark;
  rows_.clear();
}

void BarMenuContent::Add(UINT id, std::wstring text, bool checked, bool enabled, bool submenu) {
  rows_.push_back({id, std::move(text), false, checked, submenu, enabled});
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

void BarMenuContent::SetMaxWidthDip(int dip) {
  max_width_dip_ = dip;
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
  return MeasureMenuRows(rows_, dpi, max_width_dip_);
}

void BarMenuContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  RenderMenuRows(target, dpi, hot_index, dark_, rows_);
}

int BarMenuContent::HitTest(POINT client, UINT dpi) const {
  int width = 0;
  if (popup_ != nullptr && popup_->hwnd() != nullptr) {
    RECT rc{};
    GetClientRect(popup_->hwnd(), &rc);
    width = rc.right;
  }
  return MenuHitTest(rows_, client, dpi, width);
}

void BarMenuContent::Invoke(int index) {
  if (target_ == nullptr || index < 0 || index >= static_cast<int>(rows_.size())) {
    return;
  }
  const MenuRow& row = rows_[static_cast<size_t>(index)];
  if (row.id == 0 || !row.enabled || row.separator) {
    return;
  }
  PostMessageW(target_, WM_COMMAND, MAKEWPARAM(row.id, 0), 0);
}

RECT BarMenuContent::RowRect(int index, UINT dpi, int width) const {
  return MenuRowRectOf(rows_, index, dpi, width);
}

}  // namespace bamti
