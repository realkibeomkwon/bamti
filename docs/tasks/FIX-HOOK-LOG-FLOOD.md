# 작업 지시서: 저수준 훅에서 로그를 걷어내고 Ctrl 자동 반복을 무시한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-CORNER-DRAG-CTRL.md`(커밋 `2bfe4c7`)의 후속입니다. 건드리는 파일은 `src/menu_bar.cpp`와 `src/menu_bar.hpp`입니다.

사용자가 "처음엔 안 되다가 계속 시도하니 된다", "화면 전환 뒤에 클릭이 안 될 때가 있다", "안정성이 떨어진다"고 보고했습니다.

---

## 1. 측정: 훅이 초당 약 50번 로그를 쓴다

사용자가 Ctrl을 약 1.8초 누르고 있던 구간의 로그입니다.

```
09:23:05.455 [peek] ctrl vk=162 wparam=0x00000100 flags=0x00000000 async=0x8000
09:23:05.484 [peek] ctrl vk=162 wparam=0x00000100 flags=0x00000000 async=0x8000
09:23:05.530 [peek] ctrl vk=162 wparam=0x00000100 flags=0x00000000 async=0x8000
... (약 31밀리초 간격으로 계속)
09:23:07.280 [peek] ctrl vk=162 wparam=0x00000101 flags=0x00000080 async=0x8001
```

**같은 구간에서 `ctrl` 줄이 101개입니다.** `wparam=0x100`은 `WM_KEYDOWN`이고 `flags=0`이므로 전부 눌림입니다. **키보드 자동 반복입니다.**

`LowLevelKeyboardProc`은 눌림과 뗌마다 `Log`를 부르는데, `src/log.cpp`의 `Log`가 하는 일은 가볍지 않습니다.

```cpp
  OutputDebugStringW(line);
  std::lock_guard lock(g_lock);
  ...
  WriteLogFile(line);
```

`OutputDebugStringW`는 시스템 전역 뮤텍스를 잡고, 그 뒤에 잠금을 건 파일 쓰기가 이어집니다. **저수준 키보드 훅 프로시저는 훅을 건 스레드, 곧 상단바의 UI 스레드에서 불립니다.** 그러므로 Ctrl을 누르고 있는 동안 UI 스레드가 초당 약 50번 전역 뮤텍스와 파일 쓰기에 묶입니다.

같은 스레드가 30밀리초 코너 감시 타이머를 돌리고 상단바를 그려야 합니다. **타이머 틱을 놓치면 코너에 120밀리초 머문 것을 못 알아채고, 그것이 "처음엔 안 된다"로 나타납니다.**

훅이 벗겨지는 것은 아닙니다. 이 컴퓨터의 `HKCU\Control Panel\Desktop\LowLevelHooksTimeout`이 8000이므로 시간 초과로 제거될 여지는 없습니다. **문제는 훅이 사라지는 것이 아니라 UI 스레드가 막히는 것입니다.**

또한 자동 반복마다 `PostMessageW(kCornerWatchMsg)`도 함께 나갑니다. `StartCornerWatch`가 걸러 주므로 해롭지는 않지만 불필요합니다.

---

## 2. 고칠 것

### 2-1. 훅 안에서 로그를 부르지 않는다

**`LowLevelKeyboardProc` 안의 `Log` 호출을 지우십시오.** 이 함수 안에서는 어떤 로그도 남기지 마십시오.

진단이 필요하면 창 절차에서 남깁니다. `kCornerWatchMsg`를 받은 자리에서 한 줄 남기면 충분하고, 그 자리는 자동 반복이 걸러진 뒤라 드물게 불립니다.

### 2-2. 자동 반복을 무시한다

훅에 Ctrl의 눌림 상태를 기억하는 정적 변수를 두고 **올라감에서 내려감으로 바뀌는 순간에만** 메시지를 보내십시오.

```cpp
bool g_ctrl_held = false;
```

- 눌림인데 `g_ctrl_held`가 거짓이면 참으로 바꾸고 `PostMessageW(..., kCornerWatchMsg, 1, 0)`
- 눌림인데 이미 참이면 아무것도 하지 않는다 (자동 반복)
- 뗌이면 `g_ctrl_held`를 거짓으로 되돌린다. **메시지는 보내지 않습니다.** 감시를 끝내는 판단은 지금처럼 감시 타이머가 `GetAsyncKeyState`로 합니다.

`RemoveWinHook`에서 `g_ctrl_held`를 거짓으로 되돌리십시오. 다른 전역 상태를 정리하는 자리와 같습니다.

**Ctrl을 삼키지 않는 것은 그대로 지키십시오.** 반드시 `CallNextHookEx`의 반환값으로 빠져나가야 합니다.

### 2-3. 나머지 진단 로그는 그대로 둔다

`corner in=`, `dwell arm`, `dwell fire`, `MinimizeAll`, `UndoMinimizeALL`, `unlatch reason=`은 전이에서만 남으므로 그대로 두십시오. 이번 진단에 실제로 쓰였습니다.

---

## 3. 클릭이 안 되는 것을 재려면 무엇이 필요한가

전환 뒤에 클릭이 안 될 때가 있다는 보고는 아직 원인을 모릅니다. **1절의 UI 스레드 막힘이 사라진 뒤에 다시 재야 합니다.** 지금 상태로는 무엇을 보든 그 영향이 섞입니다.

다만 다음 판정에 필요한 것을 미리 넣으십시오. `ShowDesktop`과 `HideDesktop`이 200밀리초 뒤에 남기는 줄에 **전경 창**을 함께 적습니다.

```cpp
Log(L"peek", L"... fg_before=%p fg_after=%p cls=%s", ...);
```

`fg_before`는 호출 직전의 `GetForegroundWindow`, `fg_after`는 200밀리초 뒤의 값, `cls`는 `fg_after`의 창 클래스 이름(`GetClassNameW`)입니다.

`UndoMinimizeALL` 뒤에 전경이 `WorkerW`나 `Progman`(바탕 화면)으로 남아 있다면, 창이 복원됐지만 활성화되지 않아 **첫 클릭이 활성화에 먹히는 것**입니다. 그 경우 원인이 확정되므로 그때 다시 판단합니다.

**이번 작업에서 활성화를 강제하지는 마십시오.** `SetForegroundWindow`를 부르는 코드를 넣지 마십시오. 원인이 확정되지 않았고, 창 활성화를 억지로 뺏는 것은 부작용이 큽니다.

---

## 4. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다.
2. **Ctrl+C와 Ctrl+V가 그대로여야 합니다.** 훅을 건드리므로 이것부터 확인하십시오.
3. **`LowLevelKeyboardProc` 안에 `Log` 호출이 하나도 남아 있지 않아야 합니다.** 코드로 확인하십시오.
4. **자동 반복이 걸러지는지.** Ctrl을 몇 초 누르고 있어도 `kCornerWatchMsg` 관련 로그가 한 줄만 남아야 합니다. 이전에는 초당 약 50줄이었습니다.
5. 3절의 전경 창 항목이 `MinimizeAll`과 `UndoMinimizeALL` 줄에 나와야 합니다.
6. 나머지(전환 안정성, 클릭 문제)는 사용자 확인으로 넘기십시오.

이 환경에서는 마우스와 키보드 입력을 합성할 수 없습니다. 사용자 화면에 입력을 밀어 넣으려고 하지 마십시오.

**빌드하려고 실행 중인 bamti를 종료했다면, 끝난 뒤 반드시 다시 띄워 놓으십시오.** 상단바와 독이 사라진 채로 두면 사용자가 컴퓨터를 쓰지 못합니다.

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
