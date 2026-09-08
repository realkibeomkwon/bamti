# 작업 지시서: 독 우클릭 메뉴 아래에 꼬리를 붙인다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

**`FIX-DOCK-MENU-ANCHOR.md` 를 끝낸 뒤에 하십시오.** 메뉴가 아이콘 중심에 정렬되어 있어야 꼬리가 가리킬 곳이 생깁니다.

건드리는 파일은 `src/corner.hpp`, `src/corner.cpp`, `src/popup_surface.hpp`, `src/popup_surface.cpp`, `src/dock.cpp` 입니다. 호버 라벨의 꼬리는 이 지시서의 대상이 아닙니다. `FIX-LABEL-TAIL.md` 에서 이어서 합니다.

---

## 1. 무엇을 만드는가

맥의 독 우클릭 메뉴는 카드 아래쪽에 삼각형 꼬리가 나와서 누른 아이콘을 가리킵니다. 스크린샷 실측값입니다.

| 항목 | 픽셀 | 카드 높이(352px)를 214 DIP 로 본 환산 |
| --- | --- | --- |
| 카드 높이 | 352 | 214 DIP |
| 꼬리 밑변 | 50 | 30 DIP |
| 꼬리 높이 | 20 | 12 DIP |
| 꼬리 꼭짓점 x | 87.5 | 아이콘의 가로 중심과 일치 |

카드가 화면 가장자리에 밀려도 **꼬리는 아이콘의 가로 중심을 가리켜야 합니다.** 그래서 꼭짓점의 위치는 카드의 가운데가 아니라 앵커에서 계산합니다.

## 2. 고칠 것

### 2.1 카드와 꼬리를 한 도형으로 만든다

칠과 테두리가 이어지려면 **닫힌 경로 하나**여야 합니다. 삼각형을 따로 그리면 카드의 아래 변이 꼬리를 가로지릅니다.

`src/corner.hpp` 의 `BuildSquircle` 선언 앞에 넣습니다.

```cpp
// 카드 아래로 뻗는 꼬리. 맥 독 메뉴 실측에서 밑변 30 DIP, 높이 12 DIP 이고,
// 꼭짓점은 가리키려는 대상의 가로 중심에 둔다.
inline constexpr int kTailBaseDip = 30;
inline constexpr int kTailHeightDip = 12;
// 꼭짓점을 살짝 둥글린다. 뾰족하면 저배율에서 지저분해진다.
inline constexpr int kTailTipDip = 3;

struct Tail {
  float apex_x = 0.0f;    // 꼭짓점 x. 카드와 같은 좌표계다.
  float base_px = 0.0f;
  float height_px = 0.0f;
  float tip_px = 0.0f;
};

// 둥근 사각형 카드와 꼬리를 이어 붙인 닫힌 경로. 실패하면 nullptr 이다.
// 호출자는 그때 꼬리 없는 둥근 사각형으로 물러선다.
Microsoft::WRL::ComPtr<ID2D1PathGeometry> BuildCallout(ID2D1Factory* factory, const D2D1_RECT_F& card,
                                                       float radius_px, const Tail& tail);

// 같은 모양을 다시 만들지 않게 들고 있는 그릇. SquircleCache 와 쓰임이 같다.
class CalloutCache {
 public:
  ID2D1PathGeometry* Get(ID2D1Factory* factory, const D2D1_RECT_F& card, float radius_px, const Tail& tail);
  void Reset();

 private:
  Microsoft::WRL::ComPtr<ID2D1PathGeometry> geom_;
  D2D1_RECT_F card_{};
  float radius_ = -1.0f;
  Tail tail_{};
};
```

`src/corner.cpp` 에 정의를 넣습니다. 경로는 왼쪽 위에서 시작해 시계 방향으로 돕니다.

```cpp
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
```

`CalloutCache::Get` 은 `SquircleCache::Get` 과 같은 방식으로 씁니다. 사각형과 반지름에 더해 **`apex_x` 와 `base_px` 와 `height_px` 까지 같을 때만** 들고 있던 것을 돌려주십시오. 꼭짓점이 움직이는데 옛 도형을 돌려주면 꼬리가 엉뚱한 곳을 가리킵니다.

### 2.2 팝업이 꼬리 자리를 확보하고 그린다

`src/popup_surface.hpp` 의 `PopupSurface` 공개부에 추가합니다.

```cpp
  // 참이면 카드 아래에 꼬리를 붙이고, 꼭짓점이 앵커의 가로 좌표를 가리킨다.
  void SetTail(bool on) { tail_ = on; }
```

비공개부에는 이렇게 넣습니다.

```cpp
  bool tail_ = false;
  int tail_px_ = 0;
  corner::CalloutCache callout_;
```

`src/popup_surface.cpp` 의 `Open` 에서 높이를 늘립니다. `Place` 를 부르기 전입니다.

```cpp
  const UINT dpi = Dpi();
  SIZE size = content_->Measure(dpi);
  if (size.cx <= 0 || size.cy <= 0) {
    content_ = nullptr;
    return false;
  }
  tail_px_ = tail_ ? MulDiv(corner::kTailHeightDip, static_cast<int>(dpi), 96) : 0;
  size.cy += tail_px_;
```

`Place` 는 고칠 것이 없습니다. `AboveCenter` 가 `y = anchor.y - size.cy - gap` 이므로, 늘어난 높이만큼 창이 위로 올라가고 **꼬리 끝이 앵커에서 4 DIP 떨어진 자리**에 놓입니다.

