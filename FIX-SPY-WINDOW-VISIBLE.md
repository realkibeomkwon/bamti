# 작업 지시서 28: 부팅 후 스파이 창이 화면에 보인다

부팅 직후 제목 표시줄에 `Shell_TrayWnd`라고 적힌 빈 흰 창이 바탕 화면에 떠 있습니다. 이 창은 explorer의 작업 표시줄이 아니라 **우리가 만든 트레이 가로채기 스파이 창**입니다.

---

## 1. 이 창이 우리 것이라는 근거

`src/tray_intercept.cpp` 547행이 스파이 창을 만듭니다.

```cpp
    HWND spy = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSpyClass, kSpyClass, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                                wc.hInstance, this);
```

세 가지가 화면에 보이는 창과 정확히 일치합니다.

1. **창 제목이 `Shell_TrayWnd`입니다.** 두 번째 `kSpyClass` 인자가 창 제목으로 들어갑니다(31행에서 `constexpr wchar_t kSpyClass[] = L"Shell_TrayWnd";`). explorer의 진짜 작업 표시줄은 제목이 비어 있으므로, 제목 표시줄에 이 문자열이 보이는 창은 우리 것뿐입니다.
2. **`WS_OVERLAPPEDWINDOW`라서 제목 표시줄과 굵은 테두리가 있습니다.** 화면의 창 모양이 이것입니다.
3. **`WS_EX_TOOLWINDOW`라서 제목 표시줄이 얇습니다.** 화면의 제목 표시줄이 일반 창보다 얇은 것이 이 때문이고, 작업 전환 목록에도 나타나지 않습니다.
4. **`CW_USEDEFAULT`로 위치와 크기를 정합니다.** 화면 가운데쯤에 큰 크기로 떠 있는 이유입니다.

## 2. 왜 지금 보이기 시작했는가

`WS_OVERLAPPEDWINDOW`에는 `WS_VISIBLE`이 없으므로 창은 처음에 보이지 않습니다. 누군가 이 창에 `ShowWindow`를 부른 것입니다.

우리 코드는 아닙니다. `src/taskbar_controller.cpp`의 `ShowTrayWindows`(198행)가 `Shell_TrayWnd` 창에 `ShowWindow(hwnd, SW_SHOWNA)`를 부르지만, 그 앞의 `FindExplorerShellTrayWnd`(54행)가 자기 프로세스의 창을 걸러 냅니다.

```cpp
HWND FindExplorerShellTrayWnd() {
  HWND hwnd = nullptr;
  while ((hwnd = FindWindowExW(nullptr, hwnd, kPrimaryClass, nullptr)) != nullptr) {
    if (!OwnProcessWindow(hwnd)) {
      return hwnd;
    }
  }
  return nullptr;
}
```

따라서 **바깥 프로세스가 부른 것입니다.** 그리고 그것이 가능한 이유는 우리가 의도적으로 만든 상황입니다. 트레이 선점은 `FindWindowW(L"Shell_TrayWnd", nullptr)`가 explorer의 창이 아니라 **우리 스파이를 돌려주게 만드는 것**이 목적입니다(`RESEARCH-TRAY-REREGISTER.md` 6-1절). 그래서 작업 표시줄을 찾아 `ShowWindow`를 부르는 코드는 어디에 있든 우리 창을 건드리게 됩니다.

이번 부팅 로그가 그 정황을 보여 줍니다.

```
08:25:56.937  [tray] intercept priority acquired ms=0
08:25:57.786  [host] shell wait ms=828 found=1 tray=1 progman=1 notify=1
08:25:57.802  [tray] intercept z-order restored
08:26:02.409  [tray] intercept z-order restored
08:26:05.348  [tray] intercept z-order restored
08:26:06.684  [tray] intercept priority acquired ms=0
08:26:08.350  [tray] intercept z-order restored
```

`FIX-AUTOSTART-DELAY.md`로 기동이 빨라진 뒤 우리가 explorer보다 먼저 뜨게 되었고, explorer가 셸을 초기화하는 동안 우리 스파이가 계속 우선순위를 잡고 있습니다. 그 구간에 작업 표시줄을 찾는 코드가 우리 창을 만난 것으로 보입니다.

