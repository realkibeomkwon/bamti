# 작업 지시서: 상단바가 화면 캡처 오버레이 위로 다시 올라오는 것을 막는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/menu_bar.cpp` 하나이고, 바꾸는 줄은 한 줄입니다. 다만 잘못 고치기 쉬운 자리라서 배경과 판정 기준을 길게 적습니다.

## 증상

`Win + Shift + S` 로 화면 영역을 캡처할 때, 상단바가 있는 위쪽 띠에서는 마우스 클릭이 스니핑 오버레이에 닿지 않습니다. 그래서 그 자리에서 영역 선택을 시작할 수 없고, 상단바가 캡처에 들어가지 않습니다. 항상 그런 것은 아니고 될 때도 있습니다.

## 측정으로 밝힌 원인

오버레이가 떠 있는 동안 창의 z 순서를 150밀리초 간격으로 기록했습니다. 세 번의 캡처 시도 가운데 두 번째에서 원인이 그대로 드러났습니다.

```
[21:55:27.274] z-order(top first):
   0. SnippingTool XamlWindow TOPMOST rect=0,0,5120,2160 ex=0x200188
   1. SnippingTool SnipOverlayRootWindow TOPMOST
   2. bamti bamti.MenuBar TOPMOST rect=0,0,5120,48 ex=0x8080088
   at (300,20) = SnippingTool : XamlWindow      <- 클릭이 오버레이로 간다

[21:55:27.539] z-order(top first):
   0. bamti bamti.MenuBar TOPMOST rect=0,0,5120,48 ex=0x8080088
   1. bamti bamti.DockHot TOPMOST
   2. SnippingTool XamlWindow TOPMOST rect=0,0,5120,2160 ex=0x200188
   at (300,20) = bamti : bamti.MenuBar          <- 클릭이 상단바에 막힌다
```

**오버레이는 처음에 상단바보다 위에 올라옵니다. 그런데 265밀리초 뒤에 상단바가 다시 오버레이 위로 올라옵니다.** 그 순간부터 위쪽 48픽셀 띠의 마우스 입력은 전부 bamti 가 가져갑니다. 사용자가 그 안에서 끌기를 시작하지 못하는 이유가 이것입니다.

증상이 들쭉날쭉한 것도 이 때문입니다. 사용자가 265밀리초 안에 끌기를 시작하면 성공하고, 늦으면 막힙니다. 경합입니다.

### 상단바를 다시 올리는 자리

`MenuBar::Layout()` 의 마지막 줄입니다.

```cpp
// src/menu_bar.cpp:1475
SetWindowPos(hwnd_, HWND_TOPMOST, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left,
             abd.rc.bottom - abd.rc.top, SWP_NOACTIVATE);
```

창은 이미 `WS_EX_TOPMOST` 로 만들어졌으므로(`src/menu_bar.cpp:552`), 이 호출은 최상위 속성을 새로 주는 것이 아닙니다. **최상위 창들끼리의 순서에서 맨 앞으로 끌어올리는 일만 합니다.** 오버레이도 최상위이므로, 이 한 번의 호출로 상단바가 오버레이를 덮습니다.

오버레이가 뜨는 순간 `Layout()` 이 불리는 경로는 둘입니다. 둘 다 화면 구성이 바뀔 때 셸이 보내는 알림입니다.

```cpp
// src/menu_bar.cpp:1325 — 셸이 작업 표시줄 영역 변화를 알릴 때
case ABN_POSCHANGED:
  NoteWorkAreaWait(L"appbar");
  Layout();
  break;
```

```cpp
// src/menu_bar.cpp:808 — WM_SETTINGCHANGE 와 WM_DISPLAYCHANGE 에서
ReserveWorkArea();   // 안에서 Layout() 을 부른다
```

### 숨김 장치가 왜 안 걸리는가

bamti 에는 전체 화면 앱이 뜨면 스스로 숨는 장치가 있지만, 오버레이에는 걸리지 않습니다. 측정된 오버레이의 확장 스타일이 `ex=0x200188` 이라서 `WS_EX_TOOLWINDOW`(0x80)가 켜져 있고, 판정 함수가 그 조건에서 곧바로 물러납니다.

```cpp
// src/fullscreen.cpp:226
const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
if ((ex & WS_EX_NOACTIVATE) != 0 || (ex & WS_EX_TOOLWINDOW) != 0) {
  return false;
}
```

**그러나 이쪽은 고치지 마십시오.** 사용자가 원하는 것은 상단바가 캡처에 **들어가는** 것입니다. 숨김 장치가 걸리면 상단바가 사라져서 캡처에 아예 담기지 않으므로, 요구와 반대입니다. 상단바는 보이되 오버레이 아래에 있어야 합니다.

---

## 수정

