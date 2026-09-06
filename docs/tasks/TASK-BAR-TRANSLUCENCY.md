# 작업 지시서: 상단바 배경을 반투명하게 만든다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/dwm.hpp`입니다. 갈래 B로 넘어가는 경우에만 `src/clock_renderer.cpp`와 `src/theme.cpp`도 건드립니다.

이 지시서는 `src/menu_bar.cpp`의 `ApplyBackdrop` 하나만 건드리므로 `TASK-START-MENU-UNIFY.md`, `FIX-WINX-MENU-WIDTH.md`와 순서를 다투지 않습니다. 다만 같은 파일이므로 셋을 동시에 편집하지는 마십시오.

---

## 1. 무엇을 바꾸는가

상단바 배경이 지금은 뒤가 비치지 않습니다. 맥 메뉴바처럼 **뒤에 있는 창과 바탕 화면이 흐릿하게 비쳐 보이게** 만드십시오.

기준은 독입니다. 독은 이미 `DockFillColor`의 알파(다크 0.78, 라이트 0.82)로 반투명하게 그려지고 있습니다. 상단바가 그와 같은 계열의 인상을 주어야 합니다.

---

## 2. 지금 코드가 하는 일

읽어서 확인한 사실입니다.

**`src/menu_bar.cpp`의 `ApplyBackdrop`(1189행 부근)이 DWM에 세 가지를 요청합니다.**

```cpp
DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));

const int backdrop = dwm::kBackdropMainWindow;   // 2 = Mica
DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));

const int corner = dwm::kCornerDoNotRound;
DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));

const MARGINS margins{-1, -1, -1, -1};
DwmExtendFrameIntoClientArea(hwnd_, &margins);
```

**상단바는 배경을 스스로 칠하지 않습니다.** `src/clock_renderer.cpp`의 718행에서 `rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f))`로 화면 전체를 알파 0으로 비우고, 그 위에 아이콘과 글자만 올립니다. 즉 배경은 전적으로 DWM 백드롭에 맡겨져 있습니다.

**`MenuBar::Paint`는 `BufferedPaintSetAlpha`를 부르지 않습니다.** 알파를 살려서 내보내려는 의도입니다. 참고로 `src/start_menu.cpp`의 `Paint`는 반대로 `BufferedPaintSetAlpha(buffer, &client, 255)`를 불러 알파를 죽입니다.

**상단바 창은 레이어드 창이 아닙니다.** `WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST`이고 `WS_EX_LAYERED`가 없습니다(357행). 독(`src/dock.cpp` 1123행)과 팝업(`src/popup_surface.cpp`)은 `WS_EX_LAYERED`에 `UpdateLayeredWindow`를 씁니다.

**창 클래스 배경 브러시가 검정입니다.** `wc.hbrBackground = GetStockObject(BLACK_BRUSH)`(344행). `BPPF_ERASE`로 버퍼를 지우므로 화면에 직접 칠해지지는 않지만, 백드롭이 붙지 않은 상태에서 무엇이 보이는지를 가릴 때 후보로 남겨 두십시오.

**요약하면 지금 구조는 "우리는 배경을 칠하지 않고 Mica에 맡긴다"입니다.** Mica는 설계상 바탕 화면 이미지만 흐리게 비추고 뒤에 있는 다른 창은 비추지 않습니다. 뒤의 창까지 비치기를 원한다면 Mica로는 되지 않습니다.

---

## 3. 먼저 측정하십시오

`DwmSetWindowAttribute`의 반환값을 지금까지 한 번도 확인한 적이 없습니다. 백드롭이 실제로 붙고 있는지조차 확정되지 않았으므로, 고치기 전에 이것부터 남기십시오.

`ApplyBackdrop`의 호출 세 개와 `DwmExtendFrameIntoClientArea`의 `HRESULT`를 각각 받아서 한 줄로 로그하십시오.

```cpp
Log(L"bar", L"backdrop dark=0x%08lx type=0x%08lx corner=0x%08lx extend=0x%08lx value=%d",
    static_cast<unsigned long>(hr_dark), static_cast<unsigned long>(hr_type),
    static_cast<unsigned long>(hr_corner), static_cast<unsigned long>(hr_extend), backdrop);
```

판정 기준입니다.

- 전부 `0x00000000`이면 DWM은 요청을 받아들인 것입니다. 그러면 **문제는 적용 실패가 아니라 Mica라는 효과 자체의 성질**이므로 4절로 가십시오.
- `kSystemBackdropType` 호출이 `0x80070057`(`E_INVALIDARG`)이면 이 창 스타일에서 시스템 백드롭을 못 쓰는 것입니다. 그러면 4절을 건너뛰고 5절로 가십시오.

로그를 남긴 빌드를 만든 뒤 `~/.bamti/bamti.log`의 `[bar] backdrop` 줄을 그대로 인용해서 보고하십시오.

---

## 4. 갈래 A: 아크릴로 바꾼다 (먼저 시도)

