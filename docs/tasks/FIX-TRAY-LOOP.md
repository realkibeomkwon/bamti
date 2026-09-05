# 작업 지시서 8: 트레이 재숨김이 자기 자신을 되먹여 UI 스레드를 포화시킨다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

멈춤의 원인을 **확정했습니다.** 추측이 아니라 실행 중인 프로세스에서 측정한 결과입니다.

---

## 1. 측정 결과

### 1.1 UI 스레드는 죽지 않았다

멈춘 상태의 프로세스를 그대로 조사했습니다.

```
watchdog.log:
  11:00:47.705 [watchdog] alive stage=idle ping=ok
  11:00:52.812 [watchdog] alive stage=idle ping=ok
```

정지 감지 없이 `alive`가 계속 쌓입니다. `stage=idle`이므로 UI 스레드는 `GetMessage`에서 메시지를 받아 처리하는 중입니다. `Responding`도 `True`입니다.

**앞선 지시서들에서 "UI 스레드가 멈췄다"고 본 것은 틀렸습니다.** 스레드는 돌고 있었습니다.

### 1.2 한 스레드가 코어 하나를 통째로 태우고 있다

```
Id     ThreadState   CPU(초)
34000  Running       111.31   → 3초 뒤 187.08
```

3초 동안 2.77초를 썼습니다. **코어 0.92개를 계속 점유**합니다. 다른 다섯 스레드는 전부 `Wait`이고 CPU가 0.03초 이하입니다.

즉 UI 스레드가 메시지 루프를 **전속력으로 돌고 있습니다.**

### 1.3 그래서 타이머가 굶는다

```
10:59:01.606 [popup] arm guard id=1 err=0
10:59:01.606 [popup] open rows=1 shown 0ms
10:59:01.608 [popup] msg=mousemove pt=12,66 inside=0
          ← 이후 alive tick 없음
```

`WM_TIMER`는 Win32 메시지 큐에서 **가장 낮은 우선순위**입니다. 게시된(posted) 메시지가 큐에 계속 있으면 영원히 생성되지 않습니다.

게시 메시지가 끊임없이 들어오니 `WM_TIMER`가 한 번도 발생하지 못하고, 가드 폴링이 돌지 않아 메뉴가 닫히지 않습니다. `WM_PAINT`도 같은 이유로 굶어 화면이 갱신되지 않습니다.

**메뉴가 안 닫히는 것, 화면이 굳는 것, Spotlight 입력이 안 되는 것이 전부 이 하나의 결과입니다.**

---

## 2. 되먹임 고리

`src/dock.cpp:1056`과 `src/dock.cpp:1136`을 함께 보십시오.

```cpp
void CALLBACK Dock::TrayWinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
  if (g_notify == nullptr) {
    TaskbarController::Rehide();
    return;
  }
  if (!g_tray_posted.exchange(true)) {
    PostMessageW(g_notify, kTrayChangedMsg, 0, 0);
  }
}
```

```cpp
case kTrayChangedMsg:
  g_tray_posted = false;              // ← 먼저 빗장을 푼다
  if (TaskbarController::Rehide()) {  // ← 그리고 트레이를 움직인다
    RaiseOverlays();
  }
  return 0;
```

고리는 이렇게 돕니다.

```
1. 트레이 창에 변화가 생긴다 (자동 숨김 태스크바가 화면 아래에서 살짝 올라온다)
2. explorer 스레드 한정 훅이 EVENT_OBJECT_LOCATIONCHANGE 를 받는다
3. kTrayChangedMsg 를 게시한다
4. 핸들러가 g_tray_posted = false 로 빗장을 푼다
5. Rehide() 가 SetWindowPos 로 트레이를 화면 밖으로 옮긴다
6. 그 이동이 다시 EVENT_OBJECT_LOCATIONCHANGE 를 일으킨다
7. 2번으로 돌아간다
```

