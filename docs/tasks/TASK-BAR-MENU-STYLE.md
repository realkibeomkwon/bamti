# 작업 지시서: 상단바 메뉴의 그리기를 독 메뉴와 하나로 합친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

이 지시서는 뒤따르는 세 건(`TASK-BAR-MENU-ITEMS.md`, `TASK-BAR-WINX-MENU.md`, `TASK-BAR-SHOW-DESKTOP.md`)의 토대입니다. **가장 먼저 하십시오.**

건드리는 파일은 새로 만드는 `src/menu_style.hpp`와 `src/menu_style.cpp`, 그리고 `src/dock.cpp`, `src/bar_menu.hpp`, `src/bar_menu.cpp`, `CMakeLists.txt`, `bamti.vcxproj`입니다.

---

## 1. 무엇이 문제인가

메뉴를 그리는 코드가 두 벌 있고, 두 벌의 값이 서로 다릅니다. 그래서 독에서 연 메뉴와 상단바에서 연 메뉴가 다른 프로그램의 메뉴처럼 보입니다.

독 쪽은 `src/dock.cpp`의 익명 이름 공간에 있는 `MakeDockMenuMetrics`, `MeasureDockMenuRows`, `DockMenuRowRectOf`, `DrawDockMenuCheck`, `DrawDockMenuChevron`, `RenderDockMenuRows`이고, 상단바 쪽은 `src/bar_menu.cpp`의 `BarMenuContent::Measure`와 `BarMenuContent::Render`와 `BarMenuContent::RowRect`입니다.

실제로 어긋난 값은 다음과 같습니다.

| 항목 | 독(`dock.cpp`) | 상단바(`bar_menu.cpp`) |
| --- | --- | --- |
| 행 높이 | `kMenuRowDip = 30` | `kMenuRowDip = 28` |
| 구분선 높이 | `kMenuSepDip = 11` | `kMenuSepDip = 8` |
| 최소 너비 | `kMenuMinWidthDip = 160` | `kMenuMinWidthDip = 168` |
| 최대 너비 | `kMenuMaxWidthDip = 280` | `kMenuMaxWidthDip = 520` |
| 체크 칸 | `kMenuCheckDip = 18` | `kMenuCheckDip = 16` |
| 꺾쇠 칸 | `kMenuArrowDip = 16` | `kMenuArrowDip = 14` |
| 글자 좌우 여백 | 없음 | `kMenuTextPadDip = 12`을 너비에 두 번 더함 |
| 강조 칠 | `AccentFillColor`를 둥근 사각(`kMenuHoverRadiusDip = 6`)으로 칠하고 위아래로 `kMenuHoverInsetDip = 4`만큼 물러남 | `MenuItemHoverFill`을 행 전체에 각진 사각으로 칠함 |
| 강조된 행의 글자색 | `AccentOnColor` | 바뀌지 않음 |
| 체크 표시 | `DrawDockMenuCheck`가 그리는 직선 두 마디 | `L"✓"` 글리프 |
| 꺾쇠 표시 | `DrawDockMenuChevron`이 그리는 직선 두 마디 | `L"›"` 글리프 |
| 팝업 모서리 | `CornerDip()`이 `kMenuCornerDip = 10`을 돌려줌 | 재정의하지 않아 `corner::kOverlayDip`가 쓰임 |

참고 화면은 `C:\Users\KIBEOMKWON\Pictures\Screenshots\Screenshot-2026-09-05_23-08-02.png`입니다. 이 화면(상단바 우클릭 메뉴)에서 체크 표시가 글꼴 글리프라 획이 가늘고 크기가 제각각으로 보이는 것이 이 표의 마지막 세 줄 때문입니다.

**독 쪽이 기준입니다.** 상단바를 독에 맞추십시오. 독의 값은 하나도 바꾸지 마십시오.

---

## 2. 공용 모듈을 새로 만든다

