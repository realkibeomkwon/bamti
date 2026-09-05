# 작업 지시서: 독 우클릭 메뉴가 늦게 뜨고 두 번째부터 멈추는 문제

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`PLAN.md` 1단계로 도입한 `PopupSurface`에 두 가지 결함이 남아 있습니다. 이 문서는 그 원인과 수정 방법을 지정합니다.

증상은 다음 두 가지입니다.

1. 메뉴가 화면에 나타나기까지 1초 정도 걸린다.
2. 두 번째로 연 메뉴부터는 사라지지 않고 그 상태로 멈춘다.

---

## 0. 먼저 알아야 할 현재 상태

`src/popup_surface.cpp`와 `src/popup_surface.hpp`에 **커밋되지 않은 수정이 이미 들어가 있습니다.** 작업 전에 `git diff`로 내용을 확인하십시오. 이 지시서는 그 수정이 적용된 상태를 전제로 합니다.

이미 적용된 수정은 다음 네 가지입니다.

- `Open()`에서 `target_.Reset()` 제거
- `EnsureRenderTarget()`에서 재생성 대신 `Resize()` 사용
- `Create()`에서 렌더 타깃 예열
- `OnGuardTimer()`에 마우스 버튼 눌림 폴링 추가

이 수정들은 되돌리지 마십시오. 아래 1장이 그 근거입니다.

`src/spotlight.cpp`와 `src/spotlight.hpp`에도 수정이 있으나 이번 작업과 무관합니다. 건드리지 마십시오.

---

## 1. 측정으로 확인된 사실

`~/.bamti/bamti.log`에서 얻은 실제 값입니다. 추정이 아닙니다.

```
08:17:38.908 [popup] create render target 8x8 ok=1 250ms
08:17:38.908 [popup] render target warm=1 250ms
08:17:40.356 [popup] create render target 8x8 ok=1 0ms
08:17:43.210 [dock] menu open index=0 name=explorer.exe running=1 windows=1 pinned=1
08:17:43.212 [dock] menu hwnd=0000000000070BD6 rows=7 0ms
08:17:44.370 [dock] menu open index=1 name=SnippingTool.exe running=0 windows=0 pinned=1
08:17:44.374 [dock] menu hwnd=0000000000070BD6 rows=1 16ms
```

여기서 확정된 사실은 세 가지입니다.

1. **렌더 타깃 재생성은 더 이상 병목이 아닙니다.** `create render target`이 시작 시 두 번(메뉴 바용과 독용)만 찍히고, 메뉴를 열 때는 전혀 찍히지 않습니다. 예열 비용 250ms는 시작 시 한 번뿐입니다.
2. **`OpenDockMenu()`는 이미 충분히 빠릅니다.** 0ms와 16ms입니다. 즉 사용자가 체감하는 1초는 이 함수 **바깥**에서 발생합니다.
3. 로그는 두 번째 메뉴가 열린 직후 끊깁니다. 닫힘 기록이 없습니다.

따라서 1초는 `Open()`이 반환된 **뒤**, 창이 실제로 그려지기까지의 구간에 있습니다.

---

## 2. 근본 원인

### 2.1 `WM_PAINT`가 큐에서 밀린다

`PopupSurface`는 화면 갱신을 전적으로 `InvalidateRect()`에 맡기고 있습니다. `UpdateWindow()`는 코드 전체에서 **한 번도 호출하지 않습니다.** `grep -n "UpdateWindow" src/popup_surface.cpp`로 확인할 수 있습니다.

`WM_PAINT`는 Win32 메시지 큐에서 **가장 낮은 우선순위**입니다. 정확히는 게시된(posted) 메시지가 모두 처리되어 큐가 빈 뒤에야 생성됩니다. 그래서 다음 일이 벌어집니다.

- `Open()`이 `SetWindowPos(... SWP_SHOWWINDOW)`로 창을 **먼저 보여줍니다.**
- 그 시점의 창 내용은 클래스 배경 브러시(`BLACK_BRUSH`)뿐입니다.
- 실제 그리기는 큐에 남은 게시 메시지를 전부 소진한 뒤에야 일어납니다.

