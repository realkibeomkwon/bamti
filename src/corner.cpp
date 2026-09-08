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

Microsoft::WRL::ComPtr<ID2D1PathGeometry> BuildCallout(ID2D1Factory* factory, const D2D1_RECT_F& card,
                                                       float radius_px, const Tail& tail) {
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom;
  const float w = card.right - card.left;
  const float h = card.bottom - card.top;
  if (factory == nullptr || w <= 0.0f || h <= 0.0f || tail.height_px <= 0.0f || tail.base_px <= 0.0f) {
    return geom;
  }
  const float r = (std::max)(0.0f, (std::min)(radius_px, (std::min)(w, h) * 0.5f));
  // 밑변이 둥근 모서리를 먹지 않게 자른다.
  const float base = (std::min)(tail.base_px, (std::max)(0.0f, w - r * 2.0f - 4.0f));
  if (base <= 1.0f) {
    return geom;
  }
  const float half = base * 0.5f;
  const float lo = card.left + r + half + 1.0f;
  const float hi = card.right - r - half - 1.0f;
  const float apex_x = hi < lo ? (card.left + card.right) * 0.5f : (std::min)(hi, (std::max)(lo, tail.apex_x));
  const float apex_y = card.bottom + tail.height_px;

  if (FAILED(factory->CreatePathGeometry(geom.GetAddressOf())) || !geom) {
    return {};
  }
  Microsoft::WRL::ComPtr<ID2D1GeometrySink> sink;
  if (FAILED(geom->Open(sink.GetAddressOf())) || !sink) {
    return {};
  }
  auto arc = [&](float x, float y) {
    sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x, y), D2D1::SizeF(r, r), 0.0f,
                                  D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
  };
  // 꼭짓점에서 두 빗변을 tip 만큼 물러난 지점을 잇는 이차 곡선으로 둥글린다.
  const float len = std::sqrt(half * half + tail.height_px * tail.height_px);
  const float tip = (std::min)(tail.tip_px, len * 0.5f);
  const float ux = half / len;
  const float uy = tail.height_px / len;

  sink->BeginFigure(D2D1::Point2F(card.left + r, card.top), D2D1_FIGURE_BEGIN_FILLED);
  sink->AddLine(D2D1::Point2F(card.right - r, card.top));
  arc(card.right, card.top + r);
  sink->AddLine(D2D1::Point2F(card.right, card.bottom - r));
  arc(card.right - r, card.bottom);
  sink->AddLine(D2D1::Point2F(apex_x + half, card.bottom));
  sink->AddLine(D2D1::Point2F(apex_x + tip * ux, apex_y - tip * uy));
  sink->AddQuadraticBezier(D2D1::QuadraticBezierSegment(D2D1::Point2F(apex_x, apex_y),
                                                        D2D1::Point2F(apex_x - tip * ux, apex_y - tip * uy)));
  sink->AddLine(D2D1::Point2F(apex_x - half, card.bottom));
  sink->AddLine(D2D1::Point2F(card.left + r, card.bottom));
  arc(card.left, card.bottom - r);
  sink->AddLine(D2D1::Point2F(card.left, card.top + r));
  arc(card.left + r, card.top);
  sink->EndFigure(D2D1_FIGURE_END_CLOSED);

  if (FAILED(sink->Close())) {
    return {};
  }
  return geom;
}

ID2D1PathGeometry* CalloutCache::Get(ID2D1Factory* factory, const D2D1_RECT_F& card, float radius_px, const Tail& tail) {
  auto same = [](float a, float b) { return std::fabs(a - b) < 0.01f; };
  if (geom_ && same(radius_, radius_px) && same(card_.left, card.left) && same(card_.top, card.top) &&
      same(card_.right, card.right) && same(card_.bottom, card.bottom) && same(tail_.apex_x, tail.apex_x) &&
      same(tail_.base_px, tail.base_px) && same(tail_.height_px, tail.height_px)) {
    return geom_.Get();
  }
  geom_ = BuildCallout(factory, card, radius_px, tail);
  card_ = card;
  radius_ = radius_px;
  tail_ = tail;
  return geom_.Get();
}

void CalloutCache::Reset() {
  geom_.Reset();
  radius_ = -1.0f;
}

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