`src/menu_style.hpp`와 `src/menu_style.cpp`를 만들고, 지금 `dock.cpp`에 있는 메뉴 행 그리기를 **옮깁니다.** 새로 쓰지 말고 옮겨야 두 메뉴가 확실히 같아집니다.

### 2-1. 헤더

```cpp
#pragma once

#include <d2d1.h>

#include <string>
#include <vector>

namespace bamti {

struct MenuRow {
  UINT id = 0;
  std::wstring text;
  bool separator = false;
  bool checked = false;
  bool submenu = false;
  bool enabled = true;
};

struct MenuMetrics {
  int pad = 0;
  int row_h = 0;
  int sep_h = 0;
  int check_w = 0;
  int arrow_w = 0;
};

inline constexpr int kMenuCornerDip = 10;
inline constexpr int kMenuMaxWidthDip = 280;

MenuMetrics MakeMenuMetrics(UINT dpi);
SIZE MeasureMenuRows(const std::vector<MenuRow>& rows, UINT dpi, int max_width_dip = kMenuMaxWidthDip);
RECT MenuRowRectOf(const std::vector<MenuRow>& rows, int index, UINT dpi, int width);
int MenuHitTest(const std::vector<MenuRow>& rows, POINT client, UINT dpi, int width);
void RenderMenuRows(ID2D1RenderTarget* target, UINT dpi, int hot, bool dark, const std::vector<MenuRow>& rows);

}  // namespace bamti
```

`enabled`가 새로 붙은 항목입니다. 독의 행에는 이 개념이 없었으므로 기본값 `true`가 그대로 독의 동작이 됩니다.

`max_width_dip`을 인자로 뺀 이유는 3절에 적었습니다.

### 2-2. 옮길 때 지킬 것

1. `MakeDockMenuMetrics`, `MeasureDockMenuRows`, `DockMenuRowRectOf`, `DrawDockMenuStroke`, `DrawDockMenuCheck`, `DrawDockMenuChevron`, `RenderDockMenuRows`의 **본문을 그대로 가져오십시오.** 이름만 위 헤더의 이름으로 바꿉니다. 계산식과 좌표 상수(`cx - 4.2f * s` 따위)는 한 글자도 고치지 마십시오.
2. `kMenuPadDip`, `kMenuRowDip`, `kMenuSepDip`, `kMenuMinWidthDip`, `kMenuMaxWidthDip`, `kMenuCheckDip`, `kMenuArrowDip`, `kMenuHoverRadiusDip`, `kMenuHoverInsetDip`, `kMenuCornerDip`도 값을 유지한 채 `menu_style.cpp`(또는 위 헤더)로 옮깁니다. `dock.cpp`에 남은 같은 이름의 상수는 **지우십시오.** 두 벌이 남으면 다음 사람이 또 어긋나게 만듭니다.
3. `dock.cpp`의 `DockMenuRow`는 지우고 `MenuRow`를 씁니다. `DockMenuContent`와 `DockSubmenuContent`의 `rows_` 타입, 초기화 목록(`{id, text, separator, checked, submenu}`) 순서가 `MenuRow`의 필드 순서와 맞는지 확인하십시오. **`MenuRow`는 `enabled`가 맨 뒤이므로 기존 다섯 개짜리 초기화 목록은 그대로 컴파일됩니다.** 그래도 눈으로 한 번 대조하십시오.
4. `RenderMenuRows`에 `enabled` 처리를 더합니다. 세 곳입니다.
   - 강조 칠: `const bool selected = i == hot && row.enabled;`로 바꿔 꺼진 행에는 칠하지 않습니다.
   - 글자색: 꺼진 행은 `ClockTextColor(dark)`의 알파에 `0.38f`를 곱한 색을 씁니다. 지금 `bar_menu.cpp`가 쓰는 값과 같습니다.
   - `MenuHitTest`는 `separator`인 행과 `enabled`가 거짓인 행을 건너뜁니다.
5. `MenuHitTest`는 지금 `DockMenuContent::HitTest`와 `BarMenuContent::HitTest`에 복사되어 있는 반복문을 그대로 가져온 것입니다. 세 곳이 모두 이 함수를 부르게 하십시오.

