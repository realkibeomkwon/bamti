# 작업 지시서: 곡률 규칙을 새로 정의하고 모든 표면에 적용한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

지금 코드에 흩어져 있는 곡률 상수는 기준 없이 하나씩 정해진 값입니다. 이 지시서는 그 값들을 이어받지 않고 규칙을 처음부터 새로 세운 뒤, 전 표면을 그 규칙 위에 다시 올립니다.

새로 만드는 파일은 `src/corner.hpp`, `src/corner.cpp`입니다.

고치는 파일은 `CMakeLists.txt`, `src/theme.hpp`, `src/popup_surface.hpp`, `src/popup_surface.cpp`, `src/dock.cpp`, `src/spotlight.cpp`, `src/control_center.cpp`, `src/clock_flyout.cpp`, `src/clock_renderer.cpp`, `src/start_menu.cpp`, `src/status_panel.cpp`입니다.

`bamti.vcxproj`는 건드리지 마십시오. 이미 `clock_flyout.cpp`, `control_center.cpp`, `status_panel.cpp` 등 열다섯 개가 넘는 소스를 담고 있지 않아서 실제 빌드에 쓰이지 않습니다. 빌드 목록은 `CMakeLists.txt`가 단독으로 관리합니다.

---

## 1. 지금 상태

곡률을 정하는 자리가 여덟 군데로 흩어져 있고 값이 서로를 참조하지 않습니다.

| 값 | 정의 위치 | 쓰이는 곳 |
|---|---|---|
| 4 DIP | `src/theme.hpp:22` `kCornerRadiusDip` | 독 알약, 모든 팝업 창, 시작 단추 호버 |
| 8 DIP | `src/clock_flyout.cpp:16` `kCardRadiusDip` | 알림 카드, 달력 카드 |
| 4 DIP | `src/clock_flyout.cpp:524`, `:826` 리터럴 | 플라이아웃 행 호버 |
| 18 DIP | `src/spotlight.cpp:34` `kCardRadiusDip` | Spotlight 카드와 그림자 |
| 12 DIP | `src/spotlight.cpp:35` `kSearchRadiusDip` | Spotlight 검색창 |
| 8 DIP | `src/spotlight.cpp:2459` 리터럴 | Spotlight 행 호버 |
| 8 DIP | `src/dock.cpp:2067` 리터럴 | 독 아이콘 호버 |
| 6 DIP | `src/start_menu.cpp:368` 리터럴 | 시작 메뉴 행 호버 |

같은 성격의 요소가 4, 6, 8을 제각각 쓰고 있습니다. 특히 행 호버 하나를 네 파일이 서로 다른 값으로 그립니다. 그리고 `kCornerRadiusDip` 하나가 높이 64 DIP의 독 알약과 높이 28 DIP의 메뉴 항목 호버를 동시에 담당하고 있어서, 어느 한쪽에 맞추면 다른 쪽이 틀어집니다.

---

## 2. 새 규칙

### 2-1. 계층은 크기가 아니라 표면의 성격으로 정한다

세 계층만 둡니다. 값은 2배씩 벌어집니다.

| 계층 | 반지름 | 곡선 | 성격 |
|---|---|---|---|
| control | 4 DIP | 원호 | 자주 보이고 밀집한 요소. 항목 호버의 하한, 배지, 칩, 입력 보조. |
| overlay | 8 DIP | 원호 | 떠 있는 창과 그 안의 카드. |
| hero | 16 DIP | **스쿼클** | 초점 표면. 독 알약과 Spotlight 카드. |

알약은 계층이 아니라 공식입니다. 토글 트랙, 슬라이더 트랙, 게이지 막대, 실행 표시 점은 언제나 제 높이의 절반을 씁니다.

control과 overlay 값은 WinUI의 `ControlCornerRadius`(4)와 `OverlayCornerRadius`(8)에 그대로 맞춘 것입니다. 사용자가 매일 보는 표면이 Windows 규격 위에 남으므로 이질감이 생길 자리가 없습니다. 지금 팝업 창이 4를 쓰고 있으니 이 작업은 팝업을 오히려 Windows 11 표준에 가깝게 올립니다.

### 2-2. hero를 두 표면에만 주는 이유

