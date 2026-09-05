# 작업 지시서: Ctrl을 누른 채 상단바 오른쪽 끝에 올리면 바탕 화면을 미리 본다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

앞의 세 건과 겹치는 파일이 `src/menu_bar.cpp` 하나이므로 **`TASK-BAR-WINX-MENU.md` 다음에 하십시오.**

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, 새로 만드는 `src/live_preview.hpp`와 `src/live_preview.cpp`, `CMakeLists.txt`, `bamti.vcxproj`입니다.

---

## 1. 가능한가

**가능합니다.** 두 가지를 확인했습니다.

1. **자리가 비어 있습니다.** `src/bar_layout.cpp`의 `kPadRightDip = 14.0f`가 시계 오른쪽에 14 DIP의 빈 여백을 남깁니다. 여기에 감지 영역을 두면 시계를 비롯한 어떤 세그먼트의 적중 판정과도 겹치지 않습니다.
2. **미리 보기 API가 이 컴퓨터에 있습니다.** `C:\Windows\System32\dwmapi.dll`의 내보내기를 `dumpbin -exports`로 확인한 결과, **서수 113이 이름 없는 내보내기(`[NONAME]`, RVA `0x00009AA0`)로 존재합니다.** Windows 11 빌드 26200입니다. 작업 표시줄이 바탕 화면 미리 보기에 쓰는 `DwmpActivateLivePreview`가 이 서수입니다.

   다만 **서수가 있다는 사실이 그 함수라는 증명은 아닙니다.** 문서화되지 않은 API이므로 서명도 공표된 것이 없습니다. 그래서 2절에 시험 절차를 따로 두었습니다.

---

## 2. 미리 보기 API를 먼저 시험한다

**구현을 시작하기 전에 이 절부터 하십시오.** 여기서 실패하면 4절의 대체 경로로 갑니다.

`src/live_preview.cpp`에서 `dwmapi.dll`을 `GetModuleHandleW`로 잡고(이미 링크되어 있으므로 새로 `LoadLibrary`할 필요가 없습니다) `GetProcAddress(dwm, MAKEINTRESOURCEA(113))`으로 함수를 얻습니다.

서명을 두 형태로 시험하십시오. Windows 8 이후로 인자가 하나 늘었다고 알려져 있습니다.

```cpp
using Fn5 = HRESULT(WINAPI*)(BOOL activate, HWND exclude, HWND insert_before, UINT trigger, RECT* final_rect);
using Fn4 = HRESULT(WINAPI*)(BOOL activate, HWND exclude, HWND insert_before, UINT trigger);
```

다섯 인자 형태를 먼저 시험합니다. `activate=TRUE`, `exclude=상단바 hwnd`, `insert_before=nullptr`, `trigger`는 0부터 3까지, `final_rect=nullptr`로 각각 불러 보고 **반환값과 화면 변화를 로그에 남기십시오.**

```cpp
Log(L"peek", L"live preview probe arity=%d trigger=%u hr=0x%08lX", 5, trigger, hr);
```

x64 호출 규약에서는 인자가 남아도 스택이 깨지지 않으므로 두 형태를 모두 시험해도 안전합니다. 그래도 **시험 코드는 진단용으로만 두고, 어느 조합이 실제로 창들을 투명하게 만드는지 확인한 뒤에는 그 조합 하나만 남기십시오.**

확인해야 할 것이 하나 더 있습니다. **미리 보기가 켜졌을 때 독(`Dock`)이 함께 사라지는지 보십시오.** 이 API는 남길 창을 하나만 받습니다. 상단바를 넘기면 독은 다른 창들과 함께 투명해질 수 있습니다. 결과를 로그와 화면으로 남기고 **작업 보고에 적으십시오.** 그 처리를 어떻게 할지는 사용자가 판단합니다. 임의로 독을 숨기거나 최상위로 올리지 마십시오.

`activate=FALSE`로 반드시 되돌아오는지도 함께 확인하십시오. 되돌리지 못하면 화면이 투명한 채로 남습니다. **이 경우를 대비해 앱 종료 경로(`WM_DESTROY`)에서도 무조건 해제를 부르십시오.**

---

## 3. 감지와 해제

### 3-1. 켜는 조건

`WM_MOUSEMOVE`에서 다음이 모두 참일 때만 시작합니다.

1. `fullscreen_occluded_`가 거짓이다.
2. `reorder_active_`가 거짓이다. Ctrl을 쓰는 기능이 이미 있으므로 겹치면 안 됩니다.
3. `status_popup_`도 `bar_submenu_popup_`도 열려 있지 않다.
4. `(GetKeyState(VK_CONTROL) & 0x8000) != 0`이다.
5. `pt.x >= client.right - DipToPx(kPeekZoneDip, Dpi())`이다. `constexpr int kPeekZoneDip = 8;`로 시작하십시오.

