# 작업 지시서: 상단바를 레이어드 창으로 바꿔 실제로 반투명하게 만든다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-BAR-ACRYLIC-BLUR.md`의 갈래 2입니다. **갈래 1은 실패로 확정됐으므로 이 지시서가 결론입니다.**

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, `src/clock_renderer.cpp`, `src/clock_renderer.hpp`, `src/theme.cpp`, `src/theme.hpp`입니다.

**작업 트리에 커밋되지 않은 `src/menu_bar.cpp` 변경(accent policy)이 남아 있습니다. 그것을 되돌리지 말고 그 위에서 이어서 하십시오.**

---

## 1. 갈래 1이 왜 실패했는가 (측정으로 확정)

`SetWindowCompositionAttribute`는 제대로 불렸습니다.

```
[bar] composition GetProcAddress ok err=0
[bar] composition set ok=1 err=0 state=4 color=0x99000000
```

**그런데 화면 픽셀은 완전한 검정입니다.** 상단바 안쪽 네 지점을 실측했습니다.

```
x=700  y=5,12,20  R=0 G=0 B=0
x=1200 y=5,12,20  R=0 G=0 B=0
x=1700 y=5,12,20  R=0 G=0 B=0
x=2200 y=5,12,20  R=0 G=0 B=0
```

같은 시각에 상단바 아래 창은 `R=40 G=44 B=52`였습니다. 아크릴이 알파 0x99(60%) 틴트로 그려졌다면 그 색이 40% 섞여 대략 `(16,17,20)`이 나와야 합니다. **완전한 0은 아크릴이 화면에 전혀 닿지 않았다는 뜻입니다.**

### 결정적 증거

실행 중인 bamti의 창들을 열거해 확장 스타일을 실측했습니다.

| 창 | ExStyle | `WS_EX_LAYERED` | 화면 |
| --- | --- | --- | --- |
| `bamti.PopupSurface` (메뉴) | `0x08080088` | 있다 | 반투명하다 |
| `bamti.Dock` | `0x08080088` | 있다 | 반투명하다 |
| `bamti.DockHot` | `0x08080088` | 있다 | — |
| **`bamti.MenuBar` (상단바)** | **`0x08000088`** | **없다** | **검다** |

**차이는 `0x00080000`, 즉 `WS_EX_LAYERED` 하나뿐입니다.** 반투명한 창은 전부 레이어드이고 검은 창만 레이어드가 아닙니다.

### 원인

`SetWindowCompositionAttribute`는 **창 뒤에** 아크릴을 그립니다. 그 위를 우리가 덮어 버리면 보이지 않습니다.

상단바는 `EndBufferedPaint(buffer, TRUE)`로 백버퍼를 `BitBlt` 합니다. **GDI 의 `BitBlt`는 알파 채널을 해석하지 않습니다.** `clock_renderer.cpp`가 배경을 알파 0으로 비워 둔 픽셀은 화면에 RGB `0,0,0`, 즉 **불투명한 검정**으로 찍힙니다. 그 검정이 뒤의 아크릴을 완전히 가립니다.

**메뉴가 되는 이유가 여기서 갈립니다.** `PopupSurface`는 `UpdateLayeredWindow`에 `ULW_ALPHA`로 비트맵을 올립니다. 이 경로만이 알파를 실제 합성에 씁니다.

**그러므로 상단바를 레이어드 창으로 바꾸는 것 외에 다른 길이 없습니다.** DWM 백드롭도, accent policy도, 그 위에 불투명 픽셀이 덮이는 한 소용이 없습니다.

---

## 2. 목표

맥 메뉴바처럼 바탕 화면과 뒤의 창이 비쳐 보이게 만드십시오. 기준 농도는 독과 같은 계열입니다(`DockFillColor`: 다크 알파 0.78, 라이트 0.82).

---

## 3. 무엇을 하는가

`src/popup_surface.cpp`의 `EnsureLayeredTarget`, `Render`, `Present`가 이미 완성된 본입니다. **그 구조를 그대로 따르십시오.** 새로 발명할 것이 없습니다.

### 3-1. 창에 `WS_EX_LAYERED`를 준다

`MenuBar::Create`의 `CreateWindowExW`(357행 부근)에 `WS_EX_LAYERED`를 더합니다.

```cpp
hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST | WS_EX_LAYERED,
                        kMenuBarClass, L"bamti", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
```

`wc.hbrBackground`는 이미 `nullptr`로 바뀌어 있습니다. 그대로 두십시오.

### 3-2. 그리기를 DIB 로 옮긴다

지금 `MenuBar::Paint`는 `BeginPaint` → `BeginBufferedPaint` → `clock_.Draw(buffer_dc, ...)` → `EndBufferedPaint` 순서입니다.

이것을 `PopupSurface`와 같은 구조로 바꿉니다.

1. 창 크기의 32비트 top-down DIB 섹션(`CreateDIBSection`, `BI_RGB`, `biHeight`를 음수)과 메모리 DC를 만들어 들고 있습니다. 창 크기나 DPI 가 바뀔 때만 다시 만듭니다.
2. `clock_.Draw`에 그 메모리 DC 를 넘겨 지금과 똑같이 그립니다. **`clock_renderer.cpp` 안의 그리기 로직은 바꾸지 마십시오.**
3. `UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, mem_dc, &src, 0, &blend, ULW_ALPHA)`로 올립니다. `blend`는 `AC_SRC_OVER` 에 `SourceConstantAlpha = 255`, `AlphaFormat = AC_SRC_ALPHA` 입니다.