독은 Windows에 존재하지 않는 물건이라 이미 태생적으로 이질적입니다. 여기서 큰 곡률을 써도 "Windows답지 않다"는 손실이 새로 생기지 않습니다. Spotlight 카드는 화면 한가운데에 단독으로 뜨는 초점 표면이라 같은 이유가 적용됩니다.

반대로 팝업 창은 상단바에서 자라나는 부속이고 Windows 메뉴와 나란히 놓입니다. 이쪽까지 hero로 올리면 이질감이 바로 드러납니다. 제어 센터는 성격상 hero 후보이지만 이번에는 overlay로 둡니다. 승격이 필요해지면 2-6의 `CornerDip()` 한 줄만 덮어쓰면 되도록 통로를 열어 둡니다.

### 2-3. 계층 토큰은 컨테이너에만 쓰고, 안에 든 것은 공식으로 유도한다

계층 값을 직접 적는 자리는 창과 최상위 카드뿐입니다. 그 안에 든 요소는 다음 공식으로 정합니다.

```text
안쪽 반지름 = 바깥 반지름 - 패딩
```

다만 조건이 하나 붙습니다. **패딩이 바깥 반지름보다 크거나 같으면 그 요소는 바깥 모서리의 곡률 영향권 밖입니다.** 그때는 동심을 맞출 이유가 없고 제 계층 값을 그대로 씁니다. 이 조건이 없으면 패딩이 큰 패널에서 안쪽 카드가 억지로 각져 버립니다.

### 2-4. 호버 강조는 동심이 아니라 제 높이에 묶는다

호버는 상자가 아니라 행 위에 잠깐 덮는 칠입니다. 컨테이너의 모서리와 아무 관계가 없으므로 동심 공식을 쓰면 안 됩니다. 제 높이의 22%를 쓰되 control과 overlay 사이를 벗어나지 않게 자릅니다.

이 공식을 넣으면 지금 흩어진 4, 6, 8이 각 행 높이에서 자동으로 유도됩니다. 값이 비슷하게 나오더라도 근거가 생기는 것이 이번 작업의 핵심입니다.

### 2-5. 스쿼클은 hero에만 쓴다

스쿼클과 원호의 최대 편차는 반지름에 비례합니다. 반지름이 4 DIP면 96 DPI에서 4 px이고 두 곡선의 차이는 1 px에 못 미쳐서 안티에일리어싱에 묻힙니다. 낮은 곡률에 스쿼클을 쓰면 지오메트리 생성 비용만 치르고 화면에는 아무 변화가 없습니다.

그래서 판단은 픽셀이 아니라 **계층으로** 합니다. 픽셀 기준으로 판단하면 배율에 따라 같은 요소가 어떤 모니터에서는 스쿼클로, 어떤 모니터에서는 원호로 그려져서 재현되지 않는 차이가 생깁니다.

### 2-6. 팝업 창은 콘텐츠가 제 계층을 말한다

`PopupSurface` 하나가 독 우클릭 메뉴, 트레이 메뉴, 상태 항목 패널, 제어 센터, 시계 플라이아웃을 모두 담습니다. 창이 계층을 정하면 안 되고 콘텐츠가 정해야 합니다. `PaintsOwnChrome()` 옆에 같은 모양의 가상 함수를 하나 더 둡니다.

---

## 3. `src/corner.hpp`를 새로 만든다

`src/slider_geom.hpp`의 서술 방식을 따릅니다.

```cpp
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
```

---

## 4. `src/corner.cpp`를 새로 만든다

모서리를 슈퍼타원 사분면으로 만들고 직선 조각으로 이어 붙입니다. 조각 수는 반지름에 따라 늘립니다. 반지름 16 DIP를 150% 배율에서 그리면 24 px이고 조각은 12개가 되는데, 조각 하나가 덮는 활의 처짐은 0.1 px에 못 미쳐서 안티에일리어싱 아래로 들어갑니다.

```cpp
#include "corner.hpp"

#include <cmath>

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
```

네 모서리의 시작점과 끝점이 서로 맞물리는지 손으로 한 번 짚어 보십시오. 오른쪽 위 모서리는 `(rt - r, t)`에서 시작해 `(rt, t + r)`에서 끝나야 하고, 그 끝점이 다음 직선의 시작점과 같아야 합니다. 한 군데라도 어긋나면 도형에 실금이 보입니다.

