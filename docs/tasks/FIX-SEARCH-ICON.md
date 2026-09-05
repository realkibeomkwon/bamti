# 수정 지시서: Spotlight 아이콘을 Windows 11 검색 아이콘 모양으로 그린다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-TRAY-ICON-THEME.md` 다음에 하십시오.

건드리는 파일은 `src/clock_renderer.cpp`, `src/clock_renderer.hpp`, `src/dock.cpp`입니다.

---

## 1. 현재 상태

Spotlight 아이콘이 두 군데에 있고 둘 다 Segoe Fluent Icons의 `U+E721` 글리프를 씁니다.

- 상단바: `src/clock_renderer.cpp`의 `kSearchFluent`, `ClockRenderer::DrawSpotlightButton`이 그립니다.
- 독: `src/dock.cpp`의 `kSearchFluentGlyph`, `BitmapFromFluentSearch`가 GDI `DrawTextW`로 비트맵을 만듭니다.

`U+E721`은 선이 가늘고 손잡이가 짧아서, 사용자가 요청한 Windows 11 내장 검색 아이콘보다 흐리게 보입니다.

## 2. 수정 방향

글리프를 다른 글리프로 바꾸지 말고 **직접 그리십시오.** 이 저장소는 이미 배터리와 CPU를 `IconKind::kVector`로 직접 그리고 있고(`TASK-BAR-ICON-ART.md`), 시작 단추의 Windows 로고도 셸 SVG의 실측 수치를 그대로 옮겨 그렸습니다. 검색 아이콘도 같은 방식이 맞습니다. 글리프는 굵기와 여백을 우리가 고를 수 없습니다.

셸 아이콘 폴더(`C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Icons\`)에는 검색 SVG가 없습니다. 확인했습니다. 그래서 모양을 수치로 지정합니다.

### 2-1. 기하 (16 DIP 상자 기준)

```
원   중심 (6.75, 6.75)   반지름 4.10   선 굵기 1.50
손잡이 (9.65, 9.65) → (13.35, 13.35)   선 굵기 1.50   끝 모양 둥글게
```

- 손잡이의 시작점은 원의 45도 방향 접점입니다. `6.75 + 4.10/√2 = 9.65`. 원과 손잡이가 어긋나 보이면 이 관계가 깨진 것입니다.
- 상자의 위아래 여백이 각각 1.9로 같습니다. 아이콘이 한쪽으로 쏠려 보이면 중심이나 끝점을 잘못 옮긴 것입니다.
- 다른 크기로 그릴 때는 **모든 수치에 `크기/16`을 곱하십시오.** 선 굵기도 함께 곱합니다. 독의 36 DIP에서는 배율이 2.25입니다.

### 2-2. 상단바

`ClockRenderer`에 `DrawSearchGlyph(ID2D1SolidColorBrush* brush, const D2D1_RECT_F& box)`를 더하고, `DrawSpotlightButton`에서 `DrawFluentOrFallback(..., kSearchFluent, kSearchFallback)` 대신 부르십시오.

- `ID2D1StrokeStyle`은 `EnsureStroke()`가 만드는 `round_stroke_`를 재사용하십시오. CPU 링이 이미 쓰고 있습니다. 끝 모양이 둥근지 확인하고, 아니면 이 아이콘 전용 스타일을 하나 더 만드십시오.
- 원은 `DrawEllipse`, 손잡이는 `DrawLine`입니다. 채우지 마십시오.
- 색은 지금처럼 `ClockTextColor(dark)`입니다.
- `kSearchFluent`와 `kSearchFallback`이 다른 곳에서 안 쓰이면 지우십시오.

### 2-3. 독

`BitmapFromFluentSearch(int px, bool dark)`가 GDI로 글리프를 찍고 있습니다. 이 함수의 내부만 바꾸십시오. 이름과 시그니처와 부르는 쪽은 그대로 둡니다.

GDI 펜으로 그리면 계단이 보입니다. 독은 이미 D2D DC 렌더 타깃을 쓰는 경로를 갖고 있습니다(`Dock::EnsureLayeredTarget`이 `CreateDCRenderTarget`으로 DIB에 그립니다). 같은 방식으로 `px × px` DIB 섹션을 만들고, 거기에 묶은 DC 렌더 타깃에 2-1의 도형을 그린 뒤 그 DIB의 `HBITMAP`을 돌려주십시오.

- 픽셀 형식은 `DXGI_FORMAT_B8G8R8A8_UNORM` + `D2D1_ALPHA_MODE_PREMULTIPLIED`입니다. 독의 아이콘 합성 경로가 미리 곱해진 알파를 전제로 합니다. `FinalizeIconBitmap`이 어떤 알파를 기대하는지 확인하고 맞추십시오.
- 배경은 완전히 투명하게 비우고 도형만 그립니다.
- 색은 `dark`에 따라 `ClockTextColor(dark)`와 같은 값을 쓰십시오.
- 이 함수 안에서 D2D 팩토리를 매번 만들지 마십시오. `D2dFactory()`가 이미 있습니다.

`kSearchFluentGlyph`가 다른 곳에서 안 쓰이면 지우십시오.

## 3. 검증

1. 상단바 검색 단추와 독 검색 아이콘이 같은 모양으로 보여야 합니다.
2. 100%, 150%, 200% 배율에서 각각 확인하십시오. 선이 흐려지거나 굵기가 들쭉날쭉하면 배율 계산이 틀린 것입니다.
3. 라이트 테마와 다크 테마에서 각각 확인하십시오.
4. 독 아이콘을 확대해서 보았을 때 원의 가장자리에 계단이 보이지 않아야 합니다. 보이면 D2D가 아니라 GDI로 그려진 것입니다.
5. 첨부된 Windows 11 검색 아이콘과 나란히 놓고 굵기와 손잡이 길이를 비교하십시오. 눈에 띄게 다르면 2-1의 수치를 조정하고, **조정한 값을 보고하십시오.**

## 4. 보고할 것

- 두 곳의 아이콘을 찍은 화면. 배율 100%와 200% 각각.
- 2-1에서 수치를 바꿨다면 바꾼 값과 이유.
