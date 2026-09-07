# 작업 지시서: Win 조합 단축키가 셸에 전달되지 않는 문제를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/menu_bar.cpp` 하나입니다.

**이 지시서의 1절은 코드를 읽고 세운 가설이며, 아직 측정으로 확정하지 않았습니다.** 2절의 측정을 먼저 수행하고, 그 결과에 따라 3절이나 5절로 가십시오. 측정하지 않고 바로 고치면 안 됩니다.

---

## 1. 무엇이 원인이라고 보는가

증상은 bamti 를 실행하면 Win 키 조합 단축키가 듣지 않는 것입니다. 창 배치 단축키인 `Win+방향키`, bamti 자신의 `Win+Space` 가 모두 보고되었습니다.

`src/menu_bar.cpp` 의 `LowLevelKeyboardProc`(219행부터)가 Win 키를 다음과 같이 다룹니다.

1. Win 키 down 이 오면 상태만 기록하고 `return 1` 로 **항상 삼킵니다**(258행부터). 시작 메뉴가 저절로 열리는 것을 막기 위한 처리입니다. 이 시점에 운영 체제는 Win 키가 눌렸다는 사실을 알지 못합니다.
2. Win 키를 누른 채 다른 키가 눌리면(276행부터) `InjectWinKey(g_win_vk, false)` 로 Win down 을 **주입한 뒤**, 원본 조합키는 `CallNextHookEx` 로 **그대로 통과시킵니다**(297행).

문제는 2번의 순서입니다. `SendInput` 으로 주입한 Win down 은 입력 큐를 한 바퀴 돌아야 반영되는데, 원본 조합키는 지금 훅이 처리 중인 이벤트이므로 훅에서 반환하는 즉시 전달됩니다. **주입한 Win down 보다 조합키가 먼저 처리되면, 셸은 Win 이 눌리지 않은 상태에서 방향키만 받습니다.** `Win+방향키` 는 explorer 가 등록한 셸 단축키이므로 이 순서에 민감합니다.

`Win+Space` 는 경로가 다릅니다. bamti 가 281행에서 직접 삼키고 `kToggleSpotlightMsg` 를 보냅니다. 이쪽이 듣지 않는다면 원인이 위와 다르므로 2절에서 따로 확인해야 합니다.

---

## 2. 먼저 측정한다

### 2-1. 훅에 진단 로그를 넣는다

`LowLevelKeyboardProc` 안에 임시 로그를 넣으십시오. **저수준 키보드 훅은 `LowLevelHooksTimeout`(기본 300밀리초) 안에 반환해야 하므로, 로그를 무제한으로 남기면 훅이 시스템에 의해 제거됩니다.** 반드시 횟수를 제한하십시오.

```cpp
// 임시 진단. 측정이 끝나면 지운다.
static int g_diag_count = 0;
```

다음 네 지점에서만 남기고, 각각 스무 번까지만 남기십시오.

| 지점 | 남길 내용 |
| --- | --- |
| 훅 진입 직후(`code == HC_ACTION`) | `vk`, `down`/`up`, `injected` 여부 |
| `win_key_enabled()` 가 false 여서 빠져나갈 때 | `fullscreen_occluded_` 때문임을 알 수 있게 |
| Win 조합의 첫 키를 만나 주입할 때 | `vk`, `g_win_vk`, `SendInput` 반환값 |
| `VK_SPACE` 분기에 들어갈 때 | `extra_mod` 값 |

`SendInput` 의 반환값을 반드시 남기십시오. 0 이면 주입 자체가 실패한 것이고, 그때는 원인이 순서 문제가 아니라 권한 문제입니다.

### 2-2. 무엇을 확인하는가

Release 빌드로 bamti 를 실행하고 다음을 각각 눌러 로그를 확인하십시오.