`CMakeLists.txt`의 `add_executable` 목록에 `src/corner.cpp`를 넣으십시오. `src/theme.cpp` 다음 줄이 적당합니다.

---

## 5. 표면별 적용

### 5-1. `src/theme.hpp` — 옛 상수를 지운다

`kCornerRadiusDip`과 그 위의 주석 두 줄을 삭제하십시오. 이 상수를 남겨 두면 새 규칙과 옛 규칙이 공존하게 되고, 이번 작업이 끝난 뒤 누군가 다시 이 값을 집어 쓰게 됩니다. 지우면 참조하던 자리가 전부 컴파일 오류로 드러나므로 빠뜨린 곳을 찾는 목록으로도 쓸 수 있습니다.

### 5-2. `src/popup_surface.hpp` — 콘텐츠가 계층을 말하게 한다

`PaintsOwnChrome()` 바로 아래에 같은 모양으로 하나 더 둡니다.

```cpp
  // 이 콘텐츠를 담는 창의 곡률 계층. 기본은 떠 있는 창이다.
  // 초점 표면으로 올리려면 corner::kHeroDip 을 돌려주면 된다.
  virtual int CornerDip() const { return corner::kOverlayDip; }
```

`corner.hpp`를 포함시키십시오. 이번 작업에서 이것을 덮어쓰는 콘텐츠는 없습니다. 통로만 열어 둡니다.

### 5-3. `src/popup_surface.cpp` — 계층을 콘텐츠에서 받는다

`:670` 언저리의 반지름 계산을 다음으로 바꾸십시오.

```cpp
  const int tier = content_->CornerDip();
  const float radius = corner::ClampPx(corner::ToPx(tier, Dpi()), width, height);
```

`max_radius`를 손으로 계산하던 세 줄은 `ClampPx`가 대신하므로 지웁니다.

`:691`, `:694`의 채우기와 선 그리기는 계층이 hero일 때만 지오메트리로 갈라집니다. 지금은 hero 콘텐츠가 없어서 실행되지 않지만, 통로를 함께 넣어 두어야 나중에 5-2의 한 줄만으로 승격이 끝납니다.

```cpp
    ID2D1PathGeometry* squircle =
        corner::IsHero(tier) ? squircle_.Get(d2d_.Get(), rounded.rect, radius) : nullptr;
    if (fill_) {
      if (squircle != nullptr) {
        target_->FillGeometry(squircle, fill_.Get());
      } else {
        target_->FillRoundedRectangle(rounded, fill_.Get());
      }
    }
    if (stroke_) {
      if (squircle != nullptr) {
        target_->DrawGeometry(squircle, stroke_.Get(), 1.0f);
      } else {
        target_->DrawRoundedRectangle(rounded, stroke_.Get(), 1.0f);
      }
    }
```

`PopupSurface`에 `corner::SquircleCache squircle_;` 멤버를 두십시오. 렌더 타깃을 버리는 자리에서 `squircle_.Reset()`도 함께 부르십시오.

### 5-4. `src/dock.cpp` — 알약을 hero 스쿼클로 올린다

`:2000`의 반지름을 `corner::ToPx(corner::kHeroDip, dpi)`로 바꾸고 `ClampPx`로 자르십시오. 독은 항목이 하나만 남아도 폭이 슬롯 하나(64 DIP)라서 잘릴 일이 드물지만, 안전장치를 빼지 마십시오.

`:2013`, `:2016`의 `FillRoundedRectangle`과 `DrawRoundedRectangle`을 `FillGeometry`와 `DrawGeometry`로 바꾸십시오. 지금 쓰는 `0.5f` 안쪽 오프셋은 선이 화소 경계에 걸리게 하는 장치이므로 그대로 유지해야 합니다. 지오메트리에 넘기는 사각형도 같은 오프셋을 가진 것이어야 합니다.

**`Dock`에 `corner::SquircleCache` 멤버를 반드시 두십시오.** 독은 호버와 실행 애니메이션으로 매 프레임 다시 그립니다. 프레임마다 지오메트리를 새로 만들면 80 fps 목표를 위협합니다. 지오메트리는 항목 수가 바뀌어 폭이 달라질 때만 다시 만들어지면 됩니다. 지오메트리를 만들 팩토리는 `:233`의 `D2dFactory()`를 쓰십시오.

