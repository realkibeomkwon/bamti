# 작업 지시서: 바탕 화면에서 돌아올 때 원래 맨 앞이던 창을 되살린다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-CORNER-ZONE-WIDTH.md`(커밋 `5261c34`)의 후속입니다. 건드리는 파일은 `src/menu_bar.cpp`와 `src/menu_bar.hpp`입니다.

---

## 1. 측정: 복귀 뒤 전경 창이 바뀐다

사용자가 "터미널을 맨 위에 두었는데 돌아오니 브라우저가 맨 위에 있다"고 보고했습니다. 로그가 그대로 보여 줍니다.

```
10:54:56.655 UndoMinimizeALL ... fg_before=0x10122   fg_after=0x330F26 cls=MozillaWindowClass
10:55:04.165 UndoMinimizeALL ... fg_before=0x330F26  fg_after=0x10122  cls=CASCADIA_HOSTING_WINDOW_CLASS
10:55:12.615 UndoMinimizeALL ... fg_before=0x10122   fg_after=0x330F26 cls=MozillaWindowClass
```

`0x10122`가 Windows 터미널(`CASCADIA_HOSTING_WINDOW_CLASS`), `0x330F26`이 Firefox(`MozillaWindowClass`)입니다.

**`fg_before`와 `fg_after`가 다릅니다.** 그리고 어느 쪽이 올라올지 일정하지도 않습니다. 세 번 중 두 번은 터미널에서 Firefox로, 한 번은 그 반대로 바뀌었습니다.

`IShellDispatch::UndoMinimizeALL`은 최소화했던 창들을 되살릴 뿐이고 **어느 창이 활성 상태였는지는 복원하지 않습니다.** 마지막으로 되살아난 창이 앞으로 나옵니다.

우리가 `MinimizeAll`을 부르기 직전의 전경 창을 이미 기억하고 있으므로, 되살린 뒤에 그 창을 다시 앞으로 보내면 됩니다.

---

## 2. 왜 Win+D를 흉내 내지 않는가

Windows의 진짜 바탕 화면 보기(Win+D)는 활성 창까지 제대로 되돌립니다. `SendInput`으로 Win+D를 주입하면 그 동작을 그대로 얻을 수 있습니다.

**그러나 쓰면 안 됩니다.** 이 기능은 사용자가 **Ctrl을 누르고 있는 동안** 발동합니다. 그 상태에서 Win+D를 주입하면 시스템은 `Ctrl+Win+D`를 보고, 이것은 Windows 11에서 **새 가상 데스크톱 만들기**입니다. 누를 때마다 가상 데스크톱이 하나씩 늘어납니다.

Ctrl을 잠깐 떼었다가 다시 누르도록 주입하는 방법도 있지만, 사용자가 실제로 누르고 있는 키의 상태를 우리가 조작하는 것이라 부작용을 예측할 수 없습니다. **하지 마십시오.**

---

## 3. 고칠 것

### 3-1. 되살릴 창을 따로 기억한다

지금 `desktop_probe_`는 로그를 남기려고 매번 덮어쓰는 값이라 이 용도로 쓸 수 없습니다. 전용 멤버를 하나 두십시오.

```cpp
HWND restore_target_ = nullptr;
```

`ShowDesktop()`에서 `CallShellDesktop(false)`를 부르기 **직전에** `restore_target_ = GetForegroundWindow();`로 채웁니다. 접기 전에 맨 앞이던 창입니다.

`HideDesktop()`에서는 이 값을 덮어쓰지 마십시오.

### 3-2. 되살린 뒤 앞으로 보낸다

`UndoMinimizeALL`이 `S_OK`를 돌려준 경우에만, 이미 있는 `kDesktopIconicTimerId`(200밀리초) 타이머가 터질 때 다음을 하십시오. **창들이 자리를 잡은 뒤여야 하므로 그 자리가 맞습니다.**

```cpp
if (restore_target_ != nullptr && IsWindow(restore_target_) && !IsIconic(restore_target_)) {
  const BOOL zorder = SetWindowPos(restore_target_, HWND_TOP, 0, 0, 0, 0,
                                   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  const BOOL fg = SetForegroundWindow(restore_target_);
  Log(L"peek", L"restore hwnd=%p zorder=%d fg=%d", static_cast<void*>(restore_target_), zorder ? 1 : 0, fg ? 1 : 0);
}
restore_target_ = nullptr;
```

두 호출을 나눈 이유가 있습니다.

- `SetWindowPos`에 `HWND_TOP`과 `SWP_NOACTIVATE`를 주면 **활성 상태를 건드리지 않고 쌓임 순서만** 올립니다. 이것은 거의 항상 성공합니다.
- `SetForegroundWindow`는 전경 창을 바꾸는 것이라 시스템 제약을 받습니다. **실패해도 그대로 두십시오.** 앞의 호출로 눈에 보이는 순서는 이미 맞습니다.

`fg=0`이 자주 찍히면 그때 다시 판단합니다.

### 3-3. 절대 하지 말 것

- **`AttachThreadInput`을 쓰지 마십시오.** 전경 창 제약을 우회하는 흔한 수법이지만, 두 스레드의 입력 큐를 묶는 동안 한쪽이 멈추면 다른 쪽도 함께 멈춥니다. 입력이 통째로 얼어붙는 사고로 이어집니다.
- `SetWindowPos`에 `HWND_TOPMOST`를 주지 마십시오. 항상 위에 뜨는 창으로 만들어 버립니다. 반드시 `HWND_TOP`입니다.
- 최소화하기 전의 **전체 쌓임 순서를 기록해 되돌리려 하지 마십시오.** 이번 범위는 맨 앞에 있던 창 하나입니다. 셸이 하는 걸러내기를 다시 만드는 일은 위험이 큽니다.
- `MinimizeAll` 쪽에는 아무것도 더하지 마십시오.

---

## 4. 곁다리로 관찰된 것 (고치지 말 것)

로그에 이런 줄이 하나 있습니다.

```
10:54:57.384 MinimizeAll ... iconic_before=0 iconic_after=0
```

접었는데 200밀리초 뒤에도 최소화되지 않은 것으로 보입니다. 다만 바로 다음 줄에서 같은 창이 `iconic_before=1`로 나오므로 **최소화는 되었고 200밀리초 시점이 일렀을 뿐입니다.** 애니메이션이 끝나기 전에 읽은 것입니다.

이것은 진단용 탐침의 한계일 뿐 기능의 결함이 아닙니다. **고치지 마십시오.**

---

## 5. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다.
2. `restore` 로그가 `UndoMinimizeALL` 뒤에만 남고 `MinimizeAll` 뒤에는 남지 않아야 합니다.
3. `AttachThreadInput`과 `HWND_TOPMOST`가 코드에 없어야 합니다. 검색해서 확인하십시오.
4. 나머지는 사용자 확인으로 넘기십시오. 터미널을 맨 앞에 둔 채 접었다 폈을 때 터미널이 다시 맨 앞으로 와야 하고, 로그의 `fg_after`가 `fg_before`와 같아야 합니다.

이 환경에서는 마우스와 키보드 입력을 합성할 수 없습니다. 사용자 화면에 입력을 밀어 넣으려고 하지 마십시오.

**빌드하려고 실행 중인 bamti를 종료했다면, 끝난 뒤 반드시 다시 띄워 놓으십시오.**

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
