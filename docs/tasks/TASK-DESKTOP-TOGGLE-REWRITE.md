# 작업 지시서: Ctrl + 코너 바탕 화면 전환을 처음부터 다시 만든다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, `src/task_list.cpp`, `src/task_list.hpp`, 새로 만드는 `src/desktop_toggle.hpp`와 `src/desktop_toggle.cpp`, `CMakeLists.txt`, `bamti.vcxproj`입니다.

**이번에는 증상 하나를 덧대어 고치지 않습니다. 바탕 화면을 전환하는 실행부를 통째로 걷어내고 새 모듈로 다시 만듭니다.** 감지부(훅, 예비 폴링, 코너 감시, 뜸, 걸쇠)는 로그로 정상 동작이 확인되었으므로 구조를 유지하되 탐침 코드만 걷어냅니다. 어디를 남기고 어디를 버리는지는 4절과 7절에 정확히 적어 두었습니다.

---

## 1. 사용자가 겪은 것

2026-09-06 시험에서 세 가지가 보고되었습니다.

1. 드래그를 하는 도중에 Ctrl을 누르면 화면이 전환되지 않습니다.
2. Firefox를 맨 앞에 띄워 둔 채 전환을 시도하면 바탕 화면이 아니라 터미널이 맨 앞인 화면으로 바뀝니다.
3. 전환이 끝나기 전에 중간에 다른 창이 드러나는 것이 보입니다.

---

## 2. 로그로 확정한 원인

`~/.bamti/bamti.log`의 2026-09-06 15:54 구간이 근거입니다. **추측이 아니라 실측입니다.**

### 2-1. 셸의 `MinimizeAll`은 `S_OK`를 돌려주고도 아무 일도 하지 않는 경우가 있다

```
15:54:08.721 [peek] MinimizeAll hr=0x00000000 iconic_before=0 iconic_after=1 fg_before=0000000000EF0FC6 cls=MozillaWindowClass
15:54:10.815 [peek] MinimizeAll hr=0x00000000 iconic_before=0 iconic_after=0 fg_before=0000000000EF0FC6 cls=MozillaWindowClass
15:54:12.675 [peek] MinimizeAll hr=0x00000000 iconic_before=0 iconic_after=0 fg_before=0000000000EF0FC6 cls=MozillaWindowClass
```

첫 줄은 성공했고(`iconic_after=1`), 둘째와 셋째 줄은 **반환값이 `S_OK`인데도 Firefox가 최소화되지 않았습니다**(`iconic_after=0`). 두 경우 모두 직전에 `UndoMinimizeALL`을 부른 지 1초 남짓 지난 시점입니다.

곧 `IShellDispatch::MinimizeAll`과 `UndoMinimizeALL`의 반환값은 실제 효과를 보증하지 않습니다. 셸이 내부에 들고 있는 복원 스택의 상태에 따라 호출이 무시됩니다. **이 짝을 계속 쓰는 한 어떤 보정을 덧대어도 근본적으로 불안정합니다.**

### 2-2. `desktop_shown_` 플래그가 반환값만 보고 갱신되어 실제 화면과 어긋난다

`src/menu_bar.cpp`의 `MenuBar::ShowDesktop`입니다.

```cpp
desktop_hr_ = CallShellDesktop(false);
if (desktop_hr_ == S_OK) {
  desktop_shown_ = true;
}
```

2-1에서 본 대로 `S_OK`는 효과를 뜻하지 않습니다. 200밀리초 뒤에 도는 `kDesktopIconicTimerId`가 실제 결과를 재기는 하지만 **로그만 남기고 플래그를 되돌리지 않습니다.** 그래서 화면에는 Firefox가 그대로 있는데 우리 상태는 "바탕 화면이 보이는 중"이 됩니다.

다음 호버에서 `StartDesktopPeek`는 `desktop_shown_`이 참이므로 `HideDesktop`, 곧 `UndoMinimizeALL`을 부릅니다. **사용자가 바탕 화면을 기대한 자리에서 창을 되살리는 동작이 실행됩니다.**

