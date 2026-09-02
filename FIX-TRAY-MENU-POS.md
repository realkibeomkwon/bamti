# 작업 지시서 23: 트레이 앱 메뉴가 화면 하단에서 열리는 문제

Tailscale 아이콘을 상단바에서 눌렀을 때 메뉴가 상단바가 아니라 화면 하단, 곧 원래 알림 영역이 있던 자리에서 나타납니다. 이 지시서는 먼저 원인을 로그로 확정한 뒤, 원인이 무엇이든 통하는 보정 장치를 넣습니다.

---

## 1. 지금까지 확정된 사실

`%USERPROFILE%\.bamti\bamti.log.old`의 26520~26521행 같은 로스터 줄을 보면, Tailscale은 **가로채기 백엔드가 잡고 있습니다.**

```
[tray] intercept roster n=7 tips="오피스키퍼|Everything|헤드폰(Bluetooth): 30%|(no tip)|
                                  KakaoTalk|Tailscale: Connected. Click for options.|Bluetooth 장치"
[tray] uia       roster n=15 tips="숨겨진 아이콘 표시|KakaoTalk|Tailscale: ...
```

따라서 클릭은 UI Automation이 아니라 `src/tray_intercept.cpp`의 `Invoke`(347행)를 거칩니다. 이 함수는 상단바에서 그 아이콘이 차지하는 사각형을 `rect_lookup_`으로 얻어 그 중심 좌표를 메시지에 실어 보냅니다(388~403행).

```cpp
    if (have_rect) {
      pt.x = (rc.left + rc.right) / 2;
      pt.y = (rc.top + rc.bottom) / 2;
    } else {
      GetCursorPos(&pt);
    }
```

`rect_lookup_`은 `src/menu_bar.cpp` 313행에서 상단바 세그먼트의 화면 좌표를 돌려주도록 걸려 있습니다. 즉 **우리가 보내는 좌표는 상단바 위입니다.** 그런데도 메뉴가 하단에서 열린다면 남은 가능성은 셋뿐입니다.

1. 대상 앱이 메시지에 실린 좌표를 쓰지 않고 `Shell_NotifyIconGetRect`로 아이콘 위치를 다시 물어봅니다. 이 경로는 `AnswerGetRect`(873행)가 답하고 있지만, 실제로 호출되는지 여부가 로그에 남지 않습니다.
2. 대상 앱이 좌표를 아예 무시하고 작업 표시줄 위치(`SHAppBarMessage`의 `ABM_GETTASKBARPOS`)를 기준으로 메뉴를 놓습니다. 이 경로는 우리가 답할 수단이 없습니다.
3. 그 아이콘만 가로채기에서 빠져 UIA 경로로 넘어간 세션이었습니다. UIA의 `Invoke()`는 셸의 실제 트레이 버튼을 누르므로 좌표를 실을 수단이 아예 없습니다.

세 경우 모두 **메뉴가 뜬 뒤에 위치를 바로잡는 보정**으로 덮을 수 있습니다. 2절에서 원인을 가릴 로그를 넣고, 3절에서 보정을 구현합니다. 둘 다 이번 작업 범위입니다.

---

## 2. 진단 로그부터 넣는다

### 2-1. 어떤 좌표를 보냈는지 남긴다

`src/tray_intercept.cpp`의 `PostNotify`(284행)는 좌표를 로그에 남기지 않습니다. `Invoke`가 좌표를 정한 직후(403행 뒤)에 한 줄을 추가하십시오.

```cpp
    Log(L"tray", L"intercept invoke pt key=0x%llX have_rect=%d pt=%ld,%ld rc=%ld,%ld,%ld,%ld",
        static_cast<unsigned long long>(key), have_rect ? 1 : 0, pt.x, pt.y, rc.left, rc.top, rc.right, rc.bottom);
```

`have_rect=0`이 찍히면 `rect_lookup_`이 그 아이콘을 찾지 못한 것이고, 그때는 그것이 곧 원인입니다.

### 2-2. `Shell_NotifyIconGetRect` 응답을 남긴다

`AnswerGetRect`(873행)는 지금 아무 로그도 남기지 않습니다. 반환 직전에 한 줄을 넣으십시오. 이 함수는 자주 불릴 수 있으므로 **직전 호출과 1초 이상 벌어졌을 때만** 남기고, 나머지는 조용히 넘어가게 하십시오.