### 2-3. 빌드에 넣기

`CMakeLists.txt`와 `bamti.vcxproj` 양쪽에 `src/menu_style.cpp`를 더합니다. 기존 항목이 어떤 방식으로 나열되어 있는지 보고 그 방식을 따르십시오.

---

## 3. 최대 너비만 인자로 남긴다

독 메뉴의 최대 너비 280 DIP를 상단바에도 그대로 씁니다. 다만 트레이 아이콘 서브메뉴는 앱이 준 문자열을 그대로 싣기 때문에(예: `Tailscale: Connected. Click for options.`) 280에서는 대부분 말줄임표로 잘립니다.

`DrawPopupText`가 이미 `CreateEllipsisTrimmingSign`으로 말줄임을 붙이므로 잘려도 깨지지는 않습니다. 그래도 읽을 수 있게 **트레이 아이콘 서브메뉴에서만 `MeasureMenuRows`에 360을 넘기십시오.** 다른 모든 메뉴는 기본값 280입니다.

---

## 4. `BarMenuContent`를 공용 모듈 위에 다시 얹는다

`src/bar_menu.hpp`에서 `BarMenuRow`를 지우고 `MenuRow`를 씁니다. `Add`의 서명은 지금 호출부가 많으므로 그대로 둡니다.

```cpp
void Add(UINT id, std::wstring text, bool checked = false, bool enabled = true, bool submenu = false);
```

`src/bar_menu.cpp`에서 지울 것은 다음과 같습니다.

- 파일 위쪽 익명 이름 공간의 `kMenu*` 상수 전부와 `ScaleAlpha`
- `BarMenuContent::Measure`의 본문 → `return MeasureMenuRows(rows_, dpi, max_width_dip_);`
- `BarMenuContent::Render`의 본문 → `RenderMenuRows(target, dpi, hot_index, dark_, rows_);`
- `BarMenuContent::RowRect`의 본문 → `return MenuRowRectOf(rows_, index, dpi, width);`
- `BarMenuContent::HitTest`의 반복문 → `MenuHitTest(rows_, client, dpi, width)`

더할 것은 다음과 같습니다.

- `int CornerDip() const override { return kMenuCornerDip; }`
- `void SetMaxWidthDip(int dip)`와 `int max_width_dip_ = kMenuMaxWidthDip;`. 3절의 트레이 서브메뉴만 이 함수로 360을 넣습니다.

`DipToPx`가 `bar_menu.cpp`에서 더 쓰이지 않으면 함께 지웁니다. 남은 곳이 있으면 둡니다.

---

## 5. 검증

1. **빌드.** Release 구성으로 클린 빌드가 경고 없이 통과해야 합니다.
2. **눈으로 대조.** bamti를 띄우고 독 아이콘 우클릭 메뉴와 상단바 빈 곳 우클릭 메뉴를 나란히 띄운 화면을 찍으십시오. 행 높이, 강조 칠의 모양과 색, 체크 표시의 획 굵기, 꺾쇠 모양, 바깥 모서리 곡률이 같아야 합니다.
3. **체크 표시.** 상단바 메뉴에서 켜져 있는 항목(배터리, CPU 등)의 체크가 글꼴 글리프가 아니라 직선 두 마디로 그려져야 합니다. 마우스를 올려 강조된 상태에서도 체크가 `AccentOnColor`로 함께 바뀌는지 보십시오.
4. **꺼진 항목.** `트레이 아이콘` 서브메뉴를 비운 상태에서 뜨는 `미러 중인 아이콘이 없습니다` 행이 흐린 글씨로 남고, 마우스를 올려도 강조되지 않으며, 눌러도 반응이 없어야 합니다.
5. **독 회귀.** 독 메뉴의 모양이 이 작업 전과 똑같아야 합니다. 달라졌다면 옮기는 과정에서 값을 흘린 것입니다.

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