1. **`Win` 단독** — 시작 메뉴가 열리는가. 열리지 않으면 훅이 아예 걸리지 않은 것이므로 `InstallWinHook()` 의 반환값부터 확인하십시오(`SetWindowsHookExW` 실패 시 `GetLastError`).
2. **`Win+왼쪽 방향키`** — 훅 진입 로그가 남는가. 남는데 창이 움직이지 않으면 1절의 가설이 맞습니다. 진입 로그조차 없으면 훅이 제거된 것입니다.
3. **`Win+Space`** — spotlight 가 열리는가. `extra_mod` 가 true 로 찍히면 다른 수정 키가 눌린 것으로 잘못 읽힌 것입니다.
4. **`Win+E`, `Win+R`, `Win+D`** — 방향키만의 문제인지 조합 전체의 문제인지 가릅니다.

### 2-3. 분기

- 훅 진입 로그가 남고 `SendInput` 이 1 을 반환하는데도 셸이 반응하지 않으면 **3절로 가십시오.**
- 훅 진입 로그가 남지 않으면 **5절로 가십시오.**
- `SendInput` 이 0 을 반환하면 **6절로 가십시오.**

---

## 3. 조합의 첫 키를 원본 대신 주입한 쌍으로 대체한다

Win down 과 조합키 down 을 **하나의 `SendInput` 배열로 함께 주입**하고, 원본 조합키는 삼킵니다. 한 번의 `SendInput` 호출에 담긴 입력은 다른 입력이 사이에 끼어들지 않은 채 순서대로 큐에 들어가므로, 셸이 조합키를 볼 때에는 Win 이 이미 눌린 상태입니다.

### 3-1. 주입 함수를 더한다

`InjectWinKey`(206행) 바로 아래에 다음 함수를 더하십시오.

```cpp
bool InjectWinCombo(DWORD win_vk, const KBDLLHOOKSTRUCT& key) {
  INPUT in[2]{};
  in[0].type = INPUT_KEYBOARD;
  in[0].ki.wVk = static_cast<WORD>(win_vk);
  if (win_vk == VK_RWIN) {
    in[0].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  in[1].type = INPUT_KEYBOARD;
  in[1].ki.wVk = static_cast<WORD>(key.vkCode);
  in[1].ki.wScan = static_cast<WORD>(key.scanCode);
  if ((key.flags & LLKHF_EXTENDED) != 0) {
    in[1].ki.dwFlags |= KEYEVENTF_EXTENDEDKEY;
  }
  return SendInput(2, in, sizeof(INPUT)) == 2;
}
```

**`scanCode` 와 확장 키 플래그를 반드시 옮겨 담으십시오.** 방향키는 확장 키이므로 이 플래그가 빠지면 숫자판의 같은 자리 키로 해석됩니다.

### 3-2. 훅의 조합 분기를 바꾼다

`LowLevelKeyboardProc` 의 289행부터를 다음과 같이 바꾸십시오.

```cpp
    if (!g_win_combo) {
      g_win_combo = true;
      g_win_injected = true;
      if (InjectWinCombo(g_win_vk, *info)) {
        // 주입한 쌍이 원본을 대신하므로 원본은 삼킨다.
        return 1;
      }
      // 주입이 실패하면 예전처럼 원본을 통과시킨다. Win 이 빠진 채로라도 키는 살린다.
      InjectWinKey(g_win_vk, false);
    }
```

바뀌는 것은 **조합의 첫 키 하나뿐입니다.** 두 번째 키부터는 `g_win_combo` 가 이미 true 이므로 지금처럼 그대로 통과합니다. 그 시점에는 주입한 Win 이 실제로 눌려 있으므로 정상으로 동작합니다. 키를 누른 채로 두어 자동 반복이 일어나는 경우도 같습니다.

### 3-3. 짝이 맞는지 확인할 것

주입한 조합키의 down 에 대응하는 up 은 **원본이 그대로 통과합니다.** 원본 up 을 추가로 삼키거나 주입하지 마십시오. 그렇게 하면 키가 눌린 채로 남습니다.

주입한 입력에는 `LLKHF_INJECTED` 가 붙으므로 훅이 224행에서 무시합니다. 이 조기 반환을 지우지 마십시오. 지우면 주입이 자기 자신을 다시 처리하여 무한히 반복됩니다.

---

## 4. 검증

### 4-1. 빌드

Release 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

### 4-2. 창 배치가 실제로 일어나는지 재는 방법

