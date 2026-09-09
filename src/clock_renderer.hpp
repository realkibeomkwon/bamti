#pragma once

#include "bar_layout.hpp"
#include "icon_cache.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>

namespace bamti {

struct DrawTimings {
  double create_ms = 0;  // 렌더 타깃 생성. rt_가 비어 있던 프레임에만 0이 아니다.
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
  // 첫 그리기 전에 렌더 타깃과 D2D 장치를 미리 구성한다.
  // 그리지 않으면 장치 구성이 EndDraw까지 미뤄지므로, 작은 메모리 DC에
  // 실제로 한 번 그려서 비용을 여기서 치른다.
  void WarmTarget();
  bool Draw(HDC hdc, const RECT& client, const RECT& dirty, bool dark, const BarLayoutResult& layout,
            BarLayout* text, bool start_hot, bool start_pressed, DrawTimings* timings = nullptr);
  std::wstring CurrentTimeText() const;

 private:
  void DrawStartButton(ID2D1SolidColorBrush* brush, bool dark, bool hot, bool pressed, float height_dip,
                       const RECT& client, const RECT& start_rect);
  void DrawSearchGlyph(ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box);
  void DrawVectorIcon(ID2D1SolidColorBrush* brush, const StatusIcon& icon, const D2D1_RECT_F& box, bool dark);
  bool EnsureLogo(float size_dip);
  bool EnsureStroke();
  void DrawFluentOrFallback(ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, const wchar_t* fluent,
                            const wchar_t* fallback);

  bool EnsureDcTarget();
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

void DrawBatteryIcon(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box, bool dark, float level,
                     bool charging);
void DrawWifiIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush, ID2D1StrokeStyle* stroke,
                  const D2D1_RECT_F& box, D2D1_COLOR_F color, int level);

}  // namespace bamti
