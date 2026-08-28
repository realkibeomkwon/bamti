#pragma once

#include "bar_layout.hpp"

#include <d2d1.h>
#include <wrl/client.h>

#include <string>

namespace bamti {

class ClockRenderer {
 public:
  bool Initialize();
  void SetDpi(UINT dpi);
  bool Draw(HDC hdc, const RECT& client, bool dark, const BarLayoutResult& layout, BarLayout* text,
            bool start_hot, bool start_pressed);
  std::wstring CurrentTimeText() const;

 private:
  void DrawStartButton(ID2D1SolidColorBrush* brush, bool dark, bool hot, bool pressed, float height_dip,
                       const RECT& client, const RECT& start_rect);

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
  UINT dpi_ = 96;
};

}  // namespace bamti
