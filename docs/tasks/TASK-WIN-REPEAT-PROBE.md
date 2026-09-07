# 작업 지시서: 키보드 자동 반복이 저수준 훅에 닿는지 재고, 닿으면 Win 상태 초기화를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-WORKAREA-GUARD-AND-WIN-STALE.md` 와 `FIX-WIN-EXPIRED-EATS-NEXT-PRESS.md` 를 마무리하면서 남겨 둔 미확인 사항입니다. 건드리는 파일은 `src/menu_bar.cpp` 하나입니다.

**이 지시서는 두 단계입니다. 먼저 재고, 잰 값이 결함을 가리킬 때에만 고칩니다. 재기 전에 3절의 수정을 먼저 넣지 마십시오.**

---

## 1. 무엇을 걱정하고 있는가

`LowLevelKeyboardProc` 의 Win 누름 갈래입니다.

```cpp
  if (is_win) {
    if (down) {
      if (!g_win_held) {
        g_win_down_tick = GetTickCount64();
        g_win_expired = false;
      }
      g_win_held = true;
      g_win_combo = false;
      g_win_injected = false;
      g_win_vk = vk;
      return 1;
    }
```

`g_win_combo` 와 `g_win_injected` 와 `g_win_vk` 는 `if (!g_win_held)` 밖에 있으므로 **누름 이벤트가 올 때마다 조건 없이 지워집니다.** 키보드 자동 반복이 이 훅에 닿는다면, 사용자가 Win 을 누른 채로 있는 동안 반복 누름이 계속 들어와 조합 기억이 지워집니다.

지워졌을 때 무슨 일이 벌어지는지가 문제입니다. 사용자가 Win 을 누른 채 `E` 를 치면 조합 갈래가 `g_win_combo` 와 `g_win_injected` 를 세우고 `InjectWinCombo` 로 **Win 누름을 주입합니다.** 이 주입한 누름은 진짜 Win 뗌이 왔을 때 `InjectWinKey(g_win_vk, true)` 로 되돌리는 것이 유일한 통로입니다. 그런데 반복이 기억을 지워 버리면 뗌 갈래에서 다음이 됩니다.

1. `g_win_combo` 가 거짓이므로 되돌림이 실행되지 않습니다. **주입해 둔 Win 누름이 시스템 수준에 그대로 남습니다.** 진짜 뗌은 `return 1` 로 삼켜지므로 시스템은 뗌을 영영 보지 못합니다.
2. 같은 이유로 `else` 갈래가 실행되어 `kToggleStartMsg` 가 갑니다. **조합키를 쓴 뒤인데 시작 메뉴가 열립니다.**

`ExpireStaleWin` 도 이 상황을 구하지 못합니다. 뗌 갈래에서 `g_win_held` 가 이미 거짓이 되었으므로 만료 판정에 걸리지 않습니다.

**다만 자동 반복이 이 훅에 닿는지를 아직 재지 않았습니다.** 닿지 않는다면 위의 이야기는 전부 일어나지 않으므로 고칠 것이 없습니다. 그래서 재는 일이 먼저입니다.

## 2. 재는 방법

### 2.1 훅 안에서는 로그를 부르지 않는다

`WH_KEYBOARD_LL` 콜백은 `LowLevelHooksTimeout`(기본 300밀리초) 안에 끝나야 하고, 넘기면 시스템이 훅을 통째로 떼어 냅니다. 파일 로그는 뮤텍스를 잡으므로 이 안에서 부르면 안 됩니다. 이미 같은 파일에 규약이 있습니다. `ExpireStaleWin` 은 `g_win_stale_count` 를 올리기만 하고, 실제 기록은 1초짜리 `kClockTimerId` 타이머가 값이 바뀌었을 때만 남깁니다(`src/menu_bar.cpp:639`). **그 규약을 그대로 따르십시오.**

### 2.2 셈할 것

전역 변수 자리(`g_win_stale_logged` 근처)에 다음을 더하십시오.

