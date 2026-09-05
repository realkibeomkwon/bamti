# 작업 지시서: Ctrl에서 손을 뗀 뒤에 미리 보기가 켜지는 것을 막는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-SHOW-DESKTOP.md`(커밋 `e439519`)의 후속입니다. 건드리는 파일은 `src/menu_bar.cpp` 하나입니다.

---

## 1. 무엇이 문제인가

**Ctrl에서 손을 뗀 뒤에도 바탕 화면 미리 보기가 한 번 켜졌다가 100밀리초 뒤에 꺼집니다.** 화면이 잠깐 번쩍입니다.

다음 순서로 재현됩니다.

1. Ctrl을 누른 채 상단바 오른쪽 끝에 마우스를 올립니다. `UpdateDesktopPeek`가 `peek_ctrl_ = true`로 두고 300밀리초짜리 뜸 타이머를 겁니다.
2. **마우스를 움직이지 않은 채** Ctrl에서 손을 뗍니다.
3. 300밀리초가 지나 뜸 타이머가 터집니다.

문제는 `MenuBar::DesktopPeekWanted`의 이 조건입니다.

```cpp
  if (!peek_ctrl_ && (GetKeyState(VK_CONTROL) & 0x8000) == 0 &&
      (GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) {
    return false;
  }
```

`peek_ctrl_`이 참이면 앞쪽에서 단락되어 **실제 키 상태를 아예 보지 않습니다.** 그런데 `peek_ctrl_`은 `UpdateDesktopPeek`에서만 갱신되고, 그 함수는 `WM_MOUSEMOVE`로만 불립니다. 2번에서 마우스를 움직이지 않았으므로 `peek_ctrl_`은 참으로 남아 있습니다.

그래서 뜸 타이머의 판정

```cpp
        if (peek_ctrl_ && DesktopPeekWanted(peek_pt_)) {
          StartDesktopPeek();
        }
```

이 양쪽 모두 낡은 `peek_ctrl_`을 근거로 통과하고, 미리 보기가 켜집니다. 100밀리초 뒤 폴링 타이머가 Ctrl이 올라간 것을 발견해 끄기 때문에 눈에는 번쩍임으로 보입니다.

위젯을 Ctrl로 끌어 옮긴 뒤 손을 떼고 마우스를 오른쪽에 둔 채로 있으면 바로 겪게 됩니다.

---

## 2. 어떻게 고치는가

`peek_ctrl_`이라는 기억을 판정의 근거로 삼지 말고, **판정할 때마다 실제 키 상태를 읽으십시오.**

`DesktopPeekWanted`의 위 조건을 다음으로 바꿉니다.

```cpp
  if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) == 0) {
    return false;
  }
```

`GetKeyState`는 함께 보지 마십시오. 상단바는 `WS_EX_NOACTIVATE`라 키보드 메시지를 받지 않으므로 이 창의 스레드 큐가 들고 있는 키 상태는 낡을 수 있습니다. 실제로 폴링 타이머는 이미 `GetAsyncKeyState`만 쓰고 있고, 그 판정은 잘 동작합니다. **두 판정이 같은 근거를 보게 만드는 것이 이 수정의 요점입니다.**

뜸 타이머 쪽의 `peek_ctrl_ &&`도 지웁니다. `DesktopPeekWanted` 하나만 보면 됩니다.

```cpp
        if (DesktopPeekWanted(peek_pt_)) {
          StartDesktopPeek();
        }
```

`peek_ctrl_` 멤버가 더 쓰이지 않으면 `UpdateDesktopPeek`의 대입과 `menu_bar.hpp`의 선언까지 함께 지우십시오. 남은 곳이 있으면 그대로 둡니다.

`UpdateDesktopPeek`의 `ctrl_down` 인자(`WM_MOUSEMOVE`의 `MK_CONTROL`)도 쓰이지 않게 되면 인자와 호출부에서 함께 지우십시오.

---

## 3. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다.
2. **번쩍임이 사라졌는지.** Ctrl을 누른 채 오른쪽 끝에 올린 다음, 마우스를 움직이지 말고 300밀리초 안에 Ctrl에서 손을 떼십시오. 미리 보기가 **한 번도 켜지지 않아야 합니다.** 로그에 `[peek] on`이 남지 않아야 합니다.
3. **정상 동작 회귀.** Ctrl을 계속 누른 채 올리면 300밀리초 뒤에 미리 보기가 켜지고, 손을 떼면 100밀리초 안에 꺼져야 합니다. 로그에 `on`과 `off`가 한 쌍씩 남습니다.
4. 마우스를 옆으로 옮겼을 때와 상단바 밖으로 나갔을 때의 해제가 그대로 동작해야 합니다.
5. 미리 보기가 켜진 채 bamti를 종료해도 화면이 투명하게 남지 않아야 합니다.

이 환경에서는 마우스와 키보드 입력을 합성할 수 없습니다. **사용자 화면에 입력을 밀어 넣으려고 하지 마십시오.** `WM_MOUSEMOVE`와 `WM_TIMER`를 직접 보내는 방식으로 확인하되, 2번과 3번은 `GetAsyncKeyState`가 실제 키를 읽으므로 메시지만으로는 재현되지 않습니다. **그 두 항목은 코드 경로를 설명하고 사용자 확인으로 넘기십시오.**

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