Mica(`kBackdropMainWindow`)를 아크릴(`kBackdropTransientWindow`)로 바꾸십시오. 아크릴은 창 뒤의 내용을 실제로 흐리게 비춥니다. 맥 메뉴바의 인상에 가장 가까운 것이 이쪽입니다.

`src/dwm.hpp`에 상수는 이미 있습니다.

```cpp
inline constexpr int kBackdropTransientWindow = 3;
```

`ApplyBackdrop`에서 `backdrop` 값만 바꾸면 됩니다. **다른 줄은 건드리지 마십시오.** 특히 `DwmExtendFrameIntoClientArea`의 `MARGINS{-1,-1,-1,-1}`을 지우면 백드롭이 그려질 자리가 사라지므로 그대로 두어야 합니다.

바꾼 뒤 확인할 것이 두 가지 있습니다.

1. **`kSystemBackdropType` 호출의 `HRESULT`가 그대로 `S_OK`인가.** 3절에서 넣은 로그로 봅니다.
2. **상단바 위의 글자와 아이콘이 아크릴 위에서 읽히는가.** 아크릴은 Mica보다 밝기 변동이 큽니다. 다크 테마와 라이트 테마 양쪽에서 시계 글자와 트레이 아이콘이 묻히지 않는지 확인해야 합니다.

2번이 문제가 되면 색을 바꾸지 말고 먼저 보고하십시오. 글자색 조정은 이 지시서의 범위가 아닙니다.

---

## 5. 갈래 B: 배경을 우리가 칠한다 (A가 안 될 때만)

시스템 백드롭이 이 창에 붙지 않는 것으로 3절에서 확정되었을 때만 하십시오.

방향은 **독과 같은 경로로 통일하는 것**입니다. 상단바 창에 `WS_EX_LAYERED`를 주고, `UpdateLayeredWindow` 계열로 알파가 살아 있는 비트맵을 올린 뒤, 배경을 `DockFillColor`와 같은 계열의 반투명 색으로 칠합니다. 그러면 백드롭에 기대지 않고 우리가 알파를 직접 정할 수 있습니다.

**다만 이 갈래는 시작하기 전에 반드시 보고하고 멈추십시오.** 아래 두 가지 때문에 판단할 것이 남아 있습니다.

1. **부분 갱신이 사라질 위험이 있습니다.** 지금 상단바는 `InvalidateArea`로 바뀐 조각만 다시 그립니다. 레이어드 창으로 옮기면 `UpdateLayeredWindow`는 창 전체를 올립니다. 폭이 넓은 모니터에서 초당 한 번 시계를 갱신하는 비용이 얼마나 늘어나는지 측정해야 합니다. `UpdateLayeredWindowIndirect`의 `prcDirty`로 부분 갱신이 되는지도 함께 확인해야 합니다.
2. **앱바 등록과 히트 테스트에 영향이 있는지 확인되지 않았습니다.** 상단바는 `RegisterAppBar`로 작업 영역을 차지하고 있고, 트레이 미러가 `Shell_NotifyIconGetRect`에 좌표로 답하고 있습니다.

보고할 때는 3절의 로그 줄과 함께, 위 두 가지 중 무엇을 먼저 측정할 계획인지 적으십시오.

---

## 6. 건드리지 말 것

- `src/clock_renderer.cpp`의 `rt_->Clear(...)` 알파 0은 갈래 A에서는 그대로 두십시오. 배경을 백드롭이 그리게 하는 전제입니다.
- `dwm::kCornerDoNotRound`를 바꾸지 마십시오. 상단바는 화면 위쪽에 붙은 띠이므로 모서리를 둥글리면 안 됩니다.
- 독과 팝업의 색과 알파(`DockFillColor`, `DockStrokeColor`)를 바꾸지 마십시오. 이 작업은 상단바만 다룹니다.

---

## 7. 검증

1. Release 빌드가 경고 없이 통과해야 합니다.
2. `~/.bamti/bamti.log`에 `[bar] backdrop` 줄이 남고 네 `HRESULT`가 모두 `0x00000000`이어야 합니다. 아니라면 그 값을 보고하십시오.
3. 화면 확인은 사용자가 합니다. **직접 스크린샷을 찍거나 입력을 합성하지 마십시오.** 빌드를 마친 뒤 다음 세 가지를 사용자에게 확인해 달라고 요청하십시오.
   - 상단바 뒤에 창을 하나 놓았을 때 그 창이 흐릿하게 비치는가.
   - 다크 테마와 라이트 테마 양쪽에서 시계 글자와 트레이 아이콘이 읽히는가.
   - 상단바 위쪽 모서리가 여전히 각져 있는가.
4. 유휴 상태에서 CPU 사용률이 눈에 띄게 오르지 않아야 합니다. 아크릴은 DWM이 그리므로 우리 프로세스 비용은 변하지 않아야 정상입니다. `Win32_PerfRawData_PerfProc_Process`의 값으로 확인하고, 변화가 있으면 보고하십시오.