**우리가 트레이를 움직이는 행위 자체가 우리를 다시 부릅니다.** `g_tray_posted` 빗장은 중복 게시만 막을 뿐, 이 고리를 끊지 못합니다. 오히려 4번에서 `Rehide()`보다 **먼저** 풀리기 때문에, 5번이 일으킨 이벤트가 곧바로 새 메시지를 게시할 수 있습니다.

### 왜 독을 조작할 때 시작되는가

`TaskbarController`는 태스크바를 **자동 숨김**으로 만들어 두었습니다. 자동 숨김 태스크바는 마우스가 화면 아래 가장자리에 닿으면 살짝 올라옵니다.

독은 바로 그 화면 아래 가장자리에 있습니다. 독 아이콘을 우클릭하면 커서가 그 영역에 머물고, 태스크바가 올라오려 하고, 우리가 다시 밀어 넣고, 고리가 시작됩니다.

사용자가 "탐색기 우클릭을 5회 반복할 때는 괜찮았는데 이어서 캡처도구와 터미널을 우클릭하니 멈췄다"고 한 것과 맞습니다. 가장자리에 머문 시간이 쌓이면 언젠가 걸립니다.

---

## 3. 수정 지시

### 3-1. 우리가 일으킨 이동을 우리 이벤트로 세지 않는다 (필수, 핵심)

`Rehide()`가 트레이를 움직이는 동안에는 트레이 이벤트를 무시해야 합니다.

`TaskbarController`에 억제 구간을 두십시오.

```cpp
// HideTrayWindows() 가 SetWindowPos 를 호출하는 동안 참이다.
bool TaskbarController::SuppressingTrayEvents();
```

- `HideTrayWindows()`와 `ShowTrayWindows()` 진입에서 억제를 켜고, 반환 직전에 끕니다. RAII 객체로 만들어 중간 반환에도 반드시 꺼지게 하십시오.
- 창을 옮긴 뒤 이벤트가 조금 늦게 도착할 수 있으므로, 억제를 끄는 시각에 **200ms의 여유**를 두십시오. 즉 `GetTickCount64()` 기준 만료 시각을 저장하고, 그 시각 이전이면 억제 상태로 봅니다.
- `TrayWinEventProc`는 억제 중이면 **아무것도 하지 않고 즉시 반환**합니다. 메시지를 게시하지 마십시오.

### 3-2. 빗장을 늦게 푼다 (필수)

`kTrayChangedMsg` 핸들러에서 `g_tray_posted`를 `Rehide()` **뒤에** 푸십시오.

```cpp
case kTrayChangedMsg:
  if (TaskbarController::Rehide()) {
    RaiseOverlays();
  }
  g_tray_posted = false;   // ← 작업이 끝난 뒤에 푼다
  return 0;
```

현재는 먼저 풀기 때문에, `Rehide()`가 일으킨 이벤트가 같은 처리 도중에 새 메시지를 만들 수 있습니다. 3-1과 함께 두 겹으로 막습니다.

### 3-3. 트레이 감시 훅의 범위를 좁힌다 (필수)

`TaskbarController::WatchTray()`가 `EVENT_OBJECT_SHOW`부터 `EVENT_OBJECT_LOCATIONCHANGE`까지를 한 범위로 받고 있습니다. 이 범위에는 우리가 관심 없는 이벤트가 다수 포함되고, `LOCATIONCHANGE`는 특히 빈번합니다.

필요한 것만 개별로 등록하십시오.

```cpp
EVENT_OBJECT_SHOW
EVENT_OBJECT_STATECHANGE
EVENT_OBJECT_LOCATIONCHANGE
```

`LOCATIONCHANGE`는 남기되 3-1의 억제로 되먹임을 끊습니다.

### 3-4. 되먹임을 감지하면 로그를 남긴다 (필수)

같은 결함이 다른 경로로 재발할 수 있습니다. `kTrayChangedMsg` 처리 횟수를 세고, **1초에 10건을 넘으면** 경고를 남기십시오.

```cpp
Log(L"dock", L"tray storm %u msgs in 1s", count);
```

