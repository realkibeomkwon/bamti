# 작업 지시서: 두 번째 감추기가 첫 번째 목록을 지워서 창을 잃는 것을 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-DESKTOP-TOGGLE-REWRITE.md`(커밋 `4c95c2b`)의 후속입니다. 건드리는 파일은 `src/desktop_toggle.cpp`와 `src/desktop_toggle.hpp`입니다.

**재구현 자체는 성공했습니다.** 아래 1절에 근거를 적어 두었습니다. 이번에는 그 위에 남은 결함 하나만 고칩니다. 다른 것을 손대지 마십시오.

---

## 1. 재구현이 성공했다는 근거

2026-09-06 16:45 구간에서 아홉 번을 연달아 눌렀고 방향이 한 번도 어긋나지 않았습니다.

```
16:45:08.039 [peek] reveal  n=4
16:45:10.413 [peek] conceal n=4 top=CASCADIA_HOSTING_WINDOW_CLASS
16:45:11.306 [peek] reveal  n=4
16:45:12.618 [peek] conceal n=4 top=CASCADIA_HOSTING_WINDOW_CLASS
16:45:13.450 [peek] reveal  n=4
16:45:13.990 [peek] conceal n=4 top=CASCADIA_HOSTING_WINDOW_CLASS
16:45:15.111 [peek] reveal  n=4
16:45:15.760 [peek] conceal n=4 top=CASCADIA_HOSTING_WINDOW_CLASS
16:45:16.769 [peek] reveal  n=4
```

같은 구간에서 `[task] activate failed`가 **한 줄도 없습니다.** 예전 구현이 `restore ... fg=0`으로 전경 전환에 실패하던 것(`TASK-DESKTOP-TOGGLE-REWRITE.md` 2-3절)이 `ActivateHwnd` 경로로 바뀌면서 사라졌습니다.

---

## 2. 남은 결함: 두 번째 `Conceal`이 첫 번째 목록을 덮어쓴다

### 2-1. 로그에 실제로 창 네 개를 잃은 기록이 있습니다

2026-09-06 16:39:20부터 16:40:24까지입니다. **중간에 `reveal`이 한 줄도 없습니다.**

```
16:39:20.066 [peek] conceal n=5 top=CASCADIA_HOSTING_WINDOW_CLASS
16:40:07.274 [dock]  click index=9 running=1 hwnd=0000000000950910 name=터미널
16:40:08.878 [peek] conceal n=1 top=CASCADIA_HOSTING_WINDOW_CLASS
16:40:24.886 [peek] reveal  n=1
```

무슨 일이 일어났는지 순서대로 보면 이렇습니다.

1. `conceal n=5`로 창 다섯 개가 최소화되고 `concealed_`에 다섯 개가 담깁니다.
2. 사용자가 독에서 터미널을 눌러 **한 개만 직접 되살립니다.** 우리 목록은 그대로 다섯 개입니다.
3. 다시 Ctrl과 코너로 전환합니다. 창이 하나 보이므로 `Revealed()`가 거짓이고 `Conceal()`이 불립니다.
4. `Conceal()`이 `concealed_ = 목록`으로 **다섯 개짜리 목록을 한 개짜리로 덮어씁니다.** 나머지 네 개는 이 순간 우리 기억에서 사라집니다.
5. `reveal n=1`이 터미널 하나만 되살립니다. **나머지 네 개는 최소화된 채로 영영 남습니다.**

`n=5`가 `n=1`로 줄었다가 `reveal n=1`로 끝나는 것이 그 증거입니다.

### 2-2. 원인은 지시서에 있었습니다

`TASK-DESKTOP-TOGGLE-REWRITE.md` 5-2절이 `concealed_ = 목록`이라고 적었습니다. 구현이 지시를 어긴 것이 아니라 **지시가 틀렸습니다.** 감추기가 두 번 연달아 일어날 수 있다는 것을 그 지시서가 놓쳤습니다.

두 번 연달아 감추는 상황은 `Conceal` 뒤에 창이 다시 보이게 되면 언제든 생깁니다. 독에서 눌러 되살리거나, 알림을 눌러 창이 뜨거나, 새 창이 열리면 됩니다. **드문 경우가 아닙니다.**

---

## 3. 고칠 것

`Conceal()`이 새 목록을 **덮어쓰지 말고 앞에 이어 붙이도록** 바꿉니다.

```
1. 목록 = CollectDesktopClearWindows()
2. 목록이 비어 있으면 아무것도 하지 않고 끝낸다.
3. 뒤집은 사본을 HideHwnds 에 넘긴다.            // 지금과 같다
4. 새 concealed_ 를 이렇게 만든다.
     - 먼저 이번 목록을 순서대로 담는다.
     - 이어서 기존 concealed_ 를 순서대로 담되,
       이미 담긴 창과 IsWindow 가 거짓인 창은 건너뛴다.
5. 로그: conceal n=<이번에 감춘 개수> total=<concealed_ 전체 개수> top=<맨 앞 창 클래스>
```

