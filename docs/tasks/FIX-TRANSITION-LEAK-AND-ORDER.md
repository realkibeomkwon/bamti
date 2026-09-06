# 작업 지시서: 되살릴 때의 전환 효과와 Z 순서를 바로잡는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-TOGGLE-TRANSITION-FLASH.md`(커밋 `9b72ded`)의 후속입니다. 건드리는 파일은 `src/desktop_toggle.cpp`, `src/desktop_toggle.hpp`, `src/menu_bar.cpp`, `src/menu_bar.hpp`입니다. `src/task_list.cpp`는 **건드리지 마십시오.**

---

## 1. 시험 결과

2026-09-06 22:36 시험입니다.

| # | 확인 | 결과 |
| --- | --- | --- |
| 1 | 감출 때 중간 창이 드러나지 않음 | **통과** |
| 2 | 되살릴 때 중간 창이 드러나지 않음 | 실패. 여전히 보인다 |
| 3 | 손으로 최소화하면 오므라드는 효과가 돌아옴 | **해당 없음. 2-1절 참고** |
| 4 | 열 번 왕복 뒤에도 순서가 어긋나지 않음 | 실패. 간헐적으로 깨진다 |

**고칠 것은 2번과 4번입니다.**

---

## 2. 셋을 각각 무엇으로 보는가

### 2-1. 3번은 결함이 아닙니다

이 컴퓨터는 **최소화 애니메이션이 시스템 설정에서 꺼져 있습니다.** 실측값입니다.

```
HKCU\Control Panel\Desktop\WindowMetrics\MinAnimate = 0
```

그러므로 손으로 최소화해도 오므라드는 효과가 없는 것이 **정상이고, 우리 코드와 무관합니다.** 3번을 결함으로 다루지 마십시오.

**다만 이 사실은 검증 방법 하나를 무너뜨립니다.** "효과가 돌아오는가"로는 우리가 되돌리기에 성공했는지 알 수 없습니다. 그래서 3-3절에서 반환값을 재게 합니다.

한 가지 더 따라옵니다. `MinAnimate`가 0인데도 1번이 우리 수정 전에는 실패하고 후에는 통과했습니다. 곧 **`DWMWA_TRANSITIONS_FORCEDISABLED`가 끄는 것은 고전적인 최소화 애니메이션과 별개의 DWM 전환**이고, 그것이 실제로 듣고 있다는 뜻입니다. 이 전제 위에서 아래를 진행합니다.

### 2-2. 2번: 되살리기의 효과를 끄지 못한 채 되돌리고 있습니다

`Reveal()`의 지금 차례입니다.

```cpp
RestoreHwnds(concealed_);                                     // 요청만 넘기고 곧바로 돌아온다
for (HWND hwnd : concealed_) { SetTransitions(hwnd, true); }  // 그 즉시 효과를 다시 켠다
```

문제가 둘입니다.

1. **되살리기 쪽은 효과를 끈 적이 없습니다.** `Conceal`만 끄고 있습니다.
2. `ShowWindow`는 다른 프로세스의 창에 대해 **요청만 넘기고 곧바로 돌아옵니다**(`FIX-TOGGLE-TRANSITION-FLASH.md` 2-2절). 상대가 아직 `SW_RESTORE`를 처리하지 않은 사이에 효과를 켜 버리므로, 설령 꺼 두었더라도 **되살아나는 순간에는 다시 켜져 있습니다.**

감추는 쪽만 통과하고 되살리는 쪽이 실패한 것과 정확히 맞아떨어집니다.

### 2-3. 4번: Z 순서를 창마다 따로 정하고 있습니다

`RestoreHwnds`는 창 하나마다 `ShowWindow` 다음에 곧바로 `SetWindowPos(HWND_TOP, ...)`를 부릅니다. 되살리기가 아직 끝나지 않은 창에 순서를 매기면, 뒤늦게 끝난 되살리기가 그 순서를 덮어씁니다. **간헐적으로 깨지는 것이 이 어긋남의 특징입니다.**

---

## 3. 고칠 것

### 3-1. 되살릴 때도 효과를 끄고, 끈 시간은 짧게 묶습니다

**효과는 전환이 도는 동안만 꺼 두고, 정해진 시간 뒤에 무조건 되돌립니다.** 감추었는지 되살렸는지와 무관합니다.

`DesktopToggle`에 목록과 메서드를 둡니다.