**사용자 화면에 입력을 합성하지 마십시오.** 키를 누르는 것은 사용자에게 부탁하고, 여러분은 결과만 재십시오.

창 위치는 `GetForegroundWindow` 와 `GetWindowRect` 로 잽니다. `Win+왼쪽 방향키` 를 누르기 전과 누른 뒤의 `RECT` 를 비교하십시오.

**판정 기준은 반환값이나 로그가 아니라 창의 좌표입니다.** 화면 너비가 2560 인 지금 환경에서 `Win+왼쪽 방향키` 뒤의 `right` 는 1280 부근이어야 합니다.

### 4-3. 확인할 조합

| 조합 | 기대 동작 |
| --- | --- |
| `Win` 단독 | bamti 시작 메뉴가 열린다 |
| `Win+왼쪽`, `Win+오른쪽` | 창이 화면 절반으로 배치된다 |
| `Win+위`, `Win+아래` | 창이 최대화되고 복원된다 |
| `Win+Space` | spotlight 가 열린다 |
| `Win+E` | 파일 탐색기가 열린다 |
| `Win+D` | 바탕 화면이 드러난다 |
| `Win+Shift+왼쪽` | 다른 모니터로 옮겨진다. 수정 키가 하나 더 붙은 경우를 확인하는 항목이다 |

### 4-4. 자동 반복

`Win` 을 누른 채 방향키를 **연달아 여러 번** 누르십시오. 첫 번째만 듣고 두 번째부터 듣지 않는다면 `g_win_combo` 를 첫 키에서만 세우는 지금 구조가 남긴 문제이므로 다시 보고하십시오.

### 4-5. 진단 로그를 지운다

검증이 끝나면 **2절에서 넣은 임시 로그를 모두 지우십시오.** 저수준 훅에 남은 로그는 입력 지연으로 이어집니다.

---

## 5. 훅 진입 로그조차 남지 않는 경우

훅이 걸리지 않았거나 시스템이 제거한 것입니다. 다음 순서로 확인하십시오.

1. `MenuBar::InstallWinHook()`(2162행)의 반환값을 로그로 남기십시오. `SetWindowsHookExW` 가 실패하면 `GetLastError` 를 함께 남깁니다.
2. `HKEY_CURRENT_USER\Control Panel\Desktop` 의 `LowLevelHooksTimeout` 값을 **읽기만** 하십시오. **레지스트리에 쓰지 마십시오.** 값이 없으면 기본 300밀리초입니다.
3. 훅 콜백 안에서 시간이 오래 걸리는 호출을 하고 있는지 확인하십시오. 지금 콜백은 `GetAsyncKeyState` 와 `PostMessageW` 만 쓰므로 문제가 없어야 합니다.
4. 이 경우 원인이 1절의 가설과 다르므로, **3절을 적용하지 말고 측정 결과를 보고하십시오.**

---

## 6. `SendInput` 이 0 을 반환하는 경우

다른 프로세스가 주입을 막고 있거나, 권한이 더 높은 창이 포그라운드에 있는 것입니다. 이때는 코드로 풀 수 없습니다. 어떤 창이 포그라운드였는지(`GetForegroundWindow` 의 클래스 이름과 실행 파일)를 함께 보고하십시오.

---

## 7. 건드리지 말 것

- Ctrl 키 처리(232행부터)와 `kCornerWatchMsg` 전송을 바꾸지 마십시오. 핫 코너 기능이 여기에 걸려 있습니다.
- `g_swallow_space` 로 Space 의 up 을 삼키는 처리(275행)를 지우지 마십시오. 이것을 지우면 spotlight 를 연 뒤 Space 가 그 아래 창으로 새어 들어갑니다.
- `win_key_enabled()` 의 정의(`src/menu_bar.hpp` 43행)를 바꾸지 마십시오. 전체 화면 앱에서 Win 가로채기를 놓는 것은 의도한 동작입니다.
- Win 키 down 을 삼키는 처리 자체를 없애지 마십시오. 그렇게 하면 조합을 쓸 때마다 시작 메뉴가 함께 열립니다.
- 시작 메뉴와 spotlight 의 토글 로직(`ToggleStartMenu`, `ToggleSpotlight`)을 건드리지 마십시오.