조건이 맞으면 **바로 켜지 말고** `SetTimer(hwnd_, kPeekDwellTimerId, 300, nullptr)`로 300밀리초를 기다립니다. 지나가는 마우스에 화면이 번쩍이면 안 됩니다. 조건이 깨지면 타이머를 죽입니다. Windows의 작업 표시줄도 같은 방식으로 뜸을 들입니다.

### 3-2. 끄는 조건

넷 중 하나라도 생기면 즉시 해제합니다.

1. `WM_MOUSEMOVE`에서 커서가 영역을 벗어났다.
2. `WM_MOUSELEAVE`가 왔다. **`ArmMouseLeave`가 실제로 걸려 있는지 확인하십시오.** 영역에 들어갈 때마다 다시 걸어야 합니다. `TME_LEAVE`는 한 번 발화하면 풀립니다.
3. Ctrl에서 손을 뗐다. **상단바는 `WS_EX_NOACTIVATE`라 키보드 메시지를 받지 못하므로 눌러서 알 수 없습니다.** 미리 보기가 켜져 있는 동안만 도는 `SetTimer(hwnd_, kPeekPollTimerId, 100, nullptr)`를 두고 `GetAsyncKeyState(VK_CONTROL)`을 확인하십시오. 해제할 때 타이머도 함께 죽입니다.
4. 상단바에서 팝업이 열렸거나 전체 화면 창이 상단바를 덮었다.

상태는 `bool peek_active_ = false;` 하나로 들고, 켜고 끄는 것을 **반드시 이 깃발을 거쳐서** 하십시오. 이미 켜져 있는데 또 켜거나 꺼져 있는데 또 끄면 안 됩니다.

`WM_SETCURSOR`에서 이 영역에 있고 Ctrl이 눌렸을 때 어떤 커서를 줄지는 정하지 않았습니다. **기본 화살표를 그대로 두십시오.**

---

## 4. 미리 보기가 안 될 때

2절에서 어떤 조합으로도 화면이 반응하지 않으면, **호버로는 아무 일도 일어나지 않게 두십시오.** 다음 대체 경로를 호버에 붙이면 안 됩니다.

대체 경로는 문서화된 `IShellDispatch4::ToggleDesktop`입니다. `Shell.Application`(`CLSID_Shell`)을 만들어 부릅니다. 이것은 미리 보기가 아니라 **모든 창을 실제로 최소화했다가 되돌리는 토글**입니다. 마우스가 스칠 때마다 창이 내려갔다 올라오면 쓸 수 없습니다.

그래서 이 경우에는 **같은 영역에서 Ctrl을 누른 채 왼쪽 단추를 눌렀을 때만** `ToggleDesktop`을 부르도록 하고, 그 사실을 작업 보고에 적으십시오. 호버 동작은 포기합니다.

---

## 5. 검증

1. **빌드.** Release 클린 빌드가 경고 없이 통과해야 합니다.
2. **탐침 결과.** 2절의 로그를 그대로 보고에 붙이십시오. 어느 인자 개수와 어느 `trigger` 값이 동작했는지, 반환값이 무엇이었는지 적습니다.
3. **켜짐.** 창을 몇 개 띄운 상태에서 Ctrl을 누른 채 상단바 오른쪽 끝에 올리면 300밀리초 뒤에 창들이 투명해지고 바탕 화면이 보여야 합니다.
4. **꺼짐 네 가지.** 마우스를 옆으로 옮겼을 때, 상단바 밖으로 나갔을 때, Ctrl에서 손을 뗐을 때, 그리고 그 상태에서 우클릭해 메뉴를 열었을 때 각각 원래 화면으로 돌아와야 합니다.
5. **되돌리기 누락 없음.** 미리 보기가 켜진 채로 bamti를 종료해도 화면이 투명하게 남지 않아야 합니다.
6. **기존 Ctrl 기능 회귀.** Ctrl을 누른 채 위젯을 끌어 순서를 바꾸는 기능이 그대로 동작해야 합니다. 끄는 도중에 오른쪽 끝까지 끌고 가도 미리 보기가 켜지면 안 됩니다.
7. **독.** 미리 보기 중에 독이 보이는지 사라지는지 화면으로 확인하고 보고에 적으십시오.

레지스트리에 쓰는 확인 절차는 넣지 마십시오. 전원이나 세션에 손대는 확인 절차도 넣지 마십시오.