```cpp
// 우리가 전환 효과를 꺼 놓은 창. 되돌리면 비운다.
std::vector<HWND> transitions_off_;

// 꺼 놓은 창의 효과를 모두 되돌린다. 여러 번 불러도 안전하다.
void RestoreTransitions();
```

`Conceal`과 `Reveal`은 **작업 대상 창의 효과를 끄고 그 창들을 `transitions_off_`에 담기만 합니다.** 되돌리는 일은 하지 않습니다. 이미 담긴 창은 중복해서 넣지 마십시오.

`Reveal`에서 지금 하는 되돌리기 반복문은 **지웁니다.**

이렇게 하면 **새어 나가는 구멍도 함께 막힙니다.** 지금은 `Reveal()`이 올 때까지 효과를 꺼 둔 채 들고 있어서, 사용자가 작업 표시줄이나 독으로 창을 직접 되살리면 그 창의 설정이 꺼진 채로 남습니다. 시간으로 묶으면 그런 경우가 없습니다.

### 3-2. 되돌리기를 타이머로 미룹니다

`MenuBar`에 일회성 타이머를 둡니다. **비어 있는 식별자 6번을 쓰십시오**(1~5, 7, 9가 이미 쓰이고 있습니다).

```cpp
constexpr UINT_PTR kTransitionsTimerId = 6;
constexpr UINT kTransitionsRestoreMs = 400;
```

`StartDesktopPeek`에서 `Toggle()` 다음에 타이머를 겁니다.

```cpp
void MenuBar::StartDesktopPeek() {
  if (peek_latched_) {
    return;
  }
  desktop_toggle_.Toggle();
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kTransitionsTimerId, kTransitionsRestoreMs, nullptr);
  }
  peek_latched_ = true;
}
```

`WM_TIMER`에 갈래를 더합니다.

```cpp
if (wparam == kTransitionsTimerId) {
  KillTimer(hwnd_, kTransitionsTimerId);
  desktop_toggle_.RestoreTransitions();
  return 0;
}
```

같은 식별자로 `SetTimer`를 다시 부르면 앞의 것을 밀어내므로, 400밀리초 안에 전환이 또 일어나면 알아서 뒤로 미뤄집니다. **그것이 맞는 동작입니다.**

`WM_DESTROY`의 `KillTimer` 목록에 이 식별자를 더하고, **그 자리에서 `RestoreTransitions()`를 한 번 부르십시오.** 소멸자에만 맡기지 마십시오.

`~DesktopToggle()`은 `concealed_`가 아니라 **`transitions_off_`를 되돌리도록** 바꿉니다.

**400밀리초의 근거입니다.** DWM 전환은 기본적으로 200밀리초 남짓입니다. 두 배면 넉넉하고, 그동안 효과가 꺼져 있어도 이 컴퓨터는 어차피 `MinAnimate`가 0이라 사용자가 알아차릴 것이 없습니다.

### 3-3. 되돌리기가 듣는지 잽니다

2-1절에서 눈으로 확인할 방법이 사라졌으므로 반환값으로 대신합니다. `SetTransitions`가 `HRESULT`를 돌려주게 바꾸고, **묶음마다 한 줄씩** 남기십시오. 창마다 남기면 로그가 쌓입니다.

```
[peek] transitions off n=6 fail=0
[peek] transitions on n=6 fail=0
```

`fail`은 `FAILED(hr)`인 창의 개수입니다. 실패가 있으면 **첫 실패의 `hr`만** 같은 줄에 덧붙이십시오.

```
[peek] transitions on n=6 fail=6 hr=0x80070005
```

**이 두 줄로 다음이 갈립니다.**

1. 양쪽 다 `fail=0` → API는 듣고 있고, 남는 증상은 2-2절과 2-3절의 비동기 문제입니다.
2. `on fail`만 0이 아니다 → 켜는 방향만 거부당하는 것이고 `hr`이 다음 수를 정합니다.
3. `off fail`도 0이 아니다 → 1번이 통과한 이유가 우리 코드가 아니라는 뜻이므로 전제를 다시 세웁니다.

### 3-4. 되살릴 때 순서를 한 번에 정합니다

`RestoreHwnds`는 독도 쓰므로 **바꾸지 말고**, `DesktopToggle` 안에 되살리기를 따로 두십시오. `ActivateHwnd`는 `task_list.hpp`에 이미 공개되어 있으니 그대로 씁니다.