### 2-3. 되살릴 창을 맨 `SetForegroundWindow`로 올려서 실패한다

```
15:54:13.927 [peek] restore hwnd=0000000000EF0FC6 zorder=1 fg=0
15:54:15.976 [peek] restore hwnd=0000000000950910 zorder=1 fg=0
```

`fg=0`은 `SetForegroundWindow`가 거부당했다는 뜻입니다. 우리 상단바는 `WS_EX_NOACTIVATE`이고 전경 창의 스레드에 붙어 있지 않으므로 Windows가 전경 전환을 막습니다.

같은 저장소의 `src/task_list.cpp`에 있는 `ActivateHwnd`는 이 문제를 이미 해결해 두었습니다. 전경 창 스레드에 `AttachThreadInput`으로 붙은 뒤 `BringWindowToTop`과 `SetForegroundWindow`를 부르고 다시 떼어 냅니다. **peek 경로만 그것을 쓰지 않고 맨손으로 불러서 실패하고 있었습니다.**

복원이 실패한 결과로 Z 순서 맨 위에 남은 것이 터미널이었고, 그것이 다음 줄에 그대로 찍혀 있습니다.

```
15:54:14.944 [peek] MinimizeAll ... fg_before=0000000000950910 cls=CASCADIA_HOSTING_WINDOW_CLASS
```

**사용자가 보고한 2번 증상은 2-1과 2-2와 2-3이 겹쳐서 생긴 것입니다.** 어긋난 플래그가 복원을 부르고, 복원이 실패해 터미널이 앞에 남았습니다.

### 2-4. 드래그 도중의 Ctrl 감지는 이미 동작하고 있다

```
15:52:33.201 [peek] probe hook=199 lastvk=162 async_c=0 lbtn=1 capture=00000000007A0E30
15:52:33.330 [peek] watch msg
15:52:33.330 [peek] watch on=1
15:52:33.393 [peek] probe hook=200 lastvk=162 lastmsg=0x00000100 async_c=1 async_l=1 lbtn=1
```

마우스 버튼을 누른 채(`lbtn=1`) Ctrl을 눌렀고, 훅 계수기가 199에서 200으로 오르고 `async_c`가 0에서 1로 바뀌었으며 코너 감시가 켜졌습니다(`watch on=1`).

**그러므로 1번 증상의 원인은 감지부가 아닙니다.** 감지는 되었는데 실행부가 2-1의 이유로 아무 일도 하지 않았을 개연성이 높습니다. 감지부를 다시 건드리지 마십시오. 새 실행부를 넣은 뒤 8-4에서 다시 재십시오.

### 2-5. 중간에 다른 창이 드러나는 이유

`UndoMinimizeALL`은 최소화되어 있던 창을 셸이 정한 순서로 한꺼번에 되살립니다. 그 애니메이션이 도는 동안 우리가 원하는 창은 아직 맨 앞이 아니고, 우리 복원은 200밀리초 뒤에야 실행됩니다. **그 사이가 그대로 보입니다.**

숨기는 쪽도 같습니다. 여러 창을 앞에서 뒤로 최소화하면 맨 앞 창이 먼저 사라지면서 그 아래 창이 차례로 드러납니다.

---

## 3. 새 설계의 원칙 세 가지

1. **상태를 기억하지 말고 잽니다.** 지금 바탕 화면이 드러나 있는지는 매번 창을 세어서 판정합니다. `desktop_shown_` 같은 자체 깃발을 두지 않습니다.
2. **셸에 맡기지 말고 우리가 최소화하고 복원합니다.** 어느 창을 건드렸는지 목록으로 들고 있으므로 되돌릴 대상이 분명합니다.
3. **Z 순서를 우리가 정합니다.** 뒤에 있는 창부터 최소화하면 맨 앞 창이 마지막까지 화면을 덮고 있어서 중간이 드러나지 않습니다. 복원도 뒤에서 앞으로 하면 원래 순서가 그대로 복구됩니다.

---

## 4. 걷어낼 것

