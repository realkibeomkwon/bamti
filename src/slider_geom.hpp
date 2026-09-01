#pragma once

#include <cmath>
#include <windows.h>

namespace bamti {

inline constexpr int kPanelSliderThumbDip = 14;

inline int SliderDipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

inline float ClampUnit(float value) {
  if (!std::isfinite(value) || value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

struct SliderGeometry {
  float lo = 0.0f;
  float hi = 0.0f;
  float thumb_r = 0.0f;
};

inline SliderGeometry SliderGeom(float left, float right, UINT dpi) {
  const float d = static_cast<float>(SliderDipToPx(kPanelSliderThumbDip, dpi));
  const float r = d * 0.5f;
  return SliderGeometry{left + r, right - r, r};
}

}  // namespace bamti