```
1. concealed_ 를 뒤에서 앞으로 훑으며 되살리기만 요청한다.
     최소화되어 있으면 ShowWindow(hwnd, SW_RESTORE)
     보이지 않으면    ShowWindow(hwnd, SW_SHOW)
   여기서 SetWindowPos 를 부르지 않는다.
2. BeginDeferWindowPos / DeferWindowPos / EndDeferWindowPos 로
   Z 순서를 한 번에 정한다. 역시 뒤에서 앞으로 훑고 HWND_TOP 과
   SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE 를 쓴다.
3. ActivateHwnd(concealed_.front()) 로 맨 앞이던 창을 활성화한다.
```

`concealed_`가 앞에서 뒤 순서이므로 **뒤에서 앞으로 훑어야** 맨 앞이던 창이 마지막에 얹혀 제일 위로 옵니다.

`BeginDeferWindowPos`가 실패하거나 중간에 `DeferWindowPos`가 `nullptr`을 돌려주면 **거기서 포기하고 예전처럼 창마다 `SetWindowPos`를 부르십시오.** 순서가 덜 정확할 뿐 동작은 합니다.

---

## 4. 이것으로도 남으면

**2번이나 4번이 그대로면 더 손대지 말고 보고하십시오.** 남은 원인은 되살리기 자체가 비동기라는 것이고, 다음 후보는 창이 실제로 복원되기를 확인한 뒤 순서를 정하는 방식입니다. **UI 스레드를 붙잡을 위험이 있어서 따로 판단해야 합니다. 이번에 그것까지 하지 마십시오.**

---

## 5. 검증

### 5-1. 빌드

Release 빌드가 경고 없이 통과해야 합니다.

### 5-2. 로그 (직접 확인 가능)

전환 한 번에 `transitions off`가 한 줄, 400밀리초 뒤에 `transitions on`이 한 줄이면 됩니다. **창 개수만큼 줄이 늘어나면 잘못 넣은 것입니다.**

`fail` 값을 작업 보고에 그대로 적으십시오. 3-3절의 갈림길이 그 값으로 정해집니다.

가만히 두었을 때 `[peek]` 줄이 늘지 않는 것도 함께 보십시오.

### 5-3. 사용자 확인으로 넘길 것

**이 환경에서는 마우스와 키보드 입력을 합성할 수 없습니다. 사용자 화면에 입력을 밀어 넣으려고 하지 마십시오.**

| # | 확인 | 결과 |
| --- | --- | --- |
| 1 | 감출 때 중간 창이 드러나지 않음 (되돌아가지 않았는지) | |
| 2 | 되살릴 때 중간 창이 드러나지 않음 | |
| 3 | 열 번 왕복 뒤에도 순서가 어긋나지 않음 | |
| 4 | 감춘 상태에서 작업 표시줄로 창 하나를 직접 되살려도 이후 동작이 멀쩡함 | |

**애니메이션이 돌아오는지는 묻지 마십시오.** 2-1절대로 이 컴퓨터에서는 원래 없습니다.

---

## 6. 하지 말 것

- **`SystemParametersInfo`로 `SPI_SETANIMATION`이나 `SPI_SETCLIENTAREAANIMATION`을 건드리지 마십시오.** 전역 설정이라 되돌리지 못하면 사용자 설정이 망가집니다.
- **`MinAnimate`를 비롯한 레지스트리 값을 쓰지 마십시오.** 읽는 것으로 끝냅니다.
- **`src/task_list.cpp`를 건드리지 마십시오.** `HideHwnds`와 `RestoreHwnds`는 독이 함께 씁니다.
- `Conceal`의 순서 뒤집기를 없애지 마십시오. 1번이 통과한 상태입니다.
- 감지부(훅, `CtrlHeld`, 예비 폴링, 감시 주기, 뜸, 걸쇠, 코너 폭)를 건드리지 마십시오.
- 창이 복원되기를 기다리는 대기 반복문을 넣지 마십시오. 4절의 사정이 있습니다.
- `Sleep`을 쓰지 마십시오. 미루는 일은 타이머로만 합니다.
- 전환 효과를 창마다 로그로 남기지 마십시오.
- 이번 범위 밖의 파일을 정리하거나 이름을 바꾸지 마십시오.

**빌드하려고 실행 중인 bamti를 종료했다면, 끝난 뒤 반드시 다시 띄워 놓으십시오.**