**누가 불렀는지는 이번 작업에서 밝히지 않습니다.** 밝혀 봐야 그 프로그램을 우리가 고칠 수 없고, 아래 수정은 호출자가 누구든 통합니다. 다만 3-3절에서 그 사실을 로그로 남겨 두어, 앞으로 필요할 때 확인할 수 있게 합니다.

---

## 3. 수정 내용

목표는 **이 창이 어떤 경우에도 화면에 나타나지 않게 하는 것**입니다. 동시에 선점 기능은 그대로 살아 있어야 합니다.

### 3-1. 창을 보이지 않는 형태로 만든다

547행을 다음으로 바꿉니다.

```cpp
    // 선점 때문에 FindWindowW(L"Shell_TrayWnd", ...)가 이 창을 돌려주므로, 작업 표시줄을
    // 찾아 ShowWindow를 부르는 바깥 코드가 이 창을 화면에 띄울 수 있다. 제목 표시줄도
    // 테두리도 없는 0 크기 팝업으로 만들어 두면 표시되더라도 아무것도 그려지지 않는다.
    HWND spy = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
                               kSpyClass, kSpyClass, WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, wc.hInstance, this);
```

바뀐 곳은 넷입니다.

| 항목 | 이전 | 이후 | 이유 |
|---|---|---|---|
| 창 스타일 | `WS_OVERLAPPEDWINDOW` | `WS_POPUP` | 제목 표시줄과 테두리를 없앱니다 |
| 위치와 크기 | `CW_USEDEFAULT` 넷 | `0, 0, 0, 0` | 크기가 0이면 표시되어도 화면을 덮지 않습니다 |
| 확장 스타일 | — | `WS_EX_NOACTIVATE` 추가 | 실수로 포커스를 가져가지 않게 합니다 |
| 확장 스타일 | — | `WS_EX_TRANSPARENT` 추가 | 마우스 입력을 가로채지 않게 합니다 |

`WS_EX_TOOLWINDOW`와 `WS_EX_TOPMOST`는 그대로 두십시오. 앞의 것은 작업 전환 목록에서 빼는 역할이고, 뒤의 것은 선점에 쓰입니다.

**창 제목은 `kSpyClass` 그대로 두십시오.** 진짜 작업 표시줄과 구별하는 표식이라 진단에 도움이 됩니다. 위 수정을 마치면 어차피 화면에 나타나지 않습니다.

### 3-2. 표시 요청 자체를 막는다

크기가 0이어도, 바깥에서 크기를 바꾸면서 표시하면 다시 보일 수 있습니다. `WM_WINDOWPOSCHANGING`에서 표시 요청을 걷어 내십시오. 이것이 표시를 막는 문서화된 방법입니다.

스파이 창 프로시저(`msg >= WM_USER` 분기가 있는 그 함수)의 `DefWindowProcW` 호출 앞에 넣습니다.

```cpp
    if (msg == WM_WINDOWPOSCHANGING) {
      auto* pos = reinterpret_cast<WINDOWPOS*>(lp);
      if (pos != nullptr && (pos->flags & SWP_SHOWWINDOW) != 0) {
        pos->flags &= ~SWP_SHOWWINDOW;
        NoteShowAttempt();
      }
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
```

`ShowWindow(hwnd, SW_SHOW)`도 내부적으로 이 경로를 지나므로 함께 막힙니다.

**`SWP_HIDEWINDOW`는 건드리지 마십시오.** 숨기는 요청은 그대로 통과시켜야 합니다.

### 3-3. 시도를 로그로 남긴다

`NoteShowAttempt()`는 파일 지역 함수로 두고, **처음 세 번만** 기록하십시오. 반복 호출이 로그를 채우면 안 됩니다.

```
[tray] intercept spy show blocked count=1
```

이 줄이 실제로 찍히는지가 2절의 추정을 확인하는 근거가 됩니다. 창을 띄우려는 시도가 정말 있었는지, 아니면 다른 경로였는지를 이 줄로 가릴 수 있습니다.