`:2067`의 아이콘 호버 리터럴 `Dip(8)`을 `corner::HoverPx(bg 높이, dpi)`로 바꾸십시오. 호버 상자는 슬롯에서 `kHoverInsetDip`만큼 줄인 것이므로 높이는 슬롯 높이에서 인셋 두 배를 뺀 값입니다.

`:2077`의 실행 표시 점은 이미 `dot_h * 0.5f`입니다. `corner::PillPx(dot_h)`로 이름만 바꾸십시오.

### 5-5. `src/spotlight.cpp` — 카드를 hero로, 검색창을 동심으로

`:34`, `:35`의 `kCardRadiusDip`, `kSearchRadiusDip`을 삭제하십시오.

`:2346`의 카드 반지름은 `corner::ToPx(corner::kHeroDip, dpi)`입니다. 18에서 16으로 내려갑니다.

`:2361`의 카드 채우기를 `FillGeometry`로 바꾸십시오. `Spotlight`에도 `corner::SquircleCache` 멤버를 두십시오. 결과 행 수에 따라 카드 높이가 바뀌므로 캐시 열쇠가 갱신되지만, 타자 한 글자마다가 아니라 결과 개수가 바뀔 때만 다시 만들어집니다.

**`:2352`부터의 그림자 여섯 겹은 원호로 남겨 두십시오.** 알파가 0.028부터 시작하는 흐린 겹이라 곡선의 차이가 화면에 드러나지 않는데, 스쿼클로 바꾸면 지오메트리를 프레임마다 여섯 개 더 만들게 됩니다.

`:2364`의 검색창 반지름은 동심 공식으로 유도합니다.

```cpp
  const float search_radius =
      corner::ConcentricPx(radius, static_cast<float>(Dip(kPadDip)), corner::ToPx(corner::kOverlayDip, dpi), dpi);
```

검색창은 카드 가장자리에서 `kPadDip`(12 DIP)만큼 안쪽에 있고 이 값이 hero(16)보다 작으므로 영향권 안입니다. 따라서 결과는 `16 - 12 = 4`가 되어 지금의 12에서 크게 내려갑니다. **이것이 규칙대로의 정답입니다.** 화면에서 어색해 보이더라도 반지름을 손으로 되돌리지 마십시오. 고칠 자리는 반지름이 아니라 패딩입니다. `kPadDip`을 8로 줄이면 동심 결과가 `16 - 8 = 8`이 되어 overlay 계층과 맞아떨어집니다. 어느 쪽이 나은지 화면을 찍어 함께 보고하십시오.

`:2459`의 행 호버 리터럴 `Dip(8)`을 `corner::HoverPx(row.rect 높이, dpi)`로 바꾸십시오.

### 5-6. `src/control_center.cpp`

`:26`의 `kRadiusDip`을 삭제하십시오.

`:660`과 `:811`의 반지름은 카드용입니다. 제어 센터는 `PopupSurface` 안에 들어가고 창의 반지름은 overlay(8)입니다. 카드는 패널 가장자리에서 `kPanelPadDip`(14 DIP)만큼 떨어져 있는데 이 값이 8보다 크므로 **영향권 밖**입니다. 따라서 카드는 제 계층 값을 그대로 씁니다.

```cpp
  const float radius = corner::ToPx(corner::kOverlayDip, dpi);
```

4에서 8로 올라갑니다.

`:774`, `:777`의 슬라이더 트랙과 `:850`의 토글 트랙은 이미 `h * 0.5f`입니다. `corner::PillPx(h)`로 이름만 바꾸십시오.

`:826` 언저리에서 `fill_round`로 그리는 뒤로 가기 단추 호버는 카드가 아니라 호버입니다. `corner::HoverPx`를 쓰도록 별도 람다로 갈라내십시오.

### 5-7. `src/clock_flyout.cpp`

`:16`의 `kCardRadiusDip`을 삭제하십시오.

`:490`의 카드 반지름은 `corner::ToPx(corner::kOverlayDip, dpi)`입니다. 값은 8 그대로지만 이제 토큰에서 옵니다.