`src/menu_bar.cpp`에서 다음을 **모두 지웁니다.** 하나도 남기지 마십시오.

**익명 네임스페이스의 함수와 자료**

| 이름 | 줄 (작업 시점) |
| --- | --- |
| `struct ComScope` | 33 |
| `int IconicOf(HWND)` | 59 |
| `void LogDesktopPeekResult(...)` | 63 |
| `volatile UINT g_hook_key_count` 외 둘 | 74~76 |
| `int KeyDownBit(SHORT)` | 71 |
| `struct CtrlProbeSnap` | 82 |
| `bool SameCtrlProbe(...)` | 96 |
| `void LogCtrlProbeIfChanged()` | 102 |
| `HRESULT CallShellDesktop(bool)` | 140 |

`ComScope`는 `CallShellDesktop`만 씁니다. **`src/autostart.cpp`와 `src/winx_menu.cpp`에도 같은 이름의 구조체가 따로 있으니 그쪽은 건드리지 마십시오.**

`LowLevelKeyboardProc`에서는 계수기 세 개에 대입하는 세 줄만 지웁니다. **Ctrl 눌림 전이에서 `kCornerWatchMsg`를 보내는 부분은 그대로 둡니다.** 2-4에서 정상 동작이 확인되었습니다.

**타이머 상수**

`kDesktopIconicTimerId`(8)와 `kDesktopIconicMs`를 지웁니다. `kDesktopPeekDwellTimerId`, `kCornerWatchTimerId`, `kCtrlPollTimerId`는 남깁니다.

**`MenuBar`의 멤버** (`src/menu_bar.hpp`)

`peek_latched_`를 뺀 나머지를 전부 지웁니다.

```
desktop_shown_, desktop_probe_, restore_target_, desktop_fg_before_,
desktop_hr_, desktop_iconic_before_, desktop_pending_undo_
```

`peek_dwell_armed_`, `last_corner_hit_`, `corner_watch_on_`, `peek_latched_`는 남깁니다.

**`MenuBar`의 메서드**

`ShowDesktop`과 `HideDesktop`을 지웁니다. `StartDesktopPeek`는 몸통을 새로 씁니다(7-2절). 나머지(`CornerHit`, `DesktopPeekWanted`, `UpdateDesktopPeek`, `StopDesktopPeek`, `StartCornerWatch`, `StopCornerWatch`)는 그대로 둡니다.

**메시지 처리**

`WM_TIMER`에서 `kDesktopIconicTimerId` 갈래 전체(624~641줄 근처)를 지웁니다. `kCtrlPollTimerId` 갈래에서는 `LogCtrlProbeIfChanged()` 호출만 지우고 예비 감시 시작은 남깁니다.

`WM_DESTROY`(1243~1246줄 근처)의 `KillTimer` 목록에서 `kDesktopIconicTimerId` 줄을 지웁니다.

---

## 5. 새 모듈 `src/desktop_toggle.hpp` / `src/desktop_toggle.cpp`

```cpp
#pragma once

#include <windows.h>

namespace bamti {

// Ctrl + 코너 호버로 부르는 바탕 화면 전환.
//
// 상태를 기억하지 않는다. 전환할 때마다 지금 화면에 무엇이 떠 있는지
// 세어서 방향을 정한다. 셸의 MinimizeAll 은 성공을 돌려주고도 아무 일도
// 하지 않는 경우가 있어서 쓰지 않는다.
class DesktopToggle {
 public:
  // 지금 눌러야 할 방향으로 한 번 전환한다.
  // 드러나 있으면 되살리고, 창이 떠 있으면 감춘다.
  void Toggle();

  // 창이 하나도 떠 있지 않은가.
  static bool Revealed();

 private:
  void Conceal();
  void Reveal();

  // Conceal 이 최소화한 창. Z 순서 앞에서 뒤 순서로 담는다.
  std::vector<HWND> concealed_;
};

}  // namespace bamti
```

`<vector>`를 포함하십시오.

### 5-1. `Revealed`

`CollectDesktopClearWindows()`(6절)가 빈 벡터를 돌려주면 참입니다. 그것뿐입니다.

