#pragma once

#include <d2d1.h>

#include <string>
#include <vector>

namespace bamti {

struct MenuRow {
  UINT id = 0;
  std::wstring text;
  bool separator = false;
  bool checked = false;
  bool submenu = false;
  bool enabled = true;
};

struct MenuMetrics {
  int pad = 0;
  int row_h = 0;
  int sep_h = 0;
  int check_w = 0;
  int arrow_w = 0;
};

inline constexpr int kMenuPadDip = 6;
inline constexpr int kMenuCornerDip = 10;
inline constexpr int kMenuMaxWidthDip = 280;

MenuMetrics MakeMenuMetrics(UINT dpi);
SIZE MeasureMenuRows(const std::vector<MenuRow>& rows, UINT dpi, int max_width_dip = kMenuMaxWidthDip);
RECT MenuRowRectOf(const std::vector<MenuRow>& rows, int index, UINT dpi, int width);
int MenuHitTest(const std::vector<MenuRow>& rows, POINT client, UINT dpi, int width);
void RenderMenuRows(ID2D1RenderTarget* target, UINT dpi, int hot, bool dark, const std::vector<MenuRow>& rows);

}  // namespace bamti
