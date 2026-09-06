# 작업 지시서: 상단바를 실제로 반투명하게 만든다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-TRANSLUCENCY.md`(커밋 `3365dea`)의 후속입니다. **그 지시서의 판정 기준이 틀렸으므로 결론을 갈아엎습니다.**

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`입니다. 갈래 2로 넘어가면 `src/clock_renderer.cpp`와 `src/theme.cpp`도 건드립니다.

---

## 1. 앞선 작업이 왜 실패했는가

아크릴로 바꾼 뒤 로그는 이렇게 나왔습니다.

```
[bar] backdrop dark=0x00000000 type=0x00000000 corner=0x00000000 extend=0x00000000 value=3
```

앞선 지시서는 이것을 적용 성공으로 판정하게 했습니다. **그 판정이 틀렸습니다.** `DwmSetWindowAttribute`의 `S_OK`는 **DWM이 속성값을 받아 저장했다**는 뜻일 뿐이고, **그 효과를 실제로 그렸다**는 보장이 아닙니다.

사용자 화면으로 확인한 결과 **상단바는 완전한 검은 띠**입니다. 밝은 벽지 위에서도 전혀 비치지 않습니다. Mica도 아크릴도 그려지지 않았습니다.

**원인은 창 종류입니다.** 상단바는 캡션이 없는 `WS_POPUP` 창이고 확장 스타일이 `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`입니다(`src/menu_bar.cpp` 357행 부근). DWM 시스템 백드롭은 표준 프레임을 가진 창을 전제로 하며, 이런 창에는 `DwmExtendFrameIntoClientArea`로 여백을 넓혀도 실제로 그려지지 않습니다.

**그래서 지금 검게 보입니다.** `clock_renderer.cpp` 718행이 배경을 알파 0으로 비우는데, 상단바는 레이어드 창이 아니므로 `EndBufferedPaint`의 `BitBlt`가 알파 채널을 버립니다. 남는 것은 RGB 0,0,0, 즉 검정입니다. 창 클래스의 `hbrBackground`도 `BLACK_BRUSH`입니다.

**메뉴가 되는 이유가 여기서 갈립니다.** `PopupSurface`는 `WS_EX_LAYERED` 창이고 `UpdateLayeredWindow`에 `ULW_ALPHA`로 비트맵을 올립니다. 알파를 우리가 직접 합성하므로 DWM의 협조가 필요 없습니다. 상단바만 그 경로 밖에 있습니다.

---

## 2. 목표

맥 메뉴바처럼 **바탕 화면과 뒤에 있는 창이 비쳐 보이게** 만드십시오. 사용자가 보여 준 맥 화면에서는 벽지의 그라데이션이 메뉴바를 그대로 통과하고, 그 위에 옅은 어두운 틴트만 얹혀 있습니다.

---

## 3. 갈래 1: `SetWindowCompositionAttribute` (먼저 하십시오)

`user32.dll`의 `SetWindowCompositionAttribute`는 **창 종류를 가리지 않고** 블러와 아크릴을 겁니다. 작업 표시줄을 투명하게 만드는 TranslucentTB가 쓰는 것이 이 API이고, 상단바는 그 작업 표시줄과 같은 성격의 창입니다.

**문서화되지 않은 API입니다.** 헤더에 선언이 없으므로 직접 정의하고 `GetProcAddress`로 가져와야 합니다. 실패할 수 있는 경로마다 로그를 남기십시오.

```cpp
namespace {

enum AccentState : DWORD {
  kAccentDisabled = 0,
  kAccentEnableGradient = 1,
  kAccentEnableTransparentGradient = 2,
  kAccentEnableBlurBehind = 3,
  kAccentEnableAcrylicBlurBehind = 4,
  kAccentEnableHostBackdrop = 5,
};

struct AccentPolicy {
  DWORD state;
  DWORD flags;
  DWORD gradient_color;  // AABBGGRR 이다. RGB 가 아니라 BGR 순서인 것에 주의한다.
  DWORD animation_id;
};

struct WindowCompositionAttributeData {
  DWORD attrib;  // 19 = WCA_ACCENT_POLICY
  PVOID data;
  SIZE_T size;
};

using SetWindowCompositionAttributeFn = BOOL(WINAPI*)(HWND, WindowCompositionAttributeData*);

}  // namespace
```

`user32.dll`은 이미 로드되어 있으므로 `GetModuleHandleW(L"user32.dll")` 다음에 `GetProcAddress`로 가져오면 됩니다. 함수 포인터는 한 번만 찾아 정적으로 들고 있으십시오.

### 3-1. 무엇을 넣는가

`state`는 `kAccentEnableAcrylicBlurBehind`로 시작하십시오.

`gradient_color`는 **틴트**입니다. 상단바 위 글자가 읽혀야 하므로 테마에 따라 나눕니다. 바이트 순서가 `AABBGGRR`인 것에 주의하십시오.

- 다크: `0x99000000` 계열, 즉 알파 0x99에 검정 틴트
- 라이트: `0x99FFFFFF` 계열, 즉 알파 0x99에 흰 틴트

**알파를 0으로 두지 마십시오.** 아크릴에서 알파 0인 영역은 마우스 입력이 창을 통과해 뒤로 빠집니다. 상단바는 클릭과 호버를 받아야 하므로 치명적입니다. 시작값 0x99에서 시작해 사용자가 원하는 만큼 조정하십시오.

`flags`와 `animation_id`는 0으로 두십시오.

### 3-2. 어디서 부르는가

`MenuBar::ApplyBackdrop` 안에서 하십시오. 테마가 바뀔 때 이미 다시 불리고 있으므로(562행, 572행 부근) 틴트도 함께 따라갑니다.

**시스템 백드롭 요청은 지우십시오.** `kSystemBackdropType`을 `kBackdropNone`으로 바꾸고, `DwmExtendFrameIntoClientArea`의 `MARGINS{-1,-1,-1,-1}`도 `{0,0,0,0}`으로 되돌리십시오. 그리지도 않는 효과를 요청한 채로 두면 다음 사람이 또 그것을 원인으로 의심합니다.

`kUseImmersiveDarkMode`와 `kCornerDoNotRound`는 그대로 두십시오.

### 3-3. 창 클래스 배경 브러시

`wc.hbrBackground`를 `BLACK_BRUSH`에서 `nullptr`로 바꾸십시오(357행 부근). `PopupSurface`가 이미 `nullptr`입니다. 검은 브러시가 남아 있으면 아크릴이 붙기 전 한 프레임 동안 검정이 번쩍입니다.

### 3-4. 이것이 안 통할 때

`kAccentEnableAcrylicBlurBehind`가 이 Windows 빌드에서 동작하지 않을 수 있습니다. 다음 순서로 시험하고 **각각 화면이 어떻게 보이는지 사용자에게 확인받으십시오.**

1. `kAccentEnableAcrylicBlurBehind` (아크릴, 목표에 가장 가깝다)
2. `kAccentEnableBlurBehind` (예전 방식 블러, 틴트 색이 무시될 수 있다)
3. `kAccentEnableTransparentGradient` (블러 없이 반투명 틴트만, 벽지가 선명하게 비친다)

**셋 다 화면이 그대로 검으면 갈래 1을 접고 4절로 가십시오.** `SetWindowCompositionAttribute`의 반환값이 `TRUE`여도 화면이 바뀌지 않으면 실패로 판정하십시오. **이번 실패의 교훈이 그것입니다. 반환값이 아니라 화면이 판정 기준입니다.**

---

## 4. 갈래 2: 메뉴와 같은 레이어드 창 (갈래 1이 실패할 때만)

이 갈래는 **반드시 됩니다.** 메뉴가 이미 그 방식으로 반투명하게 그려지고 있는 것이 증거입니다. 대신 블러가 없어 벽지가 선명하게 비칩니다.

상단바 창에 `WS_EX_LAYERED`를 주고, `PopupSurface::Render`와 `Present`가 하는 것과 같은 구조로 옮기십시오. DIB 섹션에 그린 뒤 `UpdateLayeredWindow`에 `ULW_ALPHA`로 올립니다. 배경은 `theme.cpp`에 `BarFillColor(bool dark)`를 새로 만들어 `DockFillColor`와 같은 계열의 알파(다크 0.78, 라이트 0.82)로 칠하십시오.

**시작하기 전에 두 가지를 측정하고 보고하십시오.**

1. **부분 갱신입니다.** 지금 상단바는 `InvalidateArea`로 바뀐 조각만 다시 그립니다. `UpdateLayeredWindow`는 창 전체를 올립니다. 초당 한 번 시계를 갱신하는 비용이 얼마나 늘어나는지 `[perf] draw` 로그로 비교하십시오. `UpdateLayeredWindowIndirect`의 `prcDirty`로 부분 갱신이 되는지도 확인하십시오.
2. **앱바 등록과 트레이 좌표 응답입니다.** 상단바는 `RegisterAppBar`로 작업 영역을 차지하고 `Shell_NotifyIconGetRect`에 좌표로 답합니다. 레이어드 창으로 바꾼 뒤에도 이것이 유지되는지 확인해야 합니다.

측정 결과가 나오면 그대로 보고하고, 진행 여부는 사용자 판단을 기다리십시오.

---

## 5. 건드리지 말 것

- `dwm::kCornerDoNotRound`를 바꾸지 마십시오. 상단바는 화면 위쪽에 붙은 띠입니다.
- 독과 메뉴의 색과 알파(`DockFillColor`, `DockStrokeColor`)를 바꾸지 마십시오.
- `clock_renderer.cpp`의 `Clear(알파 0)`은 갈래 1에서는 그대로 두십시오. 그 자리에 아크릴이 그려집니다.
- 시계 글자색과 아이콘 색을 바꾸지 마십시오. 틴트를 조절해서 읽히게 만드는 것이 먼저입니다.

---

## 6. 검증

1. Release 빌드가 경고 없이 통과해야 합니다. **빌드는 CMake로 하십시오.** `bamti.vcxproj`는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.
2. `SetWindowCompositionAttribute`를 찾은 결과와 호출 반환값을 로그로 남기십시오. 다만 **이 값으로 성공을 판정하지 마십시오.**
3. **판정 기준은 화면입니다.** 직접 스크린샷을 찍거나 입력을 합성하지 말고 사용자에게 확인을 부탁하십시오.
   - 상단바에 바탕 화면이 비치는가.
   - 상단바 아래에 창을 놓았을 때 그 창이 비치는가.
   - 다크 테마와 라이트 테마 양쪽에서 시계 글자와 트레이 아이콘이 읽히는가.
   - 상단바의 아이콘과 시계를 클릭하고 마우스를 올리는 것이 예전처럼 동작하는가. **아크릴에서 알파가 낮으면 입력이 뒤로 빠지므로 이 항목을 빠뜨리지 마십시오.**
   - 위쪽 모서리가 여전히 각져 있는가.
4. 유휴 CPU가 눈에 띄게 오르지 않아야 합니다. `Win32_PerfRawData_PerfProc_Process`의 `PercentProcessorTime`으로 확인하십시오.