로그만 남기고 동작을 막지는 마십시오. 다만 이 줄이 나오면 되먹임이 남아 있다는 뜻입니다.

같은 방식으로 `kTasksChangedMsg`에도 카운터를 두십시오. 그쪽도 같은 구조의 위험이 있습니다.

### 3-5. 게시 메시지가 타이머를 굶기지 않게 한다 (필수)

되먹임을 고쳐도, 이벤트가 몰리는 순간에는 `WM_TIMER`가 밀릴 수 있습니다. 팝업이 열려 있는 동안에는 닫힘 판정이 결코 굶지 않아야 합니다.

`Dock::HandleMessage`에서 **어떤 메시지를 처리하든**, 팝업이 열려 있고 마지막 `Tick()` 이후 100ms가 지났으면 `popup_.Tick()`을 부르십시오.

```cpp
// HandleMessage 진입부
if (popup_.IsOpen() && GetTickCount64() - last_popup_tick_ >= 100) {
  last_popup_tick_ = GetTickCount64();
  popup_.Tick();
}
```

이렇게 하면 메시지가 몰릴수록 오히려 자주 검사하게 되어, 굶는 상황이 원천적으로 사라집니다. `Tick()`은 이미 재진입에 안전합니다.

---

## 4. 하지 말아야 할 것

- 지금까지의 수정을 하나도 되돌리지 마십시오.
- **트레이 감시 훅 자체를 제거하지 마십시오.** explorer가 태스크바를 다시 띄우는 것을 되돌리는 기능은 필요합니다.
- `Rehide()`를 주기 폴링으로 되돌리지 마십시오. `PLAN.md` 3단계에서 없앤 비용입니다.
- 3-4에서 되먹임을 **차단**하지 마십시오. 로그만 남깁니다.
- 감시 스레드와 `watchdog.log`를 제거하지 마십시오. 앞으로도 필요합니다.
- 억제 구간 안에서 로그를 남기거나 다른 창을 조작하지 마십시오. 억제는 짧고 단순해야 합니다.
- `WH_MOUSE_LL`, `WH_KEYBOARD_LL`을 도입하지 마십시오.

---

## 5. 검증

**되먹임 (핵심)**
- [ ] 독 아이콘을 우클릭하고 바깥 클릭으로 닫기를 **스무 번** 반복해도 멈추지 않는다.
- [ ] 서로 다른 아이콘(탐색기, 캡처도구, 터미널)을 번갈아 우클릭해도 멈추지 않는다. 이번 재현 조건이다.
- [ ] 커서를 화면 맨 아래 가장자리에 **30초간** 두어도 CPU가 오르지 않는다.
- [ ] `[dock] tray storm` 줄이 남지 않는다.
- [ ] `watchdog.log`에 `alive stage=idle ping=ok`만 5초 간격으로 쌓인다.

**CPU**
- [ ] 유휴 60초 누적 CPU가 **1초 미만**이다. 되먹임이 사라지면 이전 3.8초보다 좋아질 수 있다.
- [ ] 메뉴를 여닫는 동안 코어 점유가 순간적으로도 50%를 넘지 않는다.

**회귀**
- [ ] 태스크바가 다시 보이면 여전히 자동으로 숨겨진다. 3-1의 억제가 이 기능을 죽이지 않았는지 확인한다.
- [ ] explorer를 재시작해도 태스크바가 다시 숨겨진다.
- [ ] `bamti.exe --restore-taskbar`가 동작한다.
- [ ] 메뉴 열기와 바깥 클릭 닫기가 지금과 같이 즉시 동작한다.
- [ ] `[popup] alive tick=1 src=timer`가 계속 나온다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 6. 커밋

두 개로 나누십시오.

```
fix: 트레이 재숨김이 자기 이벤트를 되먹이지 않게 한다
```
3-1부터 3-4까지입니다. 본문에 되먹임 고리의 구조와, 수정 전후의 유휴 CPU 값을 적으십시오.

```
fix: 메시지가 몰려도 팝업 닫힘 판정이 굶지 않게 한다
```
3-5입니다.
