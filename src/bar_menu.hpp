#pragma once

#include "popup_surface.hpp"

#include <string>
#include <vector>

namespace bamti {

struct BarMenuRow {
  UINT id = 0;
  std::wstring text;
  bool separator = false;
  bool checked = false;
  bool enabled = true;
  bool submenu = false;
};

class BarMenuContent : public PopupContent {
 public:
  void Reset(HWND target, bool dark);
  void Add(UINT id, std::wstring text, bool checked = false, bool enabled = true, bool submenu = false);
  void AddSeparator();
  void SetPopup(PopupSurface* popup);
  void SetDark(bool dark);

  bool empty() const;
  int SubmenuIndex() const;
  bool RowScreenRect(int index, RECT* out) const;

  int RowCount() const override;
  bool StickyRow(int index) const override;
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;

 private:
  RECT RowRect(int index, UINT dpi, int width) const;

  HWND target_ = nullptr;
  PopupSurface* popup_ = nullptr;
  bool dark_ = true;
  std::vector<BarMenuRow> rows_;
};

}  // namespace bamti