이 콘텐츠는 `PaintsOwnChrome()`이 참이라 창 배경 없이 카드 두 개를 직접 그립니다. 카드가 곧 바깥 표면이므로 계층 값을 직접 쓰는 것이 맞습니다.

`:524`와 `:826`의 호버 리터럴 `DipToPx(4, dpi)`를 `corner::HoverPx(rc 높이, dpi)`로 바꾸십시오. 두 자리 모두 `rc`를 이미 들고 있습니다.

### 5-8. `src/clock_renderer.cpp`

`:20`의 `kStartHoverRadiusDip`을 삭제하십시오.

`:469`의 시작 단추 호버는 호버 규칙을 따릅니다. 다만 이 자리는 픽셀이 아니라 DIP 좌표계로 그리고 있으므로 `HoverPx`를 그대로 부르면 안 됩니다. 같은 비율과 같은 상한을 DIP로 직접 적용하십시오.

```cpp
    const float hover_r = (std::min)(static_cast<float>(corner::kOverlayDip),
                                     (std::max)(static_cast<float>(corner::kControlDip),
                                                (height_dip - kStartHoverInsetYDip * 2.0f) * 0.22f));
```

**`:173`부터의 배터리 아이콘과 `:125`의 `AddRoundedPolygon`은 건드리지 마십시오.** 아이콘 그림이지 표면이 아닙니다. 아이콘 그림의 곡률은 글자 크기에 묶여 있고 이 규칙의 대상이 아닙니다.

### 5-9. `src/start_menu.cpp`

`:368`의 행 호버 리터럴 `Dip(6)`을 `corner::HoverPx(row.rect 높이, dpi)`로 바꾸십시오.

`:425`가 `dwm::kCornerRound`로 창의 곡률을 DWM에 맡기고 있습니다. **이번 작업에서는 그대로 두십시오.** 이 창을 우리가 직접 깎으려면 레이어드 창으로 바꿔야 하고, 그것은 `FIX-POPUP-RADIUS.md`가 팝업에 했던 것과 같은 크기의 별도 작업입니다. 다만 DWM의 `ROUND`가 약 8 px이라 새 overlay 계층과 우연히 맞습니다. 결과적으로 이번 작업 뒤에도 시작 메뉴만 어긋나 보이지는 않습니다.

### 5-10. `src/status_panel.cpp`

`:42`의 `BarCornerRadius` 함수를 삭제하십시오. 게이지 막대는 계층이 아니라 알약입니다. 호출부 `:305`, `:442`를 `corner::PillPx(bar_h)`, `corner::PillPx(s_track_h)`로 바꾸십시오. 지금 `min(4, h/2)`가 막대 높이 6 DIP에서 3을 내놓고 있어 결과 값은 같지만, 근거가 생깁니다.

`:407`의 토글 트랙은 이미 `track_h * 0.5f`입니다. `corner::PillPx`로 이름만 바꾸십시오.

`:400`의 행 호버가 `FillRectangle`을 쓰고 있습니다. 둥근 팝업 안에서 각진 강조가 칠해지는 자리입니다. `FillRoundedRectangle`에 `corner::HoverPx(b - t, dpi)`를 넣어 다른 표면과 맞추십시오.

---

## 6. 하지 않을 것

1. **상단바(`src/menu_bar.cpp`)의 곡률은 0으로 둡니다.** 화면 전체 너비를 차지하는 바에 곡률을 주면 모서리 바깥에 갈 곳이 없습니다.
2. **`src/menu_bar.cpp:1814`, `:1875`의 `CreatePopupMenu` 계열은 손대지 않습니다.** Win32 네이티브 메뉴는 우리가 그리는 표면이 아니어서 곡률을 맞출 방법이 없습니다. control 계층을 4로 남기는 이 규칙에서는 그 메뉴가 우리 팝업 옆에서 튀지 않습니다.
3. **레이아웃 패딩을 바꾸지 마십시오.** 유일한 예외는 5-5에서 검증 뒤 판단할 Spotlight의 `kPadDip`입니다.
4. **`src/theme.cpp`의 색은 이번 작업의 대상이 아닙니다.**
5. 아이콘 그림(배터리, CPU 링, 돋보기, 시작 로고)의 곡률은 규칙 밖입니다.