### 5-2. `Conceal`

```
1. 목록 = CollectDesktopClearWindows()          // Z 순서 앞 -> 뒤
2. 목록이 비어 있으면 아무것도 하지 않고 끝낸다.
3. concealed_ = 목록
4. 뒤집은 사본을 만들어 HideHwnds 에 넘긴다.    // 뒤 -> 앞 순서로 최소화
5. 로그: conceal n=<개수> top=<맨 앞 창 클래스>
```

**4번의 뒤집기가 3절 세 번째 원칙이고 3번 증상의 핵심입니다.** `HideHwnds`는 받은 순서대로 최소화하므로, 뒤집어 넘겨야 맨 앞 창이 마지막에 사라집니다. 그때까지 그 창이 화면을 덮고 있어서 아래가 드러나지 않습니다.

`HideHwnds`는 `src/task_list.hpp`에 이미 있습니다. `SW_SHOWMINNOACTIVE`를 쓰고 이미 최소화된 창은 건너뜁니다. **새로 만들지 말고 그대로 쓰십시오.**

### 5-3. `Reveal`

```
1. concealed_ 가 비어 있으면 아무것도 하지 않고 끝낸다.
2. RestoreHwnds(concealed_) 를 부른다.
3. concealed_ 를 비운다.
4. 로그: reveal n=<개수>
```

`RestoreHwnds`(`src/task_list.cpp` 1703줄)는 받은 벡터를 **역순으로** 훑으면서 복원하고 `SetWindowPos(HWND_TOP, ..., SWP_NOACTIVATE)`로 올린 뒤, **마지막에 닿은 창을 `ActivateHwnd`로 활성화합니다.** `concealed_`가 앞에서 뒤 순서이므로 맨 뒤 창부터 복원되고 맨 앞이던 창이 마지막에 활성화됩니다. 원래 Z 순서가 그대로 돌아옵니다.

`ActivateHwnd`가 `AttachThreadInput`을 거치므로 2-3의 실패가 재현되지 않습니다. **`SetForegroundWindow`를 직접 부르지 마십시오.**

### 5-4. `Toggle`

```cpp
void DesktopToggle::Toggle() {
  if (Revealed()) {
    Reveal();
  } else {
    Conceal();
  }
}
```

`Revealed()`가 참인데 `concealed_`가 비어 있는 경우(우리가 감춘 것이 아니라 사용자가 Win+D를 눌렀거나 창을 전부 닫은 경우)에는 `Reveal`이 아무 일도 하지 않고 끝납니다. **그것이 맞는 동작입니다.** 우리가 모르는 창을 마음대로 되살리면 안 됩니다.

---

## 6. `task_list`에 창 수집 함수를 노출한다

`src/task_list.hpp`에 선언을 더합니다.

```cpp
// 바탕 화면을 드러내려면 치워야 하는 창. Z 순서 앞에서 뒤 순서다.
std::vector<HWND> CollectDesktopClearWindows();
```

몸통은 `src/task_list.cpp`에 둡니다. **거기에 `IsTaskWindow`, `IsCloaked`, `OnCurrentDesktop`, `SkipChromeExe`가 이미 있으므로 판정을 복제하지 마십시오.**

```
EnumWindows 로 훑으면서 다음을 모두 만족하는 창만 담는다.
  1. IsTaskWindow(hwnd) 가 참이다.
  2. IsIconic(hwnd) 가 거짓이다.
  3. IsWindowVisible(hwnd) 가 참이다.
  4. OnCurrentDesktop 이 참이다.
```

2번을 반드시 넣으십시오. **`IsTaskWindow`는 최소화된 창도 통과시킵니다**(독의 작업 목록에는 최소화된 창도 나와야 하기 때문입니다). 그것을 그대로 쓰면 이미 최소화된 창까지 대상에 들어가고 `Revealed()`가 영영 거짓이 됩니다.