독은 WinEvent 훅으로 `kTasksChangedMsg`를 게시하고, `ScheduleRebuild()`와 폴링 타이머가 계속 돕니다. 창을 활성화하거나 `모두 보기`를 실행한 직후에는 WinEvent가 몰려 들어옵니다. 그 사이에 열린 메뉴는 검은 창인 채로 방치되고, 큐가 빌 때까지 그려지지 않습니다.

이것이 **두 증상을 모두 설명합니다.**

- 1초 지연: `WM_PAINT`가 밀린 만큼 늦게 그려집니다.
- 두 번째 메뉴부터 멈춤: 첫 메뉴에서 명령을 실행하면 WinEvent가 쏟아지므로, 이후 메뉴는 훨씬 오래 굶습니다. 호버 반응도 `InvalidateRect`에만 의존하므로 함께 죽습니다. 사용자에게는 "멈춘 창"으로 보입니다.

### 2.2 배경 창의 `SetCapture`는 바깥 클릭을 받지 못한다

`PLAN.md`의 규칙 2는 "바깥 클릭은 마우스 캡처로 감지한다"였습니다. **이 전제는 틀렸습니다.**

`SetCapture` 공식 문서의 서술은 다음과 같습니다.

> Only the foreground window can capture the mouse. When a background window attempts to capture the mouse, the window receives messages only for mouse events that occur when the cursor hot spot is within the visible region of the window.

`PopupSurface`는 `WS_EX_NOACTIVATE`이고 포그라운드가 되지 않으므로 **항상 배경 창**입니다. 따라서 캡처를 잡아도 커서가 팝업 위에 있을 때만 마우스 메시지를 받습니다. 팝업 **바깥**을 클릭하면 `WM_LBUTTONDOWN` / `WM_RBUTTONDOWN`이 팝업에 도달하지 않습니다. 즉 `Handle()`의 바깥 클릭 처리와 `WM_CAPTURECHANGED` 안전망이 설계대로 작동하지 않습니다.

현재 `OnGuardTimer()`에 추가된 버튼 눌림 폴링이 이 구멍을 메우고 있으나, 주기가 200ms이고 `GetCapture()` 검사가 앞에 있어 신뢰도가 낮습니다.

---

## 3. 수정 지시

### 3-1. 팝업을 보인 직후 즉시 그린다 (필수, 최우선)

`src/popup_surface.cpp`의 `Open()`에서 창을 보인 뒤 `UpdateWindow()`를 호출해 `WM_PAINT`를 **동기로** 강제하십시오. 이렇게 하면 큐 우선순위와 무관하게 그리기가 끝난 뒤에 창이 보입니다.

현재 `Open()` 말미는 다음과 같습니다.

```cpp
SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
SetCapture(hwnd_);
ArmGuardTimer();
InvalidateRect(hwnd_, nullptr, FALSE);
```

이를 다음 순서로 바꾸십시오. **창을 보이기 전에 먼저 그려야** 검은 창이 한 프레임도 보이지 않습니다.

```cpp
InvalidateRect(hwnd_, nullptr, FALSE);
UpdateWindow(hwnd_);   // 큐를 우회해 지금 그린다
SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
SetCapture(hwnd_);
ArmGuardTimer();
```

`Render()`는 `open_`이 참이고 `content_`가 있을 때만 그리므로, 이 시점에는 두 조건이 모두 충족되어 있어야 합니다. 현재 코드에서 `open_ = true`는 이 구간보다 앞에 있으므로 그대로 두면 됩니다.

숨겨진 창에도 `UpdateWindow()`가 `WM_PAINT`를 보내는지 확인하십시오. 보내지 않는다면 `SetWindowPos`를 먼저 호출하고 그 **직후** `InvalidateRect` + `UpdateWindow`를 부르는 순서로 바꾸십시오. 검은 창이 한 프레임 스치는 편이, 1초 동안 검은 창이 남는 것보다 낫습니다.

### 3-2. 호버 갱신도 즉시 반영한다 (필수)

`Handle()`의 `WM_MOUSEMOVE`에서 `hot_`이 바뀔 때도 `InvalidateRect`만 호출하고 있습니다. 같은 이유로 반응이 죽습니다. 바로 뒤에 `UpdateWindow(hwnd_)`를 추가하십시오.

