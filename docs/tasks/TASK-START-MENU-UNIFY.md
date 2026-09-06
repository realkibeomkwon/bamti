# 작업 지시서: 시작 단추 좌클릭 메뉴를 독 메뉴와 같은 디자인으로 통일한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/start_menu.hpp`, `src/start_menu.cpp`, `src/menu_bar.hpp`, `src/menu_bar.cpp`, `src/popup_surface.hpp`, `src/popup_surface.cpp`입니다.

**`FIX-WINX-MENU-WIDTH.md`를 먼저 끝낸 뒤에 하십시오.** 둘 다 `src/menu_bar.cpp`를 건드리는데, 그쪽이 훨씬 작습니다.

---

## 1. 무엇을 바꾸는가

상단바 왼쪽 끝 Windows 아이콘을 **왼쪽으로 누르면 뜨는 메뉴**가 지금은 혼자만 다른 모양입니다. 이것을 **독 우클릭 메뉴, 상단바 우클릭 메뉴와 완전히 같은 그리기 경로**로 옮기십시오.

같은 아이콘을 오른쪽으로 눌러 뜨는 Win+X 메뉴는 이미 그 경로를 쓰고 있습니다(`MenuBar::ShowStartContextMenu`). 좌클릭 메뉴도 같아지면 됩니다.

바뀌지 말아야 할 것은 **항목과 동작**입니다. 여섯 줄과 그 동작을 그대로 옮기십시오.

```
파일 탐색기
설정
실행
────────────
절전
다시 시작
시스템 종료
```

---

## 2. 지금 무엇이 다른가

`src/start_menu.cpp`의 `StartMenu`는 팝업 기반 메뉴와 아무것도 공유하지 않는 독립 구현입니다. 읽어서 확인한 차이입니다.

| 항목 | `StartMenu` (지금) | `PopupSurface` + `BarMenuContent` (목표) |
| --- | --- | --- |
| 창 | `WS_EX_TOOLWINDOW \| WS_EX_TOPMOST`, 레이어드 아님 | `WS_EX_LAYERED \| WS_EX_NOACTIVATE` |
| 배경 | `0.94` 회색 불투명 단색 | `DockFillColor` 반투명 + `DockStrokeColor` 테두리 |
| 다크 테마 | **무시한다.** `dark_`를 받아 두고 `Paint`에서 쓰지 않으며 `ApplyChrome`도 `dark = FALSE` 고정 | `RenderMenuRows(target, dpi, hot, dark, rows)` |
| 모서리 | DWM `kCornerRound` | `corner`의 `kMenuCornerDip`(10) 곡률을 우리가 그린다 |
| 강조 | `MenuItemHoverFill(false, false)`, 즉 검정 6% 칠 | `AccentFillColor` 파란 칠에 흰 글자 |
| 행 높이 | 32 DIP | 30 DIP (`kMenuRowDip`) |
| 바깥 여백 | 8 DIP | 6 DIP (`kMenuPadDip`) |
| 폭 | 280 DIP 고정 | 글자 폭에 맞춰 계산 |
| 글자 | `0.09` 검정 고정 | `ClockTextColor(dark)` |

**즉 라이트 테마 전용으로 굳어 있고 곡률과 강조 색과 치수가 전부 다릅니다.** 그리기를 부분적으로 고치는 것보다 경로 자체를 옮기는 편이 짧고, 앞으로 메뉴 디자인을 한 곳에서 바꿀 수 있게 됩니다.

---

## 3. `src/start_menu.*`를 동작 모듈로 줄인다

`StartMenu` 클래스를 지우십시오. 창, 렌더러, 레이아웃, 그리기, 히트 테스트, 강조 이동이 전부 필요 없어집니다.

**파일 이름은 그대로 두십시오.** `CMakeLists.txt`와 `bamti.vcxproj`를 건드리지 않기 위해서입니다.

`src/start_menu.hpp`는 이렇게 줄입니다.

```cpp
#pragma once

namespace bamti {

enum class StartAction { kExplorer, kSettings, kRun, kSleep, kRestart, kShutdown };

// 시작 메뉴 항목의 동작을 실행한다. 메뉴 창은 이 함수가 불리기 전에 이미 닫혀 있어야 한다.
void InvokeStartAction(StartAction action);

}  // namespace bamti
```