`WM_PAINT`는 더 이상 그리기의 주 경로가 아닙니다. `PopupSurface`처럼 `BeginPaint`/`EndPaint`만 하고 실제 그리기는 `Present`류 함수에서 하십시오.

**D2D 렌더 타깃은 미리 곱해진 알파(`D2D1_ALPHA_MODE_PREMULTIPLIED`)를 씁니다.** `PopupSurface`가 이미 그렇게 만들고 있으니 같은 속성으로 만드십시오.

### 3-3. 배경을 칠한다

`src/theme.hpp`와 `theme.cpp`에 상단바 배경색을 더합니다.

```cpp
D2D1_COLOR_F BarFillColor(bool dark);
```

값은 `DockFillColor`와 같은 계열에서 시작하십시오. 다크 `(0.12, 0.12, 0.12, 0.78)`, 라이트 `(0.98, 0.98, 0.98, 0.82)`입니다.

`clock_renderer.cpp` 718행의 `rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f))`를 `rt_->Clear(BarFillColor(dark))`로 바꾸십시오. **`Clear`는 미리 곱해진 알파로 처리되므로 별도 계산이 필요 없습니다.**

**알파를 0으로 두지 마십시오.** 레이어드 창에서 알파 0인 픽셀은 마우스 입력을 통과시킵니다. 상단바 전체가 그렇게 되면 클릭도 호버도 받지 못합니다.

### 3-4. accent policy 는 남겨 두십시오

작업 트리에 있는 `ApplyAccentPolicy` 호출을 지우지 마십시오. 레이어드 창이 되면 우리 배경이 반투명해지므로, 그 뒤에 아크릴 블러가 실제로 보이게 될 수 있습니다. **그때는 맥 화면에 한 걸음 더 가까워집니다.**

블러가 보이는지는 화면으로 판정합니다. 보이지 않아도 레이어드 전환만으로 목표는 달성되므로, **아크릴이 안 보인다는 이유로 되돌리지는 마십시오.** 결과를 보고만 하십시오.

---

## 4. 반드시 확인할 세 가지

레이어드 창 전환은 그리기 방식이 바뀌는 일이라 아래가 함께 흔들릴 수 있습니다. **각각 측정하고 결과를 보고하십시오.**

### 4-1. 갱신 비용

지금 상단바는 `InvalidateArea`로 바뀐 조각만 다시 그립니다. `UpdateLayeredWindow`는 창 전체를 올립니다. 상단바는 3413×32 픽셀이므로 한 번에 약 437KB 입니다.

`[perf] draw` 로그의 `end` 항목이 전환 전후로 어떻게 달라지는지 비교하십시오. 전환 전 실측값은 `end=0.70/1.9`(평균/최대 밀리초)였습니다.

**평균이 5밀리초를 넘으면** `UpdateLayeredWindowIndirect`의 `prcDirty`로 부분 갱신을 시도하고 다시 재십시오.

### 4-2. 앱바 등록

상단바는 `RegisterAppBar`로 작업 영역을 차지합니다. 레이어드 창이 된 뒤에도 다른 창이 상단바 아래에서 최대화되는지 확인하십시오.

### 4-3. 마우스 입력

레이어드 창은 알파에 따라 히트 테스트가 달라집니다. `popup_surface.cpp`의 `PointInWindow` 주석에 그 성질이 이미 적혀 있습니다.

- 시작 단추, 트레이 아이콘, 시계, 제어 센터를 클릭할 수 있는가.
- 호버 강조가 뜨는가.
- 트레이 미러가 `Shell_NotifyIconGetRect`에 답하는 좌표가 그대로인가. 로그의 `intercept getrect` 줄로 확인합니다.

---

## 5. 건드리지 말 것

- `clock_renderer.cpp`의 세그먼트 그리기 로직, 아이콘, 글자색을 바꾸지 마십시오. 바꾸는 것은 `Clear` 한 줄과 그리기 대상 DC 뿐입니다.
- `dwm::kCornerDoNotRound`를 바꾸지 마십시오.
- 독과 메뉴의 색과 알파를 바꾸지 마십시오.
- `DwmSetWindowAttribute` 호출들을 지우지 마십시오. `kUseImmersiveDarkMode`는 여전히 필요합니다.

---

## 6. 검증

1. Release 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj`는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.
2. **창 스타일을 실측해 `WS_EX_LAYERED`(`0x00080000`)가 켜졌는지 확인하십시오.** ExStyle 이 `0x08080088`이 되어야 합니다.
3. **화면 픽셀을 실측하십시오.** 상단바 안쪽 몇 지점의 RGB 가 `0,0,0`이 아니어야 하고, 뒤에 있는 것의 색이 섞여 나와야 합니다. **이것이 이 작업의 성공 판정입니다.** 반환값이나 `HRESULT`로 판정하지 마십시오. 앞선 두 번의 실패가 모두 그 지점에서 났습니다.
4. 4절의 세 항목을 측정해 보고하십시오.
5. 사람 눈으로 봐야 하는 것은 사용자에게 부탁하십시오. 입력을 합성하지 마십시오.
   - 상단바에 바탕 화면이 비치는가. 농도가 적당한가.
   - 뒤의 창이 흐려 보이는가, 선명하게 비치는가. (블러 여부를 판정하는 항목입니다)
   - 다크와 라이트 양쪽에서 시계 글자와 트레이 아이콘이 읽히는가.
   - 위쪽 모서리가 각져 있는가.