메뉴는 행이 수십 개를 넘지 않고 Direct2D로 그리므로, 동기 그리기 비용은 무시할 수 있습니다.

### 3-3. 열림에서 그려짐까지의 실제 지연을 계측한다 (필수)

3-1을 적용한 뒤에도 느리면 원인이 다른 곳에 있습니다. 이를 구분할 수 있도록 계측을 남기십시오.

`Open()` 시작 시각을 지역 변수에 담고, `UpdateWindow()` 반환 직후에 다음을 기록하십시오.

```cpp
Log(L"popup", L"open rows=%d shown %ums", /* 행 수 */, static_cast<unsigned>(GetTickCount64() - started));
```

이 값이 사용자가 체감하는 지연과 일치해야 합니다. 일치하지 않으면 `Open()` 바깥, 즉 `Dock::HandleMessage`가 `WM_RBUTTONUP`을 받기까지의 구간이 범인입니다. 그 경우 `WM_RBUTTONDOWN` 수신 시각도 함께 기록해 비교하십시오.

### 3-4. 닫힘 사유를 반드시 로그로 남긴다 (필수)

현재 팝업이 닫힐 때 아무 기록도 남지 않아, 멈춤이 "닫히지 않은 것"인지 "닫혔는데 화면이 안 지워진 것"인지 구분할 수 없습니다. 이 구분이 없으면 다음 수정도 추측이 됩니다.

`Dismiss()`에 사유 인자를 추가하십시오.

```cpp
enum class DismissReason {
  kInvoke,        // 항목 선택
  kOutsideClick,  // 팝업 밖 클릭 (메시지 경로)
  kOutsidePoll,   // 팝업 밖 클릭 (가드 타이머 폴링)
  kCaptureLost,   // WM_CAPTURECHANGED
  kCaptureGone,   // 가드 타이머의 GetCapture 검사
  kEscape,
  kWinKey,
  kForeground,    // 포그라운드 변경
  kReopen,        // Open()이 이전 팝업을 닫음
  kExplicit,      // Close() 또는 Destroy()
};

void Dismiss(int invoke_index, DismissReason reason);
```

`Dismiss()` 안에서 다음을 기록하십시오.

```cpp
Log(L"popup", L"dismiss reason=%s index=%d", ReasonName(reason), invoke_index);
```

호출 지점마다 정확한 사유를 넘기십시오. 이 로그가 있어야 남은 문제를 근거로 좁힐 수 있습니다.

### 3-5. 바깥 클릭 감지를 캡처에 의존하지 않게 만든다 (필수)

2.2절의 이유로, 배경 창인 팝업에서 마우스 캡처는 바깥 클릭 감지 수단이 될 수 없습니다. 다음 두 가지를 적용하십시오.

**첫째, 가드 타이머의 검사 순서를 바꿉니다.**

현재 `OnGuardTimer()`는 첫 줄에서 `GetCapture() != hwnd_`이면 즉시 닫습니다. 배경 창에서는 캡처가 시스템에 의해 조용히 회수될 수 있고, 그 경우 사용자가 아무것도 하지 않았는데 메뉴가 닫힙니다. 반대로 캡처가 남아 있으면 바깥 클릭을 놓칩니다. 어느 쪽이든 신뢰할 수 없습니다.

`GetCapture()` 검사를 **닫기 조건에서 제거하고**, 버튼 눌림 폴링과 포그라운드 변경 검사만 남기십시오. 캡처는 팝업 위에서의 호버와 클릭을 안정적으로 받기 위한 보조 수단으로만 유지합니다.

**둘째, 가드 주기를 200ms에서 50ms로 줄입니다.**

폴링이 유일한 바깥 클릭 감지 수단이 되므로 200ms는 체감상 굼뜹니다. 팝업이 열려 있는 동안에만 도는 타이머이므로 상주 비용에 영향이 없습니다. `kPopupGuardMs`를 50으로 바꾸십시오.

### 3-6. 여기까지로 해결되지 않을 때만 진행한다 (조건부)

3-1부터 3-5까지 적용하고 3-4의 로그로 재현했는데도 메뉴가 닫히지 않는다면, 팝업이 열려 있는 동안에만 `WH_MOUSE_LL` 훅을 설치하는 방식으로 전환하십시오.