`src/start_menu.cpp`에는 지금 `ActivateRow`, `LaunchPath`, `SendWinChord`가 하던 일만 남깁니다. **동작을 바꾸지 마십시오.**

- `kExplorer`: `ShellExecuteExW`로 `explorer.exe`
- `kSettings`: `ShellExecuteExW`로 `ms-settings:`
- `kRun`: `SendWinChord(L'R')`, 즉 Win+R 조합을 `SendInput`으로 보낸다
- `kSleep`: `SetSuspendState(FALSE, TRUE, FALSE)`
- `kRestart`: 권한을 켠 뒤 `ExitWindowsEx(EWX_REBOOT, 0)`
- `kShutdown`: 권한을 켠 뒤 `ExitWindowsEx(EWX_SHUTDOWN, 0)`

`EnableShutdownPrivilege`는 `src/winx_menu.cpp`의 익명 이름 공간에 같은 이름의 함수가 이미 있습니다. **합치지 말고 각자 두십시오.** 헤더로 끌어내면 이 지시서의 범위를 넘습니다. 다만 `src/start_menu.cpp` 쪽은 지금 실패해도 로그를 남기지 않으니, `winx_menu.cpp`처럼 실패한 단계를 `Log(L"start", ...)`로 남기십시오.

`kRun`, `kSleep`, `kRestart`, `kShutdown`이 지금은 자기 창을 먼저 `Hide()`하고 나서 실행합니다. 팝업으로 옮기면 팝업이 스스로 닫힌 뒤에 `WM_COMMAND`가 도착하므로 그 처리는 필요 없습니다.

---

## 4. 메뉴를 팝업으로 연다

`MenuBar::ShowStartContextMenu`(2169행 부근)가 이미 같은 일을 하고 있습니다. **그 함수를 본으로 삼으십시오.**

### 4-1. 명령 아이디

`src/menu_bar.cpp` 위쪽 상수 자리(40~61행)에 여섯 개를 더합니다. `kMenuWidgetsSubCmd`가 30, `kMenuTraySubCmd`가 31이므로 40번대가 비어 있습니다.

```cpp
constexpr UINT kStartExplorerCmd = 40;
constexpr UINT kStartSettingsCmd = 41;
constexpr UINT kStartRunCmd = 42;
constexpr UINT kStartSleepCmd = 43;
constexpr UINT kStartRestartCmd = 44;
constexpr UINT kStartShutdownCmd = 45;
```

**이미 쓰이는 값과 겹치지 않는지 반드시 확인하십시오.** 예전에 `kWidgetControlCenterCmd`와 `kTrayPeekCmd`가 둘 다 20이어서 한 번에 실행되던 결함이 있었습니다(`TASK-BAR-MENU-ITEMS.md`).

### 4-2. 열림 상태

`src/menu_bar.hpp`에서 `StartMenu start_menu_;`(177행)를 지우고 대신 플래그를 둡니다.

```cpp
bool start_popup_open_ = false;
```

`cc_open_`과 `clock_open_`이 이미 같은 방식으로 쓰이고 있으니 그 관례를 그대로 따르십시오. `kPopupClosedMsg` 처리(529행)에서 `cc_open_`과 `clock_open_`을 내릴 때 `start_popup_open_`도 함께 내리십시오.

### 4-3. `ToggleStartMenu`를 다시 씁니다

지금 구현(1682행 부근)은 `status_popup_.Close()`를 먼저 부르고 `start_menu_.Toggle(...)`을 부릅니다. 이제 시작 메뉴 자신이 `status_popup_`을 쓰므로 순서를 바꿔야 합니다.

```
1. fullscreen_occluded_ 이면 아무것도 하지 않는다. (지금과 같다)
2. start_popup_open_ 이고 status_popup_.IsOpen() 이면 -> 닫고 끝낸다. 이것이 토글이다.
3. 열려 있는 다른 팝업과 Spotlight 를 닫는다. (cc_open_, clock_open_, open_panel_id_ 를 정리하는 방식은
   ShowStartContextMenu 와 같게 한다)
4. bar_menu_ 를 Reset 하고 여섯 줄과 구분선을 넣는다.
5. StartRect() 의 왼쪽 아래를 화면 좌표로 바꿔 앵커로 삼고 Anchor::BelowAt 으로 연다.
6. start_popup_open_ = true 로 두고 시작 단추를 다시 그린다.
```