```cpp
UINT g_key_repeat_count = 0;    // 어떤 키든 눌린 채로 다시 온 누름
UINT g_win_repeat_count = 0;    // Win 키가 눌린 채로 다시 온 누름
UINT g_win_repeat_combo = 0;    // 그 가운데 조합 기억이 살아 있던 횟수
UINT g_key_repeat_logged = 0;
UINT g_win_repeat_logged = 0;
UINT g_win_repeat_combo_logged = 0;
DWORD g_last_down_vk = 0;       // 아직 뗌을 보지 못한 마지막 누름의 가상 키 코드
```

`KBDLLHOOKSTRUCT` 에는 반복 횟수를 알려 주는 필드가 없습니다. **어떤 플래그로 반복을 판정하려 하지 마십시오.** 같은 키의 누름이 뗌 없이 다시 오는 것으로만 판정할 수 있습니다.

### 2.3 훅에 넣을 셈

`LowLevelKeyboardProc` 안, 주입된 이벤트를 걸러 내는 `if` 를 지난 뒤이면서 `is_win` 판정보다 앞인 자리에 넣으십시오. `ExpireStaleWin();` 호출 바로 앞이 알맞습니다.

```cpp
  if (down) {
    if (g_last_down_vk == vk) {
      ++g_key_repeat_count;
    }
    g_last_down_vk = vk;
  } else if (up && g_last_down_vk == vk) {
    g_last_down_vk = 0;
  }
```

Win 갈래의 셈은 기억을 지우기 **전에** 세어야 합니다. 누름 갈래를 다음처럼 두십시오. **이번 단계에서는 지우는 자리를 옮기지 않습니다.**

```cpp
    if (down) {
      if (!g_win_held) {
        g_win_down_tick = GetTickCount64();
        g_win_expired = false;
      } else {
        ++g_win_repeat_count;
        if (g_win_combo) {
          ++g_win_repeat_combo;
        }
      }
      g_win_held = true;
      g_win_combo = false;
      g_win_injected = false;
      g_win_vk = vk;
      return 1;
    }
```

### 2.4 타이머에서 찍기

`kClockTimerId` 갈래의 `win stale expired` 기록 바로 뒤에 붙이십시오.

```cpp
        if (g_key_repeat_count != g_key_repeat_logged || g_win_repeat_count != g_win_repeat_logged ||
            g_win_repeat_combo != g_win_repeat_combo_logged) {
          g_key_repeat_logged = g_key_repeat_count;
          g_win_repeat_logged = g_win_repeat_count;
          g_win_repeat_combo_logged = g_win_repeat_combo;
          Log(L"bar", L"key repeat any=%u win=%u win_combo=%u", g_key_repeat_count, g_win_repeat_count,
              g_win_repeat_combo);
        }
```

`MenuBar::RemoveWinHook()` 이 다른 상태를 되돌리는 자리에 `g_last_down_vk = 0;` 도 함께 넣으십시오. 셈한 값은 되돌리지 마십시오. 누적값이라야 사용자가 여러 번 시도한 결과를 한 번에 볼 수 있습니다.

## 3. 사용자에게 부탁할 확인

**이 환경에서는 사용자 화면에 입력을 합성할 수 없습니다.** 게다가 훅이 `LLKHF_INJECTED` 가 붙은 이벤트를 그대로 흘려보내므로, `SendInput` 으로 만든 키는 애초에 이 갈래에 닿지 않아 재현 수단이 되지 못합니다. 새 빌드를 실행해 놓고 다음 세 가지를 사용자에게 부탁하십시오.

1. 메모장처럼 글자를 받는 창에서 아무 글자 키나 **3초 동안 누르고 계십시오.** 글자가 죽 이어져 나오는 것을 확인하고 떼십시오.
2. Win 키를 **3초 동안 누르고 계시다가** 떼십시오. 시작 메뉴가 열리는지 함께 알려 주십시오.
3. Win 키를 누른 채로 `E` 를 한 번 치고, **Win 키를 3초 더 누르고 계시다가** 떼십시오. 파일 탐색기가 열리는지, 시작 메뉴가 함께 열리는지, 그 뒤에 다른 키가 Win 조합처럼 동작하지는 않는지 알려 주십시오.