`OnCurrentDesktop`은 `IVirtualDesktopManager`를 받습니다. `CollectDockApps`가 그것을 어떻게 만들어 쓰는지 보고 같은 방식으로 하십시오. 만들지 못하면 그 판정을 건너뛰고 나머지로 진행합니다(`OnCurrentDesktop`이 이미 `nullptr`에 참을 돌려줍니다).

`EnumWindows`는 Z 순서 위에서 아래로 열거하므로 담기는 순서가 곧 앞에서 뒤입니다. **정렬하지 마십시오.**

---

## 7. 트리거를 정리한다

### 7-1. 남기는 것

로그로 정상 동작이 확인된 것들입니다. **구조를 바꾸지 마십시오.**

- 저수준 훅의 Ctrl 눌림 전이 → `kCornerWatchMsg` → `StartCornerWatch`
- 200밀리초 예비 폴링(`kCtrlPollTimerId`)이 `GetAsyncKeyState(VK_CONTROL)`로 감시를 켜는 경로
- 30밀리초 코너 감시(`kCornerWatchTimerId`)
- 120밀리초 뜸(`kDesktopPeekDwellTimerId`)
- 걸쇠(`peek_latched_`)와 96 DIP 재장전 거리(`kPeekRearmZoneDip`)
- 14 DIP 코너 폭(`kPeekZoneDip`)

### 7-2. `StartDesktopPeek`의 몸통을 바꾼다

```cpp
void MenuBar::StartDesktopPeek() {
  if (peek_latched_) {
    return;
  }
  desktop_toggle_.Toggle();
  peek_latched_ = true;
}
```

`MenuBar`에 멤버 `DesktopToggle desktop_toggle_;`을 더합니다. **`ShowDesktop`과 `HideDesktop`을 부르는 자리는 남지 않습니다.**

### 7-3. 로그

새 모듈에서 남기는 줄은 전환 한 번에 한 줄입니다.

```
[peek] conceal n=7 top=MozillaWindowClass
[peek] reveal n=7
```

기존의 `watch on=`, `corner in=`, `dwell arm`, `dwell fire`, `unlatch reason=`은 그대로 둡니다. **`probe` 줄은 4절에서 걷어냈으므로 더 이상 남지 않아야 합니다.**

---

## 8. 검증

### 8-1. 빌드

Release 클린 빌드가 경고 없이 통과해야 합니다. `CMakeLists.txt`의 원본 목록과 `bamti.vcxproj` 양쪽에 `src/desktop_toggle.cpp`를 더하십시오.

### 8-2. 로그가 조용한지

bamti를 띄우고 아무것도 하지 않은 채 3분 두었을 때 `[peek]` 줄이 하나도 늘지 않아야 합니다. 탐침을 걷어냈으므로 이번에는 한 자리 수가 아니라 **영**이어야 합니다.

### 8-3. 상태 판정 (직접 확인 가능)

이 항목은 사용자 없이 확인할 수 있습니다. `Revealed()`가 창 개수만 보므로, 임시 진단 로그를 켜서 다음 두 상황의 반환값이 갈리는지 보십시오. **확인이 끝나면 그 진단 로그를 지우십시오.**

1. 창이 여럿 떠 있을 때 거짓
2. 전부 최소화했을 때 참

### 8-4. 사용자 확인으로 넘길 것

**이 환경에서는 마우스와 키보드 입력을 합성할 수 없습니다. 사용자 화면에 입력을 밀어 넣으려고 하지 마십시오.** 아래 네 가지는 지시서에 결과 기록란만 만들어 두고 사용자에게 넘기십시오.

1. Firefox를 맨 앞에 둔 채 Ctrl + 코너 → **바탕 화면이 드러나야 합니다.** 터미널이 올라오면 안 됩니다.
2. 이어서 다시 Ctrl + 코너 → **Firefox가 다시 맨 앞으로 돌아와야 합니다.**
3. 1번과 2번을 연달아 다섯 번 반복 → **매번 방향이 맞아야 합니다.** 예전에는 두 번째부터 어긋났습니다.
4. 창을 드래그하는 도중에 Ctrl을 눌러 코너로 → 전환되어야 합니다. **되지 않으면 그때의 `[peek]` 줄을 그대로 보고하십시오.** 2-4에서 감지는 확인되었으므로, 실패한다면 `conceal` 줄이 남았는데 화면이 안 바뀌는 것인지 `conceal` 줄 자체가 없는 것인지가 갈림길입니다.
5. 전환 도중에 중간 창이 드러나 보이는지 → 3절 세 번째 원칙이 먹혔는지 봅니다. **여전히 보인다면 이번 지시서 범위에서 더 손대지 말고 보고하십시오.** 창별 애니메이션 억제는 다음 단계이고 사용자가 판단합니다.