```cpp
    Log(L"tray", L"intercept getrect key=0x%llX found=%d axis=%s v=%ld",
        static_cast<unsigned long long>(key), found ? 1 : 0, id.message == 2 ? L"y" : L"x", v);
```

이 줄이 클릭 직후에 찍히면 원인은 1번이고, 찍히지 않으면 1번이 아닙니다.

### 2-3. 메뉴 창이 어디에 떴는지 남긴다

3절에서 만드는 보정 장치가 후보 창을 판정할 때마다 로그를 남기게 하십시오. 형식은 3-4절에 적었습니다.

---

## 3. 보정: 뜬 메뉴를 아이콘 아래로 옮긴다

### 3-1. 새 파일 두 개를 만든다

`src/tray_popup_guard.hpp`

```cpp
#pragma once

#include <windows.h>

namespace bamti {

// 트레이 아이콘을 대신 눌러 준 직후, 대상 앱이 띄우는 팝업이 상단바가 아니라 원래
// 알림 영역(화면 하단) 근처에서 열리는 경우가 있다. 짧은 시간 동안 새로 나타나는
// 최상위 팝업을 지켜보다가 그런 창을 아이콘 바로 아래로 옮긴다.
//
// anchor: 상단바에서 그 아이콘이 차지하는 화면 좌표 사각형.
// owner_pid: 아이콘을 등록한 프로세스. 모르면 0을 넘긴다(프로세스를 가리지 않는다).
//
// 반드시 메시지 펌프를 도는 UI 스레드에서 부를 것. WinEvent 훅은 설치한 스레드로만
// 콜백을 보낸다.
void TrayPopupGuardArm(const RECT& anchor, DWORD owner_pid);

// 프로세스를 내릴 때 훅과 타이머를 정리한다.
void TrayPopupGuardShutdown();

}  // namespace bamti
```

`src/tray_popup_guard.cpp`의 구현 규칙은 다음과 같습니다.

- 상태는 파일 지역 정적 변수로 둡니다. 무장 만료 시각(`ULONGLONG armed_until`), 앵커 사각형, 대상 pid, 훅 핸들 둘, 타이머 id, 이번 무장에서 옮긴 창 수를 들고 있으면 됩니다.
- `TrayPopupGuardArm`은 훅이 없으면 설치하고, 이미 있으면 앵커와 pid만 갱신하고 만료 시각을 늘립니다. 무장 시간은 **2000ms**로 두십시오. 앱이 꺼져 있어 프로세스가 새로 뜨는 경우까지 덮으려면 이 정도가 필요합니다.
- 훅은 두 개를 겁니다. 표준 메뉴는 `EVENT_SYSTEM_MENUPOPUPSTART`로, 앱이 자기 창으로 그리는 팝업은 `EVENT_OBJECT_SHOW`로 잡힙니다.

```cpp
  hook_show_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &GuardProc, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  hook_menu_ = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART, nullptr, &GuardProc, 0, 0,
                               WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
```

- 해제는 `SetTimer(nullptr, 0, ...)`로 건 스레드 타이머에서 합니다. 만료됐거나 창을 한 개 옮겼으면 훅 둘을 `UnhookWinEvent`로 떼고 타이머를 죽입니다. 훅을 계속 걸어 두면 안 됩니다. 전역 `EVENT_OBJECT_SHOW`는 트래픽이 많아 상주 비용으로 적합하지 않습니다.

### 3-2. 후보 판정

`GuardProc`은 다음을 모두 만족하는 창만 후보로 봅니다. 하나라도 어긋나면 아무 일도 하지 않습니다.