`Render` 의 배경 그리는 부분을 고칩니다. `own_chrome` 이 참인 콘텐츠는 지금처럼 배경을 그리지 않습니다.

```cpp
  const float width = static_cast<float>(dib_w_);
  const float height = static_cast<float>(dib_h_);
  const float card_h = height - static_cast<float>(tail_px_);
  const bool own_chrome = content_->PaintsOwnChrome();
  const int tier = content_->CornerDip();
  const float radius = corner::ClampPx(corner::ToPx(tier, Dpi()), width, card_h);
  const D2D1_ROUNDED_RECT rounded{D2D1::RectF(0.5f, 0.5f, width - 0.5f, card_h - 0.5f), radius, radius};
```

**반지름을 자를 때 전체 높이가 아니라 카드 높이를 넘기는 것이 중요합니다.** 그리는 부분은 이렇게 됩니다.

```cpp
  if (!own_chrome) {
    ID2D1PathGeometry* shape = nullptr;
    if (tail_px_ > 0) {
      RECT wr{};
      GetWindowRect(hwnd_, &wr);
      corner::Tail tail{};
      tail.apex_x = static_cast<float>(anchor_.x - wr.left);
      tail.base_px = corner::ToPx(corner::kTailBaseDip, Dpi());
      tail.height_px = static_cast<float>(tail_px_);
      tail.tip_px = corner::ToPx(corner::kTailTipDip, Dpi());
      shape = callout_.Get(d2d_.Get(), rounded.rect, radius, tail);
    } else if (corner::IsHero(tier)) {
      shape = squircle_.Get(d2d_.Get(), rounded.rect, radius);
    }
    if (fill_) {
      if (shape != nullptr) {
        target_->FillGeometry(shape, fill_.Get());
      } else {
        target_->FillRoundedRectangle(rounded, fill_.Get());
      }
    }
    if (stroke_) {
      if (shape != nullptr) {
        target_->DrawGeometry(shape, stroke_.Get(), 1.0f);
      } else {
        target_->DrawRoundedRectangle(rounded, stroke_.Get(), 1.0f);
      }
    }
  }
  content_->Render(target_.Get(), Dpi(), hot_);
```

`content_->Render` 는 그대로 둡니다. 메뉴 행은 위에서부터 쌓이므로 아래에 붙은 꼬리가 행을 밀지 않습니다.

`BindDC` 가 실패해서 자원을 버리는 자리에 `squircle_.Reset();` 이 있습니다. 그 옆에 `callout_.Reset();` 을 함께 넣으십시오.

### 2.3 독 메뉴만 꼬리를 켠다

`src/dock.cpp` 의 `Dock::Create` 에서 `popup_.SetDark(dark_);` 아래에 한 줄을 넣습니다.

```cpp
  popup_.SetTail(true);
```

**`submenu_` 에는 넣지 마십시오.** 하위 메뉴는 오른쪽에 붙는 판이라 꼬리가 없습니다.

## 3. 검증

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

새 바이너리는 WMI 로 띄웁니다. bamti 는 단일 인스턴스라서 새로 띄우면 예전 것이 스스로 종료됩니다.

```powershell
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '"D:\repos\bamti\build\Release\bamti.exe"'
}
"rc=$($r.ReturnValue) pid=$($r.ProcessId)"
```

**화면에 클릭이나 키 입력을 합성하지 마십시오.** 아래를 사용자에게 부탁하고 결과를 받으십시오. 화면 캡처로 모양을 재는 것은 괜찮습니다.

1. 독 가운데쯤의 아이콘을 우클릭합니다. 카드 아래 가운데에서 꼬리가 나와 아이콘을 가리켜야 합니다.
2. 꼬리와 카드의 테두리가 **한 줄로 이어져야 합니다.** 카드의 아래 변이 꼬리를 가로지르면 도형이 하나로 합쳐지지 않은 것입니다.
3. 독 맨 왼쪽 아이콘과 맨 오른쪽 아이콘을 우클릭합니다. 카드가 화면 안으로 밀려도 **꼬리는 여전히 그 아이콘을 가리켜야 합니다.**
4. 꼬리 끝이 독을 덮지 않아야 합니다.
5. 상단바의 시계, 제어 센터, 상태 아이콘 팝업에는 꼬리가 없어야 합니다.
6. "옵션" 하위 메뉴가 예전 자리에 그대로 붙어야 합니다.
7. 행을 오르내릴 때 강조가 예전처럼 따라와야 합니다. 매 프레임 도형을 새로 만들면 안 되므로, `CalloutCache` 가 실제로 재사용되는지 확인하십시오.

## 4. 하지 말 것

- `BuildSquircle` 과 `SquircleCache` 를 고치지 마십시오. 독 배경과 Spotlight 카드가 쓰고 있습니다.
- 꼬리를 별도의 창이나 별도의 도형으로 그리지 마십시오. 테두리가 끊깁니다.
- 메뉴 행의 치수(`src/menu_style.cpp`)를 바꾸지 마십시오.
- `PopupContent` 인터페이스에 꼬리 관련 메서드를 추가하지 마십시오. 꼬리는 창의 성질이지 내용의 성질이 아닙니다.
- 상단바 팝업에 꼬리를 켜지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