### 3-4. 이미 만들어진 창에도 적용되는지 확인한다

`StartSpy`가 창을 다시 만드는 경로(explorer 재시작 처리 등)가 있다면 그 경로도 같은 인자를 쓰는지 확인하십시오. 창 생성이 한 곳뿐이면 그대로 두면 됩니다.

---

## 4. 선점이 깨지지 않는지 확인한다

이번 수정에서 가장 조심할 부분입니다. 창 스타일과 크기를 바꾸어도 다음이 유지되어야 합니다.

- `FindWindowW(L"Shell_TrayWnd", nullptr)`는 창의 표시 여부와 스타일에 관계없이 최상위 창을 찾습니다. 따라서 선점은 그대로 동작해야 합니다.
- `SetWindowPos(spy, HWND_TOPMOST, ...)`로 z-order를 잡는 `KeepPriority`(669행)와 `WaitForPriority`(686행)도 그대로 동작해야 합니다.
- `WM_COPYDATA` 수신은 창 스타일과 무관합니다. `ChangeWindowMessageFilterEx` 호출은 지금 자리를 유지하십시오.

**메시지 전용 창(`HWND_MESSAGE`)으로 바꾸지 마십시오.** 그렇게 하면 `FindWindowW`가 찾지 못해 선점이 완전히 깨집니다.

---

## 5. 검증

빌드한 뒤 `D:\repos\bamti\build\Release\bamti.exe`로 확인합니다. 이 경로가 자동 시작 작업이 가리키는 실행 파일입니다.

### 5-1. 재부팅 없이 확인할 것

1. 앱을 다시 띄우고 바탕 화면에 `Shell_TrayWnd` 제목의 창이 없어야 합니다.
2. 로그의 `intercept prestart elapsed_ms=`와 `intercept priority acquired ms=`가 이전처럼 찍혀야 합니다.
3. **트레이 아이콘이 상단바에 그대로 미러링되어야 합니다.** 선점이 깨지면 여기서 바로 드러납니다. 기동 3분 뒤에 남는 `intercept roster`와 `uia roster` 두 줄을 대조하십시오. 판정 방법은 `RESEARCH-TRAY-REREGISTER.md` 6-1절의 "측정 방법"에 있습니다.
4. 트레이 아이콘을 좌클릭과 우클릭해서 각 앱의 메뉴가 뜨는지 확인하십시오.

### 5-2. 재부팅해서 확인할 것

1. **바탕 화면에 `Shell_TrayWnd` 창이 없어야 합니다.** 이번 지시서의 목적입니다.
2. `[tray] intercept spy show blocked count=` 줄이 있는지 확인하고, **있든 없든 그 사실을 완료 보고에 적어 주십시오.** 있으면 2절의 추정이 맞은 것이고, 없으면 창이 보였던 경로가 다른 데 있다는 뜻이므로 추가 조사가 필요합니다.
3. 트레이 아이콘 미러링이 정상인지 5-1절 3번과 같은 방법으로 확인하십시오.

---

## 6. 주의 사항

- **선점 자체를 포기하는 방향으로 고치지 마십시오.** 클래스 이름을 바꾸거나 z-order를 양보하면 창은 안 보이지만 트레이 가로채기가 무력해집니다. 그 기능이 이 프로젝트의 5단계 성과입니다.
- 기동 순서를 되돌리는 방식도 쓰지 마십시오. 빨라진 기동은 원하는 결과입니다.
- `taskbar_controller.cpp`는 건드리지 마십시오. `FindExplorerShellTrayWnd`가 자기 프로세스 창을 이미 걸러 내고 있어서 그쪽에는 결함이 없습니다.
- 스파이 창의 사각형을 상단바 위치에 맞추는 방안은 이번 범위가 아닙니다. 작업 표시줄 위치를 `Shell_TrayWnd`의 사각형으로 알아내는 앱에는 도움이 될 수 있지만, `FIX-TRAY-MENU-POS.md`의 팝업 보정과 맞물리는 부분이 있어 따로 판단해야 합니다. 이번에는 0 크기로 두십시오.