`MenuBar::Layout()` 의 `SetWindowPos` 가 z 순서를 건드리지 않게 합니다.

```cpp
SetWindowPos(hwnd_, nullptr, abd.rc.left, abd.rc.top, abd.rc.right - abd.rc.left,
             abd.rc.bottom - abd.rc.top, SWP_NOACTIVATE | SWP_NOZORDER);
```

바꾸는 것은 두 가지입니다. 세 번째 인자를 `HWND_TOPMOST` 에서 `nullptr` 로 바꾸고, 플래그에 `SWP_NOZORDER` 를 더합니다. 위치와 크기를 맞추는 일은 그대로 하고 순서만 건드리지 않습니다.

**창의 최상위 속성은 그대로 남습니다.** `WS_EX_TOPMOST` 는 창을 만들 때 준 것이고 이 변경으로 사라지지 않으므로, 상단바는 여전히 일반 창들 위에 있습니다. 달라지는 것은 다른 최상위 창이 상단바를 덮을 수 있게 된다는 점뿐이며, 화면 캡처 오버레이에 대해서는 그것이 옳은 동작입니다. Windows 작업 표시줄도 같은 방식으로 오버레이에 덮입니다.

`Layout()` 을 부르는 네 자리 가운데 어느 곳도 z 순서를 올릴 목적으로 부르지 않습니다. 창 생성 직후(600행), `WM_DPICHANGED`(783행), `ABN_POSCHANGED`(1327행), `ReserveWorkArea`(1493행) 전부 위치와 크기를 맞추려는 호출입니다.

---

## 검증

### 빌드

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

### bamti 재시작

**새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. 상단바 창(클래스 `bamti.MenuBar`)에 `WM_COMMAND` 로 명령 1번을 보내면 정리 절차를 거쳐 끝납니다.

### 판정 기준

**눈으로 보고 판정하지 마십시오.** 이 문제는 경합이라 몇 번은 우연히 성공합니다. z 순서를 기록해서 판정해야 합니다.

1. 오버레이가 떠 있는 **모든** 표본에서 `SnippingTool` 의 창이 `bamti.MenuBar` 보다 앞에 있어야 합니다. 한 표본이라도 순서가 뒤집히면 실패입니다.
2. 같은 구간에서 `WindowFromPoint(300, 20)` 이 계속 `SnippingTool` 을 돌려줘야 합니다. 한 번이라도 `bamti.MenuBar` 가 나오면 실패입니다.
3. 오버레이를 닫은 뒤에는 상단바가 다시 일반 창들 위에 있어야 합니다. 크롬 같은 창을 최대화해서 상단바가 가려지지 않는지 봅니다.
4. 상단바의 위치와 높이가 그대로여야 합니다. 작업 영역도 그대로 예약되어 다른 창이 상단바 아래에서 최대화되어야 합니다.

측정 스크립트는 `C:\Users\KIBEOMKWON\AppData\Local\Temp\claude\D--repos-bamti\6924051f-cc12-4917-af69-1633a195fefc\scratchpad\zwatch2.ps1` 에 있습니다. 그대로 돌리면 창 순서가 바뀔 때마다 `zwatch2.log` 에 기록합니다. **다만 `Win + Shift + S` 를 누르는 것은 사용자에게 부탁하십시오. 사용자 화면에 입력을 합성하지 마십시오.**

### 그래도 상단바가 다시 올라온다면

`Layout()` 말고도 셸이 앱바를 끌어올리는 경로가 두 개 더 있습니다.

```cpp
// src/menu_bar.cpp:811
case WM_WINDOWPOSCHANGED:
  SHAppBarMessage(ABM_WINDOWPOSCHANGED, &abd);

// src/menu_bar.cpp:818
case WM_ACTIVATE:
  SHAppBarMessage(ABM_ACTIVATE, &abd);
```

**이 둘을 미리 손대지 마십시오.** 위의 한 줄만 고치고 먼저 측정한 뒤, 그래도 순서가 뒤집히면 그때 이 경로를 의심하십시오. 두 메시지는 앱바가 작업 영역을 유지하는 데 쓰이므로 함부로 빼면 상단바 아래로 창이 최대화되지 않게 됩니다.

---

## 이번 작업에서 하지 않는 것

- `src/fullscreen.cpp` 의 `IsTrueFullscreen` 은 건드리지 않습니다. 위에 적은 대로, 숨기는 것은 요구와 반대입니다.
- `ABM_WINDOWPOSCHANGED` 와 `ABM_ACTIVATE` 는 측정 결과가 나오기 전에는 손대지 않습니다.
- 독(`bamti.Dock`, `bamti.DockHot`)의 z 순서 처리는 이번 범위가 아닙니다. 같은 증상이 화면 아래쪽에도 있는지는 아직 확인하지 않았습니다.