---

## 7. 성능

스쿼클 지오메트리를 프레임마다 만들면 안 됩니다. 캐시가 필요한 표면은 독과 Spotlight, 그리고 나중에 hero로 올릴 팝업입니다. 셋 다 `corner::SquircleCache` 멤버를 두고 그것을 거쳐야 합니다.

렌더 타깃을 다시 만드는 자리에서 캐시도 함께 `Reset()` 하십시오. 지오메트리 자체는 팩토리에 묶여 있고 렌더 타깃에 묶여 있지 않아 살아남지만, 크기가 바뀐 뒤 낡은 모양이 남는 것을 막는 편이 안전합니다.

`TASK-BAR-DRAW-PERF.md`에서 재던 독 그리기 시간을 이 작업 전후로 함께 재십시오.

---

## 8. 검증

1. 빌드가 경고 없이 통과해야 합니다. `/W4`이므로 `std::pow`와 `std::lround`의 인자 형 변환에서 경고가 나기 쉽습니다.
2. `kCornerRadiusDip`을 지운 뒤 남는 참조가 없어야 합니다. 컴파일이 통과하면 확인된 것입니다.
3. **독 알약의 모서리가 원호가 아닌 것이 눈에 보여야 합니다.** 스쿼클이 실제로 적용되었는지 확인하는 가장 확실한 방법은 `kSquircleExponent`를 잠시 2.0으로 바꿔 그린 화면과 4.0으로 그린 화면을 나란히 놓고 비교하는 것입니다. 두 화면이 같아 보이면 지오메트리 경로를 타지 않고 원호로 물러서고 있는 것입니다. 확인한 뒤 4.0으로 되돌리십시오.
4. Spotlight 카드도 같은 방법으로 확인하십시오.
5. 독 아이콘 호버, Spotlight 행 호버, 시작 메뉴 행 호버, 시계 플라이아웃 행 호버, 상태 패널 행 호버를 모두 띄워 **곡률이 서로 같은 계열로 보이는지** 확인하십시오. 지금은 4, 6, 8이 섞여 있습니다.
6. 제어 센터를 열어 카드 곡률이 4에서 8로 올라간 것과, 패널 창의 곡률도 8인 것을 확인하십시오.
7. 항목이 하나뿐인 아주 좁은 팝업을 열어 `ClampPx`가 동작하는지 보십시오. 마주 보는 모서리가 서로 먹어 들어가 모양이 뭉개지면 안 됩니다.
8. 100%, 150%, 200% 배율에서 각각 확인하십시오. 특히 200%에서 스쿼클의 조각 이음매가 보이면 `SegmentsFor`의 상한 24를 올려야 합니다.
9. 밝은 테마와 어두운 테마 양쪽에서 확인하십시오. 1 px 선이 지오메트리로 바뀌면서 밝은 테마에서 흐려지기 쉽습니다.
10. 독에 항목을 추가하고 제거해 폭이 바뀔 때 알약 모양이 따라오는지 보십시오. 캐시 열쇠가 폭을 놓치면 낡은 모양이 남습니다.
11. **검증에 레지스트리를 쓰지 마십시오.** 테마 확인은 이미 설정된 상태에서 하십시오.
12. **화면에 입력을 합성하지 마십시오.** 클릭이나 키 입력이 필요한 확인은 준비만 해 두고 사용자에게 부탁하십시오.

---

## 9. 보고할 것

- 독 알약과 Spotlight 카드를 지수 2.0과 4.0으로 각각 그린 화면. 차이가 보이지 않으면 원호로 물러선 것이므로 원인을 찾아 함께 보고하십시오.
- Spotlight 검색창을 동심 결과 4로 그린 화면과, `kPadDip`을 8로 줄여 8이 나오게 한 화면. 어느 쪽을 택할지는 이 두 장을 보고 정합니다.
- 호버 다섯 자리를 한 장에 담은 화면.
- 독 그리기 시간의 작업 전후 측정값.
- `corner::HoverPx`가 각 자리에서 실제로 내놓은 값. 규칙이 의도대로 유도되는지 숫자로 확인합니다.