1. `idObject == OBJID_WINDOW && idChild == CHILDID_SELF`이고 `hwnd != nullptr`입니다.
2. `GetAncestor(hwnd, GA_ROOT) == hwnd`입니다(최상위 창).
3. `IsWindowVisible(hwnd) != FALSE`입니다.
4. `GetWindowRect`가 성공하고 폭과 높이가 모두 0보다 큽니다.
5. 무장 중이고(`GetTickCount64() < armed_until`) 아직 이번 무장에서 옮긴 창이 없습니다.
6. `owner_pid`가 0이 아니면 창의 프로세스 id가 그 값과 같아야 합니다.
7. 창 사각형이 화면을 거의 다 덮지 않아야 합니다. 폭이 그 모니터 작업 영역 폭의 90%를 넘거나 높이가 90%를 넘으면 메뉴가 아니라 일반 창이므로 제외합니다.
8. **아래쪽에 떴어야 합니다.** 창 사각형의 세로 중심이 앵커가 놓인 모니터 작업 영역의 세로 중심보다 아래면 참입니다. 이 조건이 있어야 이미 제자리에 뜬 메뉴를 건드리지 않습니다.

### 3-3. 이동

```cpp
  MONITORINFO mi{sizeof(mi)};
  const HMONITOR mon = MonitorFromRect(&anchor_, MONITOR_DEFAULTTONEAREST);
  GetMonitorInfoW(mon, &mi);

  const LONG w = rc.right - rc.left;
  const LONG h = rc.bottom - rc.top;
  const LONG gap = MulDiv(6, static_cast<int>(GetDpiForSystem()), 96);   // 6 DIP
  LONG x = (anchor_.left + anchor_.right) / 2 - w / 2;
  LONG y = anchor_.bottom + gap;
  x = (std::max)(mi.rcWork.left, (std::min)(x, mi.rcWork.right - w));
  y = (std::max)(mi.rcWork.top, (std::min)(y, mi.rcWork.bottom - h));
  SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
```

크기는 바꾸지 마십시오. 표준 메뉴는 화면 좌표로 히트 테스트를 하므로 창을 옮기면 마우스 반응도 함께 따라옵니다.

### 3-4. 로그

옮겼을 때와, 무장 중에 후보를 걸렀을 때 모두 남깁니다. 거른 쪽은 이번 무장에서 **최대 다섯 줄까지만** 남기십시오. 전역 `EVENT_OBJECT_SHOW`는 짧은 시간에도 수십 건이 옵니다.

```
[tray] popup move cls=#32768 pid=1234 from=1500,1300,320,220 to=1180,44
[tray] popup skip cls=Chrome_WidgetWin_1 pid=1234 reason=not-bottom rc=100,200,800,600
```

`reason`은 `not-toplevel`, `hidden`, `pid`, `too-big`, `not-bottom` 가운데 하나로 적습니다.

### 3-5. CMakeLists

`src/tray_popup_guard.cpp`를 `add_executable` 목록에 넣으십시오. 새 라이브러리는 필요하지 않습니다. `SetWinEventHook`은 이미 링크된 `user32`에 있습니다.

---

## 4. 무장 지점을 잇는다

### 4-1. 아이콘을 등록한 프로세스 id를 알아낸다

가로채기가 잡은 항목은 소유 창을 알고 있으므로 pid를 정확히 넘길 수 있습니다. UIA만 아는 항목은 알 수 없으므로 0을 넘깁니다.

`src/tray_mirror.hpp`의 `ItemState`에 필드를 하나 더합니다.

```cpp
    DWORD owner_pid = 0;
```

항목을 갱신하는 자리에서 `TrayIconInfo::owner`가 유효하면 `GetWindowThreadProcessId`로 채우고, 아니면 0으로 둡니다. 그리고 UI 스레드가 읽을 수 있도록 공개 함수를 더합니다. 내부 뮤텍스로 보호하십시오. **워커 스레드가 쓰는 백엔드 객체를 UI 스레드에서 직접 만지지 마십시오.**

```cpp
  DWORD OwnerPid(uint64_t key) const;   // 모르면 0
```

### 4-2. `menu_bar.cpp`에서 무장한다

무장할 자리는 두 곳입니다. 둘 다 `hit->id`가 `"bamti.tray/"`로 시작할 때만 무장해야 합니다. 상단바의 내장 위젯은 우리가 직접 팝업을 그리므로 이 보정이 끼어들면 안 됩니다.