**4번의 순서가 중요합니다.** 이번에 감춘 창들은 방금까지 화면에 보였으므로 Z 순서상 앞이고, 앞서 감춰 둔 창들은 그 뒤입니다. 이 순서로 담아야 `RestoreHwnds`가 역순으로 훑으면서 뒤의 것부터 되살리고 맨 앞이던 창을 마지막에 활성화합니다. 원래 순서가 그대로 돌아옵니다.

`IsWindow` 걸러내기를 반드시 넣으십시오. 그러지 않으면 닫힌 창의 손잡이가 목록에 계속 쌓입니다.

`Reveal()`은 바꾸지 마십시오. 지금도 `concealed_` 전체를 넘기고 비우므로, 목록만 제대로 쌓이면 그대로 맞습니다.

### 3-1. 덤으로 열거를 한 번 줄입니다

지금 `Toggle()`은 `Revealed()`에서 `EnumWindows`를 한 번 돌고, 거짓이면 `Conceal()`에서 또 한 번 돕니다. **한 번 전환에 전체 창 열거가 두 번 도는 셈입니다.**

`Conceal()`이 목록을 인자로 받게 바꾸고 `Toggle()`에서 한 번만 모으십시오.

```cpp
void DesktopToggle::Toggle() {
  std::vector<HWND> visible = CollectDesktopClearWindows();
  if (visible.empty()) {
    Reveal();
  } else {
    Conceal(std::move(visible));
  }
}
```

`Revealed()`는 헤더에 그대로 두십시오. 다른 곳에서 쓰지 않더라도 이 판정의 뜻을 이름으로 남겨 두는 편이 낫고, 지금은 `Toggle()`이 그 자리를 대신합니다. **다만 `Toggle()` 안에서 `Revealed()`를 부르지는 마십시오.** 그러면 열거가 다시 두 번이 됩니다.

---

## 4. 검증

### 4-1. 빌드

Release 빌드가 경고 없이 통과해야 합니다.

### 4-2. 목록이 쌓이는지 (직접 확인 가능)

이 항목은 사용자 없이 확인할 수 있습니다. **`total=`이 로그에 나오므로 임시 진단 코드를 넣을 필요가 없습니다.**

1. 창을 여러 개 띄운 상태에서 `Conceal`을 한 번 일으킵니다.
2. 그중 하나를 되살립니다.
3. 다시 `Conceal`을 일으킵니다.
4. **`total=`이 1단계의 `n=`보다 작아지지 않아야 합니다.** 예전에는 5에서 1로 줄었습니다.
5. `Reveal`을 일으켜 `reveal n=`이 `total=`과 같은지 봅니다.

이 절차를 마우스 없이 어떻게 일으킬지는 알아서 정하되, **사용자 화면에 입력을 합성하지 마십시오.** 마땅한 방법이 없으면 4-3으로 넘기고 그 사실을 보고에 적으십시오.

### 4-3. 사용자 확인으로 넘길 것

**이 환경에서는 마우스와 키보드 입력을 합성할 수 없습니다.**

| # | 확인 | 결과 |
| --- | --- | --- |
| 1 | 창 여럿을 Ctrl과 코너로 감춘 뒤, 독에서 하나만 되살리고, 다시 Ctrl과 코너로 감췄다가 되살렸을 때 **처음의 창이 모두 돌아옴** | 통과 (2026-09-06 21:53, 아래 로그) |
| 2 | 그 왕복을 다섯 번 반복해도 창이 하나도 사라지지 않음 | 미확인. 1번을 한 번만 시험했습니다 |

1번의 근거입니다.

```
21:53:04.616 [peek] conceal n=5 total=5 top=CASCADIA_HOSTING_WINDOW_CLASS
21:53:09.749 [peek] conceal n=1 total=5 top=MozillaWindowClass
21:53:11.984 [peek] reveal  n=5
```

두 번째 줄이 이 수정의 핵심입니다. 이번에 감춘 창은 하나인데 목록은 다섯 개로 유지되었고(`n=1 total=5`), 되살릴 때 다섯 개가 모두 돌아왔습니다. 고치기 전에는 같은 자리에서 `conceal n=1` 뒤에 `reveal n=1`이 나와 네 개를 잃었습니다(2-1절).

같은 구간에 `[task] activate failed`가 없습니다.

---

## 5. 하지 말 것

- 감지부를 건드리지 마십시오. 훅, 예비 폴링, 감시 주기, 뜸, 걸쇠, 코너 폭, 재장전 거리 전부 그대로입니다.
- `Reveal()`의 몸통을 바꾸지 마십시오.
- `IShellDispatch`를 다시 쓰지 마십시오.
- `SetForegroundWindow`를 직접 부르지 마십시오. `ActivateHwnd`를 거칩니다.
- 사라진 탐침(`probe` 줄)을 되살리지 마십시오.
- 이번 범위 밖의 파일을 정리하거나 이름을 바꾸지 마십시오.
- 레지스트리에 쓰는 확인 절차를 넣지 마십시오.

**빌드하려고 실행 중인 bamti를 종료했다면, 끝난 뒤 반드시 다시 띄워 놓으십시오.** 2026-09-06 16:54에 종료된 뒤로 떠 있지 않습니다.
