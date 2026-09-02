#pragma once

#include <algorithm>
#include <d2d1.h>
#include <windows.h>
#include <wrl/client.h>

namespace bamti::corner {

// 곡률 계층. 표면이 무엇인지로 정한다. 크기로 정하지 않는다.
//
//   control  자주 보이고 밀집한 요소. WinUI 의 ControlCornerRadius 와 같다.
//   overlay  떠 있는 창과 그 안의 카드. WinUI 의 OverlayCornerRadius 와 같다.
//   hero     초점 표면. 독 알약과 Spotlight 카드뿐이다.
//            이 계층만 연속 곡률로 그린다.
//
// 세 값이 2배씩 벌어지기 때문에 동심 공식이 계층 사이를 정확히 잇는다.
//   hero(16) - 패딩 8 = overlay(8),  overlay(8) - 패딩 4 = control(4)
inline constexpr int kControlDip = 4;
inline constexpr int kOverlayDip = 8;
inline constexpr int kHeroDip = 16;

// 동심 공식의 하한. 이보다 작으면 각진 것과 구별되지 않는다.
inline constexpr int kMinInnerDip = 2;

// 슈퍼타원 지수. 4 가 애플 아이콘에 가깝고 2 면 원호와 같아진다.
inline constexpr float kSquircleExponent = 4.0f;

inline float ToPx(int dip, UINT dpi) {
  return static_cast<float>(MulDiv(dip, static_cast<int>(dpi), 96));
}

// 이 계층을 스쿼클로 그리는가. 픽셀이 아니라 계층으로 판단한다.
// 픽셀로 판단하면 같은 요소가 배율에 따라 다른 곡선으로 그려진다.
inline bool IsHero(int tier_dip) {
  return tier_dip >= kHeroDip;
}

// 컨테이너 안에 든 요소의 반지름.
// 패딩이 바깥 반지름보다 크거나 같으면 그 요소는 바깥 모서리의 영향권
// 밖이다. 그때는 동심을 맞출 이유가 없고 제 계층 값을 그대로 쓴다.
inline float ConcentricPx(float outer_px, float pad_px, float own_px, UINT dpi) {
  if (pad_px >= outer_px) {
    return own_px;
  }
  return (std::max)(ToPx(kMinInnerDip, dpi), outer_px - pad_px);
}

// 호버 강조는 상자가 아니라 행 위에 잠깐 덮는 칠이다. 컨테이너의 모서리와
// 관계가 없으므로 동심이 아니라 제 높이에 묶는다.
inline float HoverPx(float height_px, UINT dpi) {
  const float lo = ToPx(kControlDip, dpi);
  const float hi = ToPx(kOverlayDip, dpi);
  return (std::min)(hi, (std::max)(lo, height_px * 0.22f));
}

// 양 끝이 반원인 것. 토글 트랙, 슬라이더 트랙, 게이지 막대, 실행 표시 점.
inline float PillPx(float height_px) {
  return height_px * 0.5f;
}

// 반지름이 상자를 넘지 않게 자른다. 좁은 팝업에서 마주 보는 모서리가 서로
// 먹어 들어가면 모양이 뭉개진다.
inline float ClampPx(float radius_px, float width_px, float height_px) {
  const float half = (std::min)(width_px, height_px) * 0.5f - 1.0f;
  return (std::max)(0.0f, (std::min)(radius_px, half));
}

// hero 전용. 실패하면 nullptr 을 돌려준다. 호출자는 그때 원호로 물러선다.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> BuildSquircle(ID2D1Factory* factory, const D2D1_RECT_F& rect,
                                                       float radius_px);

// 같은 모양을 다시 만들지 않게 들고 있는 그릇.
// 호버나 애니메이션으로 자주 다시 그리는 표면은 반드시 이것을 거쳐야 한다.
class SquircleCache {
 public:
  ID2D1PathGeometry* Get(ID2D1Factory* factory, const D2D1_RECT_F& rect, float radius_px);
  void Reset();

 private:
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom_;
  D2D1_RECT_F rect_{};
  float radius_ = -1.0f;
};

}  // namespace bamti::corner