1. **좌클릭** — `WM_LBUTTONUP` 처리에서 `status_.Dispatch(ev)`를 부르기 직전(632행 부근). 지금 이 경로에는 트레이 항목인지 가리는 검사가 없으므로 새로 넣으십시오. 트레이 항목이면 무장한 뒤 `Dispatch`를 부르고, `OpenStatusPanel(*hit)`은 지금처럼 이어서 부르면 됩니다.
2. **우클릭** — `WM_RBUTTONUP`의 `hit->id.rfind("bamti.tray/", 0) == 0` 분기에서 `tray_.ForwardsContextMenu()`가 참일 때(679행 부근), `Dispatch` 직전입니다.

앵커 사각형은 313행의 `SetRectLookup` 람다와 같은 방식으로 구합니다. 같은 코드를 두 번 쓰지 말고 **`bool SegmentScreenRect(const std::string& id, RECT* out) const` 같은 비공개 멤버 함수로 빼서** 람다와 무장 지점이 함께 쓰게 하십시오.

```cpp
  RECT anchor{};
  if (SegmentScreenRect(hit->id, &anchor)) {
    TrayPopupGuardArm(anchor, tray_.OwnerPid(TrayMirror::ParseId(hit->id)));
  }
```

`Dispatch`는 워커 스레드에 일감을 넘길 뿐이라 실제 클릭 전달은 조금 뒤에 일어납니다. 그러니 **무장을 먼저 하고 그다음에 `Dispatch`를 부르는 순서**를 지켜야 합니다.

### 4-3. 종료 처리

`MenuBar`가 창을 파괴하는 자리(`WM_DESTROY` 처리)에서 `TrayPopupGuardShutdown()`을 부르십시오.

---

## 5. 검증

빌드한 뒤 앱을 다시 띄우고 다음을 순서대로 확인합니다.

1. 상단바의 Tailscale 아이콘을 **좌클릭**합니다. 메뉴가 아이콘 바로 아래에서 열려야 합니다.
2. 같은 아이콘을 **우클릭**합니다. 마찬가지로 아이콘 아래에서 열려야 합니다.
3. 메뉴 항목을 실제로 눌러 봅니다. 옮긴 뒤에도 항목이 정상으로 실행되고, 바깥을 누르면 닫혀야 합니다. 이 항목이 깨지면 창을 옮긴 방식이 잘못된 것입니다.
4. KakaoTalk, Everything, 오피스키퍼 아이콘도 같은 방식으로 눌러 봅니다. **원래 제자리에서 잘 열리던 앱의 메뉴가 이동 때문에 어긋나면 안 됩니다.** 3-2절 8번 조건이 그 보호 장치이므로, 어긋난다면 그 판정을 다시 보십시오.
5. 상단바의 배터리, 볼륨, 네트워크, 제어 센터 팝업을 열어 봅니다. 위치가 그대로여야 합니다. 무장 조건에서 `bamti.tray/` 검사가 빠지면 여기서 티가 납니다.
6. 로그에서 다음을 확인해 원인을 확정하고, 그 내용을 완료 보고에 적어 주십시오.
   - `intercept invoke pt ... have_rect=?` 값
   - `intercept getrect` 줄의 유무
   - `popup move` 또는 `popup skip` 줄의 내용
7. 아무 동작도 하지 않는 동안 `popup move`나 `popup skip`이 로그에 쌓이지 않는지 확인합니다. 훅이 제때 풀리는지 보는 항목입니다.

---

## 6. 주의 사항

- 훅을 상주로 걸지 마십시오. 전역 `EVENT_OBJECT_SHOW`는 창이 뜰 때마다 오는 이벤트라 상주 비용으로 적합하지 않습니다. 무장한 2초 동안만 걸고 반드시 떼십시오.
- `WINEVENT_SKIPOWNPROCESS`를 빠뜨리면 우리가 그리는 팝업까지 후보로 들어옵니다.
- 커서를 옮기는 방식(`SetCursorPos`)으로 우회하지 마십시오. 사용자 입력 장치를 대신 움직이는 방법은 쓰지 않습니다.
- 작업 표시줄을 다시 보이게 하거나 위치를 바꾸는 방식도 쓰지 마십시오. 이번 범위 밖입니다.
- 원인이 2-2절 로그로 "앱이 `Shell_NotifyIconGetRect`를 쓴다"로 확정되더라도 3절의 보정은 그대로 두십시오. UIA 경로로 넘어간 항목에는 그 응답이 닿지 않습니다.
