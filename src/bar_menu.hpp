#pragma once

#include "menu_style.hpp"
#include "popup_surface.hpp"

#include <string>
#include <vector>

namespace bamti {

class BarMenuContent : public PopupContent {
 public:
  void Reset(HWND target, bool dark);
  void Add(UINT id, std::wstring text, bool checked = false, bool enabled = true, bool submenu = false);
  void AddSeparator();
  void SetPopup(PopupSurface* popup);
  void SetDark(bool dark);
  void SetMaxWidthDip(int dip);

  bool empty() const;
  UINT SubmenuIdAt(int index) const;
  int RowIndexOfCommand(UINT id) const;
  bool RowScreenRect(int index, RECT* out) const;

  int CornerDip() const override { return kMenuCornerDip; }
  int RowCount() const override;
  bool StickyRow(int index) const override;
  bool Selectable(int index) const override;
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;

 private:
  RECT RowRect(int index, UINT dpi, int width) const;

  HWND target_ = nullptr;
  PopupSurface* popup_ = nullptr;
  bool dark_ = true;
  int max_width_dip_ = kMenuMaxWidthDip;
  std::vector<MenuRow> rows_;
};

}  // namespace bamti