앵커 계산과 `SetAfterTick`, `SetDark`는 `ShowStartContextMenu`에서 하는 그대로 하십시오. 위치가 우클릭 메뉴와 정확히 같은 자리가 되는 것이 옳습니다.

**`from_keyboard` 인자는 남겨 두십시오.** 6절에서 씁니다.

### 4-4. `start_menu_` 참조를 모두 옮깁니다

`src/menu_bar.cpp`에 `start_menu_`를 쓰는 자리가 열여덟 곳 있습니다. 성격이 세 가지입니다.

1. **`start_menu_.visible()`로 시작 단추를 눌린 것처럼 그리는 자리**(1349행) → `start_popup_open_`으로 바꿉니다.
2. **`start_menu_.Hide()`로 다른 UI를 열기 전에 닫는 자리**(674, 1608, 1708, 1719, 1795, 1824, 1883, 2174행 등) → `if (start_popup_open_) { status_popup_.Close(); InvalidateArea(hwnd_, StartRect()); }` 형태로 바꿉니다. **`status_popup_.Close()`를 조건 없이 부르면 다른 팝업까지 닫으므로 반드시 플래그를 먼저 보십시오.**
3. **`Warmup`(409행)** → 팝업은 `MenuBar::Create`에서 이미 예열하고 있으므로(`status_popup_.Create` 안에서 렌더 타깃을 미리 만듭니다) 이 줄은 지웁니다.

323행과 2483행의 `start_menu_.Hide()`도 같은 규칙으로 처리하십시오. 하나도 빠뜨리지 마십시오. 컴파일 오류로 전부 드러납니다.

### 4-5. 명령 처리

`HandleCommand`(1012행 부근, `kSettingsCmd` 처리 근처)에 여섯 개를 더합니다.

```cpp
if (cmd >= kStartExplorerCmd && cmd <= kStartShutdownCmd) {
  InvokeStartAction(static_cast<StartAction>(cmd - kStartExplorerCmd));
}
```

`StartAction`의 나열 순서를 명령 아이디 순서와 맞춰 두었으므로 이 변환이 성립합니다. **둘 중 하나만 바꾸면 조용히 어긋나므로 두 곳에 서로를 가리키는 주석을 한 줄씩 남기십시오.**

---

## 5. 무엇이 저절로 따라오는가

경로를 옮기면 아래는 코드를 더 쓰지 않아도 독 메뉴와 같아집니다. 확인만 하십시오.

- 반투명 배경과 테두리 (`PopupSurface::Render`가 `DockFillColor`와 `DockStrokeColor`로 그립니다)
- 다크 테마 대응 (`BarMenuContent::Render`가 `dark_`를 `RenderMenuRows`에 넘깁니다)
- 파란 강조와 흰 글자, 강조가 행보다 위아래로 4 DIP 물러나는 모양
- 모서리 곡률 10 DIP (`BarMenuContent::CornerDip`)
- 바깥 클릭, Esc, Win 키로 닫히는 동작
- 폭이 글자에 맞춰 정해지는 것

---

## 6. 키보드 조작을 잃지 마십시오

**이것이 이 작업에서 가장 놓치기 쉬운 부분입니다.**

지금 `StartMenu`는 키보드로 다룰 수 있습니다. Win 키로 열면 첫 줄이 강조되고(`from_keyboard`가 참이면 `hot_ = 0`), 위아래 방향키로 옮기고(`MoveHot`), Enter로 실행합니다. 이것이 가능한 이유는 `StartMenu`가 `SetForegroundWindow`와 `SetFocus`로 포커스를 받아 `WM_KEYDOWN`을 직접 받기 때문입니다.

**`PopupSurface`에는 이 기능이 없습니다.** `WS_EX_NOACTIVATE` 창이라 키보드 메시지가 오지 않고, 지금은 `Tick`에서 Esc와 Win 키만 `GetAsyncKeyState`로 폴링합니다. 그대로 옮기면 키보드 조작이 사라집니다.

`PopupSurface::Tick`에 위/아래/Enter 폴링을 더하십시오. Esc와 Win 키를 처리하는 자리 바로 다음이 자연스럽습니다.