`PLAN.md` 5장 3항은 저수준 훅을 금지했습니다. 그 근거는 "캡처로 충분하다"였는데, 2.2절에서 그 전제가 틀렸음이 확인되었습니다. **따라서 이 항목에 한해 금지를 해제합니다.** 다만 다음 조건을 지키십시오.

- 훅은 `Open()`에서 설치하고 `Dismiss()`에서 **반드시** 해제합니다. 상주시키지 않습니다.
- 콜백에서는 좌표 비교와 `PostMessage`만 수행합니다. 파일 입출력, COM 호출, 동기 메시지 전송을 넣지 마십시오. 이 콜백은 시스템 전역 입력 경로에 있으므로, 여기서 지연되면 모든 앱의 마우스가 느려집니다.
- 훅이 설치되지 않아도(권한 문제 등) 팝업은 3-5의 폴링으로 계속 닫혀야 합니다.

먼저 3-1부터 3-5까지만 적용하고 검증하십시오. 3-6은 그 결과를 보고 판단합니다.

---

## 4. 하지 말아야 할 것

- 0장에 적힌 기존 수정 네 가지를 되돌리지 마십시오. 1장의 로그가 효과를 입증합니다.
- `Open()`에 `target_.Reset()`을 다시 넣지 마십시오. 이것이 원래 1초의 원인이었습니다.
- `SetForegroundWindow`와 `AttachThreadInput`을 도입하지 마십시오. `PLAN.md` 5장 2항이 그대로 유효합니다.
- `spotlight.cpp`와 `spotlight.hpp`의 미커밋 수정을 건드리지 마십시오.
- 3-6을 먼저 하지 마십시오. 3-1이 두 증상을 모두 설명하므로, 훅부터 넣으면 실제 원인을 덮어쓴 채 복잡도만 늘어납니다.

---

## 5. 검증

각 항목은 `~/.bamti/bamti.log`와 실제 조작으로 확인합니다.

**지연**
- [ ] `[popup] open ... shown` 값이 **16ms 이하**이다.
- [ ] 메뉴를 열 때 `[popup] create render target`이 찍히지 않는다. 시작 시 두 번만 찍힌다.
- [ ] 검은 빈 창이 보이는 순간이 없다.

**닫힘**
- [ ] 메뉴를 연속으로 다섯 번 열고 닫아도 매번 닫힌다. 두 번째부터 멈추지 않는다.
- [ ] 첫 메뉴에서 `모두 보기`나 창 활성화를 실행한 **직후**에 연 메뉴도 정상으로 뜨고 닫힌다. 이 조건이 이번 재현의 핵심입니다.
- [ ] 바탕화면 클릭, 다른 앱 클릭, 상단바 클릭, `ESC`, `Alt+Tab`, `Win` 키에서 각각 닫힌다.
- [ ] 닫힐 때마다 `[popup] dismiss reason=...`이 남고, 사유가 실제 조작과 일치한다.
- [ ] 아무 조작도 하지 않고 메뉴를 10초간 두었을 때 저절로 닫히지 않는다.

**회귀**
- [ ] 독 아이콘 위에서 마우스를 움직이면 행 강조가 즉시 따라온다.
- [ ] 상단바 상태 항목 팝업 패널도 같은 기준을 만족한다. 같은 `PopupSurface`를 쓰므로 함께 확인해야 합니다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 6. 커밋

수정 내용을 하나의 커밋으로 만드십시오. 0장의 기존 미커밋 수정도 함께 담습니다.

```
fix: 독 우클릭 메뉴가 늦게 뜨고 닫히지 않는 문제를 고친다
```

본문에는 다음을 적으십시오.

- `WM_PAINT`가 큐에서 밀려 창이 늦게 그려졌다는 점과 `UpdateWindow()`로 해결했다는 점
- 배경 창의 `SetCapture`가 바깥 클릭을 받지 못한다는 점과, 그래서 `PLAN.md` 규칙 2의 전제를 폴링으로 대체했다는 점
- 3-6을 적용했다면 저수준 훅 금지를 해제한 근거와, 훅 수명을 팝업이 열린 동안으로 한정했다는 점