### 8-4 결과 기록

| # | 확인 | 결과 |
| --- | --- | --- |
| 1 | Firefox를 맨 앞에 둔 채 Ctrl + 코너 → 바탕 화면이 드러나고 터미널이 올라오지 않음 | 통과 (2026-09-06 21:53) |
| 2 | 이어서 다시 Ctrl + 코너 → Firefox가 맨 앞으로 돌아옴 | 통과 (2026-09-06 21:53) |
| 3 | 1번과 2번을 연달아 다섯 번 반복 → 매번 방향이 맞음 | 통과 (2026-09-06 16:45, 열두 번 연속) |
| 4 | 창을 드래그하는 도중에 Ctrl을 눌러 코너로 → 전환됨. 실패 시 그때의 `[peek]` 줄 | 통과 (2026-09-06 22:18). 실행부가 아니라 감지부가 원인이었고 `FIX-CTRL-HELD-DURING-DRAG.md`에서 고쳤습니다 |
| 5 | 전환 도중에 중간 창이 드러나 보이지 않음 | 감추는 쪽은 통과, **되살리는 쪽은 미해결**. `FIX-TOGGLE-TRANSITION-FLASH.md`와 `FIX-TRANSITION-LEAK-AND-ORDER.md`에서 다루었고 사용자 판단으로 닫았습니다 |

3번의 근거는 16:45 구간에서 `conceal n=4`와 `reveal n=4`가 열두 번 번갈아 나온 것입니다. 같은 구간에 `[task] activate failed`가 없어서, 예전 구현이 `restore ... fg=0`으로 전경 전환에 실패하던 것(2-3절)이 `ActivateHwnd` 경로로 해소되었음을 함께 확인했습니다.

4번과 5번은 화면으로만 판정할 수 있고 아직 시험하지 않았습니다.

---

## 9. 하지 말 것

- **`IShellDispatch`를 다시 쓰지 마십시오.** `MinimizeAll`, `UndoMinimizeALL`, `ToggleDesktop` 전부 이번 재구현에서 퇴출됩니다.
- **`SetForegroundWindow`를 직접 부르지 마십시오.** `ActivateHwnd`를 거칩니다.
- **`ShowWindow(SW_HIDE)`로 사용자 창을 감추지 마십시오.** 작업 표시줄에서 사라져 되살릴 길이 막힙니다. 최소화만 씁니다.
- **자체 상태 깃발을 새로 만들지 마십시오.** `concealed_` 목록은 되돌릴 대상을 적어 둔 것이지 화면 상태를 뜻하지 않습니다. 방향 판정은 언제나 `Revealed()`로 합니다.
- 감지부(훅, 예비 폴링, 감시 주기, 뜸, 걸쇠, 코너 폭, 재장전 거리)의 값과 구조를 바꾸지 마십시오.
- Raw Input을 등록하지 말고 `WH_MOUSE_LL`을 걸지 마십시오.
- 훅 안에서 로그를 부르지 마십시오.
- `SystemParametersInfo`로 시스템 전역 애니메이션 설정을 바꾸지 마십시오. 되돌리지 못하면 사용자 설정이 망가집니다.
- 레지스트리에 쓰는 확인 절차를 넣지 마십시오.
- 이번 범위 밖의 파일을 정리하거나 이름을 바꾸지 마십시오.

**빌드하려고 실행 중인 bamti를 종료했다면, 끝난 뒤 반드시 다시 띄워 놓으십시오.**
