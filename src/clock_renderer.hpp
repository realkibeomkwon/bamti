#pragma once

#include "bar_layout.hpp"
#include "icon_cache.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace bamti {

struct DrawTimings {
  double bind_ms = 0;
  double brush_ms = 0;
  double begin_ms = 0;
  double draw_ms = 0;
  double end_ms = 0;
};

class ClockRenderer {
 public:
  bool Initialize();
  void SetDpi(UINT dpi);
  bool Draw(HDC hdc, const RECT& client, const RECT& dirty, bool dark, const BarLayoutResult& layout,
            BarLayout* text, bool start_hot, bool start_pressed, DrawTimings* timings = nullptr);
  std::wstring CurrentTimeText() const;

 private:
  void DrawStartButton(ID2D1SolidColorBrush* brush, bool dark, bool hot, bool pressed, float height_dip,
                       const RECT& client, const RECT& start_rect);
  void DrawVectorIcon(ID2D1SolidColorBrush* brush, const StatusIcon& icon, const D2D1_RECT_F& box, bool dark);
  bool EnsureLogo(float size_dip);
  bool EnsureStroke();
  void DrawFluentOrFallback(ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, const wchar_t* fluent,
                            const wchar_t* fallback);

  void DropTarget();

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent_format_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> glyph_format_;
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> logo_geom_;
  Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> logo_brush_;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke_;
  IconCache icons_;
  UINT dpi_ = 96;
  float logo_geom_size_ = 0.0f;
  bool fluent_missing_logged_ = false;
};

}  // namespace bamti