세 번째에서 Win 이 굳으면 잠금 화면으로 갔다가 돌아오면 풀립니다(`WM_WTSSESSION_CHANGE` 가 `ReleaseHeldWin` 을 부릅니다). 부탁하기 전에 이 되돌리는 방법을 사용자에게 미리 알려 드리십시오.

그 뒤 `~/.bamti/bamti.log` 에서 `key repeat` 이 들어간 줄을 모두 뽑아 보고하십시오.

## 4. 잰 값으로 판정하고, 그 뒤에 고칠 것

| 관측 | 결론과 다음 행동 |
| --- | --- |
| `any=0` 이고 `win=0` 이다 | 자동 반복은 이 훅에 닿지 않습니다. **고치지 말고** 탐침만 남긴 채 그렇게 보고하십시오 |
| `any` 는 오르는데 `win=0` 이다 | 글자 키는 반복하지만 Win 키는 반복하지 않습니다. 이 경우에도 **고치지 마십시오.** 다만 사실을 분명히 보고하십시오 |
| `win` 이 오른다 | 걱정하던 경로가 실제로 열려 있습니다. 아래 수정을 넣으십시오 |
| `win_combo` 가 오른다 | 조합 기억이 실제로 지워지고 있습니다. 아래 수정이 반드시 필요합니다 |

`win` 이 오르는 것이 확인되었을 때에만 누름 갈래를 다음으로 바꾸십시오.

```cpp
    if (down) {
      if (!g_win_held) {
        g_win_down_tick = GetTickCount64();
        g_win_expired = false;
        g_win_combo = false;
        g_win_injected = false;
        g_win_vk = vk;
      } else {
        ++g_win_repeat_count;
        if (g_win_combo) {
          ++g_win_repeat_combo;
        }
      }
      g_win_held = true;
      return 1;
    }
```

근거를 적어 둡니다. 한 번의 누름 주기 안에서 "조합을 썼다"는 사실과 "Win 누름을 주입했다"는 사실은 주기가 끝날 때까지 유지되어야 합니다. `g_win_vk` 도 첫 누름의 것을 지켜야 주입한 키와 되돌리는 키가 어긋나지 않습니다. `g_win_down_tick` 을 반복마다 새로 적지 않는 지금 동작은 그대로 두십시오. 오래 누르고 있는 상태를 만료가 알아보려면 처음 누른 시각이 남아 있어야 합니다.

한 가지 부작용을 알고 넘어가십시오. 이 수정 뒤에는 LWin 을 누른 채로 RWin 을 눌러도 새 주기로 보지 않고 `g_win_vk` 가 LWin 으로 남습니다. 두 Win 키를 겹쳐 쓰는 경우는 드물고, 주입한 키를 그대로 되돌리는 쪽이 굳는 것보다 안전하므로 이렇게 둡니다.

수정을 넣었다면 3절의 세 가지를 사용자에게 **한 번 더** 부탁해서, `win` 은 계속 오르지만 `win_combo` 는 더 이상 오르지 않는지 확인하십시오.

## 5. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

2. `LowLevelKeyboardProc` 안에 `Log` 호출이 하나도 없어야 합니다. 이것은 눈으로 확인하고 보고하십시오.
3. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
4. 사용자 확인의 결과와 `key repeat` 로그 줄을 함께 보고하십시오. 확인하지 못한 항목은 확인하지 못했다고 적으십시오.

## 6. 하지 말 것

- 재기 전에 3절의 수정을 먼저 넣지 마십시오. `win` 이 오르지 않는다면 지금 코드가 옳습니다.
- 훅 콜백 안에서 `Log` 를 부르지 마십시오. 훅이 시간 제한을 넘겨 떨어져 나갑니다.
- 자동 반복을 `KBDLLHOOKSTRUCT` 의 `flags` 나 `dwExtraInfo` 로 판정하려 하지 마십시오. 그런 정보가 들어 있지 않습니다.
- `g_win_expired` 를 없애거나 `ReleaseHeldWin` 안에서 세우지 마십시오. `FIX-WIN-EXPIRED-EATS-NEXT-PRESS.md` 에서 이미 정리한 부분입니다.
- 사용자 화면에 키 입력을 합성하지 마십시오.
- `src/menu_bar.cpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
