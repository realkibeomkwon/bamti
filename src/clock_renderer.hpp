#pragma once

#include "status_item.hpp"

#include <d2d1.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace bamti {

class ClockRenderer {
 public:
  bool Initialize();
  void SetDpi(UINT dpi);
  bool Draw(HDC hdc, const RECT& client, bool dark, const std::wstring& left_label,
            const std::vector<StatusItem>& items, std::vector<StatusHit>* hits, RECT* start_hit,
            bool start_hot, bool start_pressed);

 private:
  bool EnsureTextFormat();
  void DrawStartButton(ID2D1SolidColorBrush* brush, bool dark, bool hot, bool pressed, float height_dip);
  std::wstring CurrentTimeText() const;
  bool MakeLayout(const std::wstring& text, float width_dip, float height_dip,
                  Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout, DWRITE_TEXT_METRICS& metrics);

  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt_;
  UINT dpi_ = 96;
};

}  // namespace bamti
