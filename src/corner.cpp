#include "corner.hpp"

#include <cmath>
#include <d2d1helper.h>

namespace bamti::corner {
namespace {

constexpr float kHalfPi = 1.5707963f;

// 모서리 하나를 몇 조각으로 쪼갤지. 반지름이 클수록 늘린다.
int SegmentsFor(float radius_px) {
  const int n = static_cast<int>(std::lround(radius_px * 0.5f));
  return (std::min)(24, (std::max)(8, n));
}

// 슈퍼타원 사분면 위의 점. t 는 0 에서 pi/2 까지.
//   x = R * cos(t)^(2/n),  y = R * sin(t)^(2/n)
D2D1_POINT_2F SuperPoint(float radius_px, float t) {
  const float e = 2.0f / kSquircleExponent;
  const float x = radius_px * std::pow((std::max)(0.0f, std::cos(t)), e);
  const float y = radius_px * std::pow((std::max)(0.0f, std::sin(t)), e);
  return D2D1::Point2F(x, y);
}

}  // namespace

Microsoft::WRL::ComPtr<ID2D1PathGeometry> BuildSquircle(ID2D1Factory* factory, const D2D1_RECT_F& rect,
                                                       float radius_px) {
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom;
  const float w = rect.right - rect.left;
  const float h = rect.bottom - rect.top;
  if (factory == nullptr || w <= 0.0f || h <= 0.0f) {
    return geom;
  }
  const float r = (std::max)(0.0f, (std::min)(radius_px, (std::min)(w, h) * 0.5f));
  if (r < 0.5f) {
    return geom;
  }
  if (FAILED(factory->CreatePathGeometry(geom.GetAddressOf())) || !geom) {
    return {};
  }
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geom->Open(sink.GetAddressOf())) || !sink) {
    return {};
  }

  const int n = SegmentsFor(r);
  // 모서리의 안쪽 중심에서 부호만큼 뻗어 나간다. reverse 는 진행 방향이
  // 사분면을 거꾸로 훑어야 하는 모서리에서 켠다.
  auto corner = [&](float cx, float cy, float sx, float sy, bool reverse) {
    for (int i = 1; i <= n; ++i) {
      const float u = static_cast<float>(i) / static_cast<float>(n);
      const float t = (reverse ? 1.0f - u : u) * kHalfPi;
      const D2D1_POINT_2F p = SuperPoint(r, t);
      sink->AddLine(D2D1::Point2F(cx + sx * p.x, cy + sy * p.y));
    }
  };

  const float l = rect.left;
  const float t = rect.top;
  const float rt = rect.right;
  const float b = rect.bottom;

  sink->BeginFigure(D2D1::Point2F(l + r, t), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(D2D1::Point2F(rt - r, t));
  corner(rt - r, t + r, 1.0f, -1.0f, true);
  sink->AddLine(D2D1::Point2F(rt, b - r));
  corner(rt - r, b - r, 1.0f, 1.0f, false);
  sink->AddLine(D2D1::Point2F(l + r, b));
  corner(l + r, b - r, -1.0f, 1.0f, true);
  sink->AddLine(D2D1::Point2F(l, t + r));
  corner(l + r, t + r, -1.0f, -1.0f, false);
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);

  if (FAILED(sink->Close())) {
    return {};
  }
  return geom;
}

ID2D1PathGeometry* SquircleCache::Get(ID2D1Factory* factory, const D2D1_RECT_F& rect, float radius_px) {
  auto same = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
  if (geom_ && same(radius_, radius_px) && same(rect_.left, rect.left) && same(rect_.top, rect.top) &&
      same(rect_.right, rect.right) && same(rect_.bottom, rect.bottom)) {
    return geom_.Get();
  }
  geom_ = BuildSquircle(factory, rect, radius_px);
  rect_ = rect;
  radius_ = radius_px;
  return geom_.Get();
}

void SquircleCache::Reset() {
  geom_.Reset();
  radius_ = -1.0f;
}

}  // namespace bamti::corner