- 이미 있는 `ReadAsyncKey`를 그대로 쓰십시오. `down`과 `pressed_since` 두 값을 돌려줍니다.
- `VK_DOWN`: 다음으로 고를 수 있는 행으로 `hot_`을 옮깁니다. 끝에 닿으면 처음으로 돌아갑니다.
- `VK_UP`: 반대 방향입니다.
- `VK_RETURN`: `hot_`이 0 이상이면 그 행을 실행합니다. 이미 있는 `InvokeRow(hot_)`을 쓰십시오.
- **구분선과 꺼진 행은 건너뛰어야 합니다.** `MenuRow::separator`와 `enabled`를 `PopupContent` 밖에서는 볼 수 없으므로, `PopupContent`에 `virtual bool Selectable(int index) const { return true; }`를 더하고 `BarMenuContent`에서 이를 구현하십시오. `MenuHitTest`가 이미 같은 조건으로 걸러 내고 있으니 그 판정을 그대로 씁니다.
- 키를 눌러 `hot_`이 바뀌면 다시 그려야 합니다. `Present()`를 부르십시오.

**함정 두 가지입니다.**

1. **누르고 있는 동안 계속 넘어가면 안 됩니다.** `down`만 보면 폴링 주기마다 한 칸씩 갑니다. 앞선 상태를 기억해 눌리는 순간에만 한 번 움직이십시오. Esc가 `esc_down_`으로 하고 있는 방식과 같습니다.
2. **`Open`에서 앞선 상태의 씨앗을 심으십시오.** `esc_down_`과 `win_down_`을 `GetAsyncKeyState`로 미리 채우는 줄(265~267행) 옆에 방향키와 Enter도 같이 채워야, 메뉴를 연 키 입력이 곧바로 첫 행을 실행해 버리는 일이 생기지 않습니다.

**`from_keyboard`가 참일 때 첫 행을 강조하는 것**은 `PopupSurface::Open` 뒤에 `TrackHotScreen` 대신 쓸 수단이 필요합니다. `void SetHot(int index)`를 하나 더해 `hot_`을 정하고 `Present()`를 부르게 하십시오. 마우스로 열었을 때는 지금처럼 강조 없이(`hot_ = -1`) 시작해야 합니다.

이 변경은 독 메뉴와 상단바 우클릭 메뉴에도 함께 적용됩니다. 그쪽에서도 방향키가 도는지 확인하고, 예상 밖의 부작용이 있으면 보고하십시오.

---

## 7. 건드리지 말 것

- 항목 여섯 개의 문구와 순서, 구분선 자리를 바꾸지 마십시오.
- `menu_style.cpp`의 치수와 색을 바꾸지 마십시오. 이 작업은 시작 메뉴를 기존 규격에 맞추는 것이지 규격을 바꾸는 것이 아닙니다.
- Win+X 메뉴(`ShowStartContextMenu`)의 동작을 바꾸지 마십시오.
- `kStartMenuClass` 창 클래스 이름은 쓰이지 않게 되므로 헤더에서 지우십시오. 남겨 두면 다음 사람이 살아 있는 창으로 오해합니다.

---

## 8. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다.
2. `src` 전체에서 `start_menu_`와 `StartMenu`를 검색해 남은 참조가 없어야 합니다.
3. `~/.bamti/bamti.log`에 시작 메뉴를 열 때 `[popup] open rows=7`이 남아야 합니다. 구분선을 포함해 일곱 줄입니다.
4. 화면 확인은 사용자에게 부탁하십시오. **직접 스크린샷을 찍거나 입력을 합성하지 마십시오.** 확인 항목입니다.
   - 좌클릭 메뉴가 독 우클릭 메뉴와 배경, 테두리, 모서리, 강조 색, 글자 크기가 같은가.
   - 다크 테마에서 글자가 흰색으로 나오는가. (지금은 검은색으로 나옵니다)
   - Win 키로 열었을 때 첫 줄이 강조되고 위아래 방향키와 Enter가 도는가.
   - 좌클릭 메뉴가 뜬 자리가 우클릭 Win+X 메뉴가 뜨는 자리와 같은가.
   - 여섯 항목이 모두 예전과 같이 동작하는가. **`다시 시작`과 `시스템 종료`는 실제로 누르지 말고 사용자에게 판단을 맡기십시오.** 확인이 필요하면 `BAMTI_POWER_DRYRUN` 같은 안전 장치를 쓰는 방법을 먼저 제안하십시오.
