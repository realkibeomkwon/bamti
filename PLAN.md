# bamti 최적화·리팩터링 계획

대상 범위는 세 가지입니다. 첫째로 독 아이콘 우클릭 메뉴가 늦게 뜨고 닫히지 않는 결함을 근본 원인 수준에서 제거합니다. 둘째로 상주 프로세스의 CPU 사용량과 응답 지연을 줄입니다. 셋째로 프로젝트의 최종 목표인 "트레이를 상단바로 옮기고 사용량을 상시 표시하는 기능"을 얹을 수 있는 기반을 미리 마련합니다.

이 문서는 구현자가 그대로 따라갈 수 있도록 파일, 함수, 판정 기준을 명시합니다. 각 단계는 독립적으로 빌드되고 검증될 수 있도록 나누어 두었습니다.

---

## 0. 요약: 무엇이 문제인가

| 증상 | 근본 원인 | 위치 |
|---|---|---|
| 메뉴가 닫히지 않고 멈춘다 | 닫기 신호가 `WM_ACTIVATE`(WA_INACTIVE) 하나에만 의존하는데, 메뉴 창이 포그라운드 활성화에 실패하면 그 신호가 영원히 오지 않는다 | `dock.cpp` `HandleMenu`, `ActivateMenuWindow` |
| 메뉴가 뜨는 데 오래 걸린다 | `ActivateMenuWindow()`가 응답 없는 포그라운드 창에 최대 50ms 동안 블로킹 호출을 보내고, 그 뒤 `AttachThreadInput`으로 상대 스레드의 입력 큐에 자신을 묶는다 | `dock.cpp` `ActivateMenuWindow` |
| 상주 CPU 사용량이 높다 | 50ms 폴링 타이머가 초당 20회씩 전체화면 판정과 트레이 재숨김 검사를 수행한다 | `dock.cpp` `PollPointer` |
| 창 변화 때마다 버벅인다 | WinEvent 훅이 `EVENT_OBJECT_CREATE`부터 `EVENT_OBJECT_HIDE`까지 전 범위를 받고, 콜백마다 크로스 프로세스 호출을 한다 | `dock.cpp` `Create`, `WinEventProc` |
| 목록 갱신이 무겁다 | `CollectDockApps()`가 창 하나당 COM 프로퍼티 스토어를 네 번 연다 | `task_list.cpp:654` |

가장 중요한 판단부터 말씀드리면, **메뉴 창을 포그라운드로 활성화하려는 시도 자체를 버리는 것이 정답입니다.** 현재 코드가 여러 번 고쳐지면서 지저분해진 이유는, 활성화가 실패할 수 있다는 사실을 전제하지 않은 채 그 위에 가드 타이머와 보조 플래그를 계속 덧붙였기 때문입니다. 활성화를 포기하고 마우스 캡처 기반으로 전환하면 상태 변수 다섯 개가 하나로 줄고, 닫히지 않는 경우 자체가 구조적으로 사라집니다.

---

## 1. 근본 원인 상세 분석

### 1.1 메뉴가 닫히지 않는 이유

현재 메뉴가 닫히는 경로는 실질적으로 다음 네 가지뿐입니다.

1. 메뉴 창 위에서 마우스 버튼을 놓는 경우 (`HandleMenu`의 `WM_LBUTTONUP` / `WM_RBUTTONUP`)
2. 메뉴 창이 활성화를 잃는 경우 (`WM_ACTIVATE`에서 `WA_INACTIVE`)
3. 독 본체나 hot 창을 클릭하는 경우
4. 폴링 타이머가 "메뉴 창이 보여야 하는데 보이지 않음"을 감지하는 경우

문제는 2번입니다. 독 본체(`hwnd_`)는 `WS_EX_NOACTIVATE` 스타일로 만들어져 있고, bamti 프로세스는 사용자 입력을 받은 포그라운드 프로세스가 아닌 경우가 많습니다. Windows는 포그라운드 설정 권한이 없는 프로세스의 `SetForegroundWindow` 호출을 조용히 무시하고 `FALSE`만 돌려줍니다. 이때 메뉴 창은 화면에 보이지만 활성화된 적이 없으므로, 사용자가 바깥을 클릭해도 `WA_INACTIVE`가 발생하지 않습니다. 즉 **닫히지 않는 것이 아니라, 닫으라는 신호를 받을 수단이 아예 없는 상태**입니다.

여기에 `menu_guard_until_ = GetTickCount64() + 80`이라는 가드가 겹칩니다. 활성화가 느리게 성공한 경우, 유일하게 도착한 `WA_INACTIVE`가 이 80ms 창 안에 들어와 버려지는 일도 발생할 수 있습니다.

4번 안전망은 `menu_expect_visible_ && !IsWindowVisible(menu_hwnd_)` 조건이라서, 창이 보이는 채로 멈춘 상황에는 작동하지 않습니다. 즉 지금 코드에는 이 결함을 잡아낼 안전망이 없습니다.

### 1.2 메뉴가 느린 이유

`ShowContextMenu`가 반환되기까지 다음 작업이 순차로 일어납니다.

1. 창 목록의 각 항목마다 `WindowTitle(hwnd)`를 호출합니다. `InternalGetWindowText`가 실패하면 `SendMessageTimeoutW(WM_GETTEXTLENGTH, ..., 10ms)`로 넘어가므로, 응답이 느린 창이 여러 개면 지연이 누적됩니다.
2. 텍스트 폭을 재기 위해 `GetDC` / `CreateMenuFont` / `GetTextExtentPoint32W`를 수행합니다. 폰트는 매번 새로 만들고 곧바로 지웁니다.
3. `ActivateMenuWindow()`에서 `SendMessageTimeoutW(fg, WM_NULL, SMTO_BLOCK, 50ms)`를 호출합니다. 포그라운드 창이 바쁘면 여기서 50ms를 통째로 소모합니다.
4. `AttachThreadInput(self_tid, fg_tid, TRUE)`로 상대 스레드의 입력 큐에 자신을 묶습니다. 상대 스레드가 응답하지 않는 동안에는 우리 UI 스레드의 입력 처리까지 함께 정지합니다. 이것이 "멈춰버린다"는 증상의 유력한 원인입니다.

### 1.3 상주 비용이 큰 이유

- `SetTimer(hwnd_, kPollTimerId, 50, nullptr)`가 초당 20회 실행됩니다. 매번 `RefreshFullscreen()`이 `SHQueryUserNotificationState`, `GetForegroundWindow`, `DwmGetWindowAttribute`, `GetWindowLongW`, `GetMonitorInfoW`, `GetDpiForWindow`, `GetSystemMetricsForDpi`를 두 개의 창(`hwnd_`, `hot_hwnd_`)에 대해 각각 수행합니다. 여기에 `TaskbarController::Rehide()`가 `FindWindowW`와 `FindWindowExW` 열거를 더합니다.
- WinEvent 훅이 `{EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE}` 범위를 시스템 전역으로 받습니다. 캐럿, 툴팁, 메뉴, 애니메이션 등 온갖 객체가 이 범위에 포함되므로 콜백이 초당 수십에서 수백 회 발생합니다. 그런데 콜백 첫머리에서 `GetClassNameW(hwnd, ...)`를 **`object != OBJID_WINDOW` 검사보다 먼저** 호출합니다. 이 호출은 크로스 프로세스 마샬링을 유발하므로 낭비가 큽니다.
- `CollectDockApps()`는 창마다 `WindowAumid`와 `WindowPropString` 세 번을 호출하고, 이 네 함수가 각각 독립적으로 `SHGetPropertyStoreForWindow`를 호출합니다. 창이 30개라면 COM 프로퍼티 스토어를 120번 엽니다. 여기에 `CoCreateInstance(CLSID_VirtualDesktopManager)`도 호출마다 새로 수행합니다.

---

## 2. 단계별 구현 계획

각 단계는 순서대로 진행하며, 단계마다 빌드가 통과하고 수동 검증이 가능해야 합니다.

### 1단계: 독 컨텍스트 메뉴를 팝업 표면(PopupSurface)으로 재작성

가장 우선순위가 높은 작업입니다. 기존 메뉴 코드를 부분 수정하지 말고, 새 파일로 처음부터 작성한 뒤 `dock.cpp`에서 갈아끼우기를 권장합니다.

#### 1-1. 새 파일 구성

```
src/popup_surface.hpp
src/popup_surface.cpp
```

`PopupSurface`는 독 컨텍스트 메뉴뿐 아니라 이후 상단바 상태 항목 팝업 패널까지 함께 쓰는 공용 컴포넌트로 설계합니다. 이 공용화가 이번 리팩터링에서 가장 큰 이득입니다. codexbar 스타일 패널도 결국 같은 문제(활성화, 닫기, 포커스, 화면 경계 보정)를 해결해야 하기 때문입니다.

```cpp
namespace bamti {

// 팝업 안에 그릴 내용을 제공하는 쪽이 구현한다.
class PopupContent {
 public:
  virtual ~PopupContent() = default;
  // 주어진 DPI에서 필요한 크기를 픽셀 단위로 돌려준다.
  virtual SIZE Measure(UINT dpi) = 0;
  // Direct2D 렌더 타깃에 그린다. 좌표는 클라이언트 기준이다.
  virtual void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) = 0;
  // 클라이언트 좌표를 항목 인덱스로 바꾼다. 해당 항목이 없으면 -1을 돌려준다.
  virtual int HitTest(POINT client, UINT dpi) const = 0;
  // 항목이 선택되었을 때 호출된다. 팝업은 이미 닫힌 뒤에 호출한다.
  virtual void Invoke(int index) = 0;
};

class PopupSurface {
 public:
  enum class Anchor { AboveAt, BelowAt };  // 독은 AboveAt, 상단바는 BelowAt을 쓴다.

  bool Create(HINSTANCE instance, HWND owner);
  void Destroy();

  // 이미 열려 있으면 먼저 닫은 뒤 연다.
  bool Open(PopupContent* content, POINT anchor_screen, Anchor mode);
  void Close();
  bool IsOpen() const;

  // 소유자가 테마 변경을 알릴 때 호출한다.
  void SetDark(bool dark);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  void Dismiss(int invoke_index);   // 닫기의 단일 진입점
  void EnsureRenderTarget();
  void Render();

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  PopupContent* content_ = nullptr;   // 소유하지 않는다. 열려 있는 동안만 유효하다.
  bool open_ = false;
  int hot_ = -1;
  bool dark_ = true;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
};

}  // namespace bamti
```

#### 1-2. 핵심 설계 규칙

이 규칙들은 협상 대상이 아닙니다. 기존 결함이 전부 이 규칙을 어긴 데서 나왔습니다.

**규칙 1. 팝업 창을 절대 활성화하지 않는다.**

- 창 스타일에 `WS_EX_NOACTIVATE`를 넣습니다.
- `WM_MOUSEACTIVATE`에서 `MA_NOACTIVATE`를 돌려줍니다.
- `SetForegroundWindow`, `SetFocus`, `BringWindowToTop`, `AttachThreadInput`을 **한 번도 호출하지 않습니다.** 기존 `ActivateMenuWindow()`는 통째로 삭제합니다.
- 이로써 1.2절의 50ms 블로킹 호출과 입력 큐 결합이 모두 사라집니다.

**규칙 2. 바깥 클릭은 마우스 캡처로 감지한다.**

```cpp
// Open() 안에서, 창을 보인 직후
SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE | SWP_SHOWWINDOW);
SetCapture(hwnd_);
```

캡처 상태에서는 화면 어디를 눌러도 마우스 메시지가 `hwnd_`로 전달됩니다. 따라서 `WM_LBUTTONDOWN` / `WM_RBUTTONDOWN`을 받았을 때 좌표가 클라이언트 영역 밖이면 즉시 닫으면 됩니다.

```cpp
case WM_LBUTTONDOWN:
case WM_RBUTTONDOWN: {
  POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  RECT client{};
  GetClientRect(hwnd_, &client);
  if (!PtInRect(&client, pt)) {
    Dismiss(-1);   // 바깥을 눌렀으므로 아무 명령 없이 닫는다.
  }
  return 0;
}
case WM_LBUTTONUP: {
  POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  RECT client{};
  GetClientRect(hwnd_, &client);
  Dismiss(PtInRect(&client, pt) ? content_->HitTest(pt, Dpi()) : -1);
  return 0;
}
```

**규칙 3. 닫기 경로를 하나로 통일하고, 안전망을 세 겹 둔다.**

`Dismiss(int invoke_index)`만이 닫기를 수행합니다. 이 함수는 몇 번 호출되어도 안전해야 합니다.

```cpp
void PopupSurface::Dismiss(int invoke_index) {
  if (!open_) {
    return;
  }
  open_ = false;
  hot_ = -1;
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  KillTimer(hwnd_, kPopupGuardTimer);
  ShowWindow(hwnd_, SW_HIDE);

  PopupContent* content = content_;
  content_ = nullptr;
  if (content != nullptr && invoke_index >= 0) {
    // 명령 실행 중 재진입이 일어나도 팝업 상태는 이미 정리되어 있다.
    content->Invoke(invoke_index);
  }
}
```

안전망은 다음 세 가지입니다.

- **안전망 A:** `WM_CAPTURECHANGED`를 받으면 무조건 `Dismiss(-1)`을 호출합니다. 다른 창이 캡처를 가져갔다는 뜻이므로 팝업은 더 이상 입력을 받을 수 없습니다. 이것이 가장 중요한 방어선입니다.
- **안전망 B:** 팝업이 열려 있는 동안 200ms 주기 타이머를 돌리며 `GetCapture() == hwnd_`를 확인합니다. 조건이 깨져 있으면 `Dismiss(-1)`을 호출합니다. `WM_CAPTURECHANGED`가 유실되는 드문 경우를 잡습니다.
- **안전망 C:** 같은 타이머에서 `GetForegroundWindow()`가 마지막 검사 시점과 달라졌고 그 창이 자기 프로세스 소유가 아니면 닫습니다. Alt+Tab이나 Win 키로 다른 앱이 올라온 상황을 처리합니다.

세 안전망 덕분에 **팝업이 화면에 남은 채 영원히 멈추는 상태가 구조적으로 불가능해집니다.**

**규칙 4. `ESC` 키를 처리한다.**

캡처 상태에서는 키보드 메시지가 오지 않습니다. 안전망 B의 200ms 타이머 안에서 `GetAsyncKeyState(VK_ESCAPE) & 0x8000`을 확인해 닫습니다. 저수준 키보드 훅(`WH_KEYBOARD_LL`)은 시스템 전역 입력 지연을 유발할 수 있으므로 쓰지 않습니다.

**규칙 5. 상태 변수를 하나로 줄인다.**

`menu_open_`, `menu_dismissing_`, `menu_expect_visible_`, `menu_guard_until_`, `context_index_`를 전부 없애고 `bool open_` 하나만 남깁니다. `pending_menu_cmd_`, `pending_menu_app_`, `pending_menu_windows_`도 `PopupContent` 구현체가 자기 데이터를 들고 있으므로 필요하지 않습니다.

#### 1-3. 독 쪽 연결

`dock.cpp`에 `DockMenuContent : public PopupContent`를 만듭니다. 이 클래스가 행 목록, 측정, 렌더, 명령 실행을 담당합니다.

```cpp
class DockMenuContent : public PopupContent {
 public:
  void Reset(Dock* owner, const DockApp& app);
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;   // Dock의 명령 처리기를 호출한다.
 private:
  Dock* owner_ = nullptr;
  DockApp app_{};
  std::vector<DockMenuRow> rows_;
  std::vector<HWND> window_targets_;
};
```

`Invoke`는 `PostMessageW(dock_hwnd, kMenuCommandMsg, ...)` 방식을 유지합니다. 명령을 메시지 루프의 다음 회차로 미루는 기존 판단은 옳으므로 그대로 둡니다. 창을 닫거나 활성화하는 작업이 팝업의 메시지 처리 도중에 실행되면 재진입 위험이 있기 때문입니다.

`Dock`에서 삭제할 함수와 멤버는 다음과 같습니다.

- 함수: `ShowContextMenu`(내용은 `DockMenuContent::Reset`으로 이동), `EnsureMenuWindow`, `ApplyMenuChrome`, `ActivateMenuWindow`, `DismissMenuWindow`, `DestroyMenuWindow`, `CloseDockMenu`, `HandleMenu`, `MenuProc`, `PaintMenu`, `MenuHitTest`, `MenuRowRect`, `ArmMenuMouseLeave`
- 멤버: `menu_hwnd_`, `menu_open_`, `menu_dismissing_`, `menu_expect_visible_`, `menu_hot_`, `menu_guard_until_`, `pending_menu_cmd_`, `pending_menu_app_`, `pending_menu_windows_`, `menu_window_cmds_`, `menu_rows_`, `context_index_`
- 클래스 등록: `kDockMenuClass` 등록 코드는 `PopupSurface`로 이동합니다.

`Dock::Busy()`는 `popup_.IsOpen() || dragging_ || pressed_ >= 0`으로 바꿉니다.

#### 1-4. 렌더링을 Direct2D로 통일한다

기존 `PaintMenu`는 GDI를 씁니다. 매 페인트마다 `CreateCompatibleDC`, `CreateCompatibleBitmap`, `CreateFontIndirectW`, `CreateSolidBrush`, `CreatePen`을 새로 만들고 지웁니다. 이것을 Direct2D `ID2D1HwndRenderTarget`과 DirectWrite로 옮깁니다.

- 렌더 타깃, 브러시, `IDWriteTextFormat`을 `PopupSurface`가 한 번 만들어 보관하고, DPI나 테마가 바뀔 때만 다시 만듭니다.
- 행 텍스트 폭 측정은 `IDWriteTextLayout::GetMetrics()`로 수행합니다. `GetDC` / `GetTextExtentPoint32W` 경로가 사라집니다.
- 진행 바, 둥근 모서리, 반투명 배경을 나중에 그대로 쓸 수 있게 됩니다. 이것이 상단바 패널 작업의 전제 조건입니다.

`EndDraw()`가 `D2DERR_RECREATE_TARGET`을 돌려주면 타깃을 버리고 다음 프레임에 다시 만듭니다.

#### 1-5. 이 단계의 검증

1. 실행 중인 앱이 10개 이상인 상태에서 독 아이콘을 우클릭하고, 로그의 `menu ... %ums` 값이 **16ms 이하**인지 확인합니다.
2. 메뉴가 뜬 상태에서 다음 동작마다 메뉴가 즉시 닫히는지 확인합니다. 바탕화면 클릭, 다른 앱 창 클릭, 상단바 클릭, 독의 다른 아이콘 클릭, `ESC` 키, `Alt+Tab`, `Win` 키, 다른 아이콘 우클릭.
3. 응답하지 않는 창을 만들어 포그라운드에 둔 상태(예: 무한 루프 중인 앱)에서 우클릭했을 때, 메뉴가 지연 없이 뜨고 정상적으로 닫히는지 확인합니다. 기존 코드는 이 경우에 멈춥니다.
4. 메뉴를 100회 연속으로 열고 닫은 뒤 작업 관리자에서 GDI 객체 수와 USER 객체 수가 증가하지 않는지 확인합니다.

---

### 2단계: WinEvent 훅 범위를 줄인다

`dock.cpp`의 `Create()`와 `WinEventProc()`를 수정합니다.

#### 2-1. 콜백에서 검사 순서를 바꾼다

현재 콜백은 `GetClassNameW`를 먼저 호출합니다. 다음처럼 가장 싼 검사를 앞에 둡니다.

```cpp
void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
  if (object != OBJID_WINDOW || child != CHILDID_SELF || hwnd == nullptr) {
    return;
  }
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
    return;
  }
  if (g_notify == nullptr) {
    return;
  }
  if (!g_rebuild_posted.exchange(true)) {
    PostMessageW(g_notify, kTasksChangedMsg, 0, 0);
  }
}
```

트레이 재숨김 판정(`GetClassNameW` + `TaskbarController::Rehide`)은 이 콜백에서 완전히 분리합니다. 아래 2-3에서 다룹니다.

#### 2-2. 필요한 이벤트만 개별 훅으로 등록한다

```cpp
static const DWORD kEvents[] = {
    EVENT_OBJECT_CREATE,
    EVENT_OBJECT_DESTROY,
    EVENT_OBJECT_SHOW,
    EVENT_OBJECT_HIDE,
    EVENT_OBJECT_CLOAKED,
    EVENT_OBJECT_UNCLOAKED,
};
const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS | WINEVENT_SKIPOWNTHREAD;
for (DWORD e : kEvents) {
  if (HWINEVENTHOOK h = SetWinEventHook(e, e, nullptr, WinEventProc, 0, 0, flags)) {
    hooks_.push_back(h);
  }
}
```

기존 `{EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE}` 범위는 결과적으로 같은 네 이벤트를 담지만, 범위 형태로 등록하면 향후 이 범위에 값이 추가될 때 함께 딸려옵니다. 명시적 목록이 더 안전합니다. `WINEVENT_SKIPOWNTHREAD`를 추가한 점이 실질적인 개선입니다.

#### 2-3. 트레이 감시를 트레이 스레드 한정 훅으로 옮긴다

`Shell_TrayWnd`가 다시 보이는지를 50ms마다 폴링할 필요가 없습니다. 트레이 창의 스레드에만 붙는 훅 하나면 충분합니다.

```cpp
// TaskbarController에 추가한다.
bool TaskbarController::WatchTray(WINEVENTPROC proc) {
  HWND tray = PrimaryTray();
  if (tray == nullptr) {
    return false;
  }
  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(tray, &pid);
  tray_hook_ = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_LOCATIONCHANGE, nullptr, proc,
                               pid, tid, WINEVENT_OUTOFCONTEXT);
  return tray_hook_ != nullptr;
}
```

explorer가 재시작되면 이 훅은 무효가 되므로, `EVENT_OBJECT_DESTROY`에서 `Shell_TrayWnd`가 사라진 것을 감지했을 때 훅을 다시 겁니다. 안전망으로 5초 주기의 저빈도 타이머에서 `Rehide()`를 한 번 호출합니다.

---

### 3단계: 50ms 폴링 타이머를 제거한다

`dock.cpp`의 `kPollTimerId` 타이머는 초당 20회 실행되며 상주 CPU 비용의 대부분을 차지합니다. 폴링이 담당하던 세 가지 일을 각각 이벤트로 옮깁니다.

| 폴링이 하던 일 | 대체 수단 |
|---|---|
| `RefreshFullscreen()` | `EVENT_SYSTEM_FOREGROUND` 훅 + `IAppVisibility::Advise` + AppBar의 `ABN_FULLSCREENAPP` |
| `TaskbarController::Rehide()` | 2-3의 트레이 한정 훅 + 5초 주기 저빈도 안전망 |
| `PointerOverUi()` 검사 | hot 창과 독 본체의 `WM_MOUSEMOVE` + `TrackMouseEvent(TME_LEAVE)` |

`ARCHITECTURE.md`가 이미 `IAppVisibility`와 `ABN_FULLSCREENAPP`을 전체화면 감지 수단으로 지정하고 있으므로, 이 변경은 설계 문서와도 일치합니다.

포인터 추적에는 이미 `ArmMouseLeave()`와 `WM_MOUSELEAVE` 처리가 있습니다. 다만 마우스가 hot 창과 독 본체 사이를 오갈 때 경계가 어긋날 수 있으므로, 다음과 같이 정리합니다.

- hot 창에서 `WM_MOUSEMOVE`를 받으면 `ShowPill()`을 호출하고, hot 창에도 `TrackMouseEvent`를 겁니다.
- 독 본체의 `WM_MOUSELEAVE`에서 `StartHideTimer()`를 겁니다. 숨김 지연(`kHideDelayMs = 100`)은 그대로 둡니다.
- 숨김 타이머가 만료되었을 때만 `GetCursorPos` 한 번으로 최종 확인을 합니다.

폴링을 완전히 없애기 어려운 부분이 남는다면, 주기를 50ms에서 **500ms**로 낮추고 독이 보이거나 팝업이 열려 있는 동안에만 타이머를 돌립니다. 독이 숨겨져 있고 팝업도 닫혀 있으면 타이머를 아예 끕니다.

`menu_bar.cpp`의 `kFullscreenTimerId`도 같은 방식으로 정리합니다. 시계 타이머(`kClockTimerId`, 1000ms)는 유지하되, 다음 두 가지를 개선합니다.

- 전체 창을 무효화하지 말고 시계 영역 사각형만 `InvalidateRect`에 넘깁니다.
- 표시 문자열이 이전과 같으면 무효화 자체를 건너뜁니다. 초를 표시하지 않는 설정이라면 실제 갱신은 분당 한 번이면 충분합니다.

---

### 4단계: `CollectDockApps()`를 캐시 기반으로 다시 짠다

`task_list.cpp:654`가 대상입니다.

#### 4-1. 프로퍼티 스토어를 창당 한 번만 연다

현재 `WindowAumid`와 `WindowPropString`(세 번 호출)이 각각 `SHGetPropertyStoreForWindow`를 부릅니다. 이를 한 번 열어 네 키를 모두 읽는 함수로 합칩니다.

```cpp
struct WindowProps {
  std::wstring aumid;
  std::wstring icon_resource;
  std::wstring relaunch_name;
  std::wstring relaunch_command;
};

WindowProps ReadWindowProps(HWND hwnd) {
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) {
    return {};
  }
  WindowProps out;
  out.aumid = ReadString(store.Get(), PKEY_AppUserModel_ID);
  out.icon_resource = ReadString(store.Get(), PKEY_AppUserModel_RelaunchIconResource);
  out.relaunch_name = ReadString(store.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource);
  out.relaunch_command = ReadString(store.Get(), PKEY_AppUserModel_RelaunchCommand);
  return out;
}
```

창 30개 기준으로 COM 호출이 120회에서 30회로 줄어듭니다.

#### 4-2. HWND 단위 캐시를 둔다

실행 파일 경로, AUMID, 아이콘 리소스, relaunch 명령은 창이 살아 있는 동안 바뀌지 않습니다. 제목만 자주 바뀝니다.

```cpp
struct WindowCacheEntry {
  std::wstring path;
  WindowProps props;
  bool is_task_window = false;
};
// 파일 스코프
std::unordered_map<HWND, WindowCacheEntry> g_window_cache;
```

- `CollectDockApps()`는 캐시에 없는 HWND에 대해서만 비싼 조회를 수행합니다.
- `EVENT_OBJECT_DESTROY` 처리 시 해당 HWND를 캐시에서 지웁니다. 지우기를 놓쳐도 안전하도록, 열거 도중 `IsWindow(hwnd)`가 거짓인 항목을 함께 제거합니다.
- 캐시 크기 상한을 512로 두고, 넘으면 통째로 비웁니다. 셸 도구에서 정교한 LRU는 과합니다.

#### 4-3. `IVirtualDesktopManager`를 한 번만 만든다

`CoCreateInstance`를 호출마다 하지 말고 함수 지역 `static` 또는 파일 스코프에 보관합니다. COM 아파트먼트가 UI 스레드 하나뿐이므로 안전합니다.

#### 4-4. 결과가 같으면 아무것도 하지 않는다

`Rebuild()`에서 새로 만든 `items_`가 이전과 동일하면 `EnsureIcons()`, `Layout()`, `RenderLayered()`를 모두 건너뜁니다. 비교는 `(key, hwnd, running, pinned, windows.size())` 튜플 목록으로 충분합니다.

#### 4-5. 디바운스를 늘린다

`kRebuildDelayMs`를 50에서 **150**으로 올립니다. 창을 여러 개 여닫을 때 갱신이 한 번으로 뭉쳐집니다. 사람이 느끼기에 150ms는 즉시입니다.

#### 4-6. 이 단계의 검증

`Rebuild()` 시작과 끝에 `GetTickCount64()`를 찍어 로그로 남기고, 창 30개 환경에서 **8ms 이하**인지 확인합니다. 캐시가 따뜻해진 뒤에는 2ms 이하가 나와야 합니다.

---

### 5단계: 아이콘과 GDI 자원 수명을 정리한다

- `CreateMenuFont`처럼 매 호출마다 자원을 만드는 함수를 없앱니다. 4단계 이후 남는 GDI 폰트 생성 지점은 DPI별로 한 번만 만들어 보관합니다.
- `icon_cache_`가 `std::map<std::wstring, HBITMAP>`인데, `ResetIconCache()`가 언제 불리는지와 DPI 변경 시 어떤 키로 무효화되는지를 점검합니다. 키에 픽셀 크기를 포함시켜 `<path>|<px>` 형태로 만들면 DPI 전환 때 전체를 버릴 필요가 없습니다.
- 아이콘 추출은 디스크와 리소스를 읽으므로 잠재적으로 느립니다. 현재 UI 스레드에서 동기로 수행합니다. 첫 표시 때 자리표시자를 그리고, 워커 스레드에서 추출한 뒤 `PostMessage`로 교체하는 방식을 검토합니다. 다만 이는 복잡도를 올리므로, 4단계 이후에도 실제로 느릴 때만 진행합니다. **측정 없이 착수하지 않습니다.**

---

### 6단계: 빌드 설정을 릴리스에 맞게 조인다

`CMakeLists.txt`에 릴리스 최적화 설정이 명시되어 있지 않습니다. 다음을 추가합니다.

```cmake
target_compile_options(bamti PRIVATE
  /utf-8 /W4 /permissive- /EHsc /sdl
  $<$<CONFIG:Release>:/O2>
  $<$<CONFIG:Release>:/Oi>
  $<$<CONFIG:Release>:/Gy>
  $<$<CONFIG:Release>:/Gw>
  $<$<CONFIG:Release>:/GL>
)

target_link_options(bamti PRIVATE
  $<$<CONFIG:Release>:/LTCG>
  $<$<CONFIG:Release>:/OPT:REF>
  $<$<CONFIG:Release>:/OPT:ICF>
)
```

`/GS-`처럼 보안 검사를 끄는 옵션은 넣지 않습니다. 상주 셸 도구에서 얻는 이득보다 위험이 큽니다.

바이너리 크기와 시작 시간을 줄이려면 지연 로드도 검토합니다. `powrprof.dll`이나 `windowscodecs.dll`처럼 시작 직후에 필요하지 않은 라이브러리에 `/DELAYLOAD`를 적용합니다. `delayimp.lib` 링크가 필요합니다.

---

### 7단계: 상단바 상태 항목 기반 마련 (다음 작업의 전제)

이 단계는 트레이를 상단바로 옮기는 본 작업에 앞서, 프로토콜과 렌더링 기반만 미리 확장하는 준비 작업입니다.

#### 7-1. Status Item 스키마를 확장한다

현재 `status_item.hpp`의 `StatusItem`은 `id`, `text`, `tooltip`, `priority`만 갖고 `kStatusTextMaxChars = 32` 제한이 있습니다. 첨부 이미지 수준(아이콘 + 퍼센트 + 팝업 패널 안의 여러 진행 게이지)을 표현하려면 다음이 필요합니다.

```cpp
struct StatusGauge {
  std::wstring label;     // "Session (5h) — Codex"
  float value = 0.0f;     // 0.0 ~ 1.0
  std::wstring detail;    // "Resets 18:31 · in 3h 38m"
  std::wstring note;      // "Ahead of pace"
};

struct StatusPanel {
  std::wstring title;         // "Codex"
  std::wstring subtitle;      // "Prolite"
  std::wstring updated_text;  // "Updated 13s ago"
  std::vector<StatusGauge> gauges;
  std::vector<std::wstring> actions;  // "Settings...", "Quit"
};

struct StatusItem {
  std::string id;
  std::wstring text;        // 바에 표시할 짧은 문자열. 예: "10%"
  std::wstring icon_glyph;  // 선택. 유니코드 글리프 또는 등록된 아이콘 키
  std::wstring tooltip;
  uint32_t accent = 0;      // 0이면 테마 기본색. 사용량이 높을 때 강조에 쓴다.
  int priority = 0;
  std::optional<StatusPanel> panel;  // 클릭 시 열 팝업 패널
};
```

`pipe_server.cpp`의 JSON 라인 파서(`json_line.hpp`)를 확장해 위 필드를 받습니다. 기존 클라이언트가 보내는 최소 형태(`id`와 `text`)는 그대로 동작해야 하므로, 새 필드는 전부 선택 항목으로 둡니다.

`kStatusTextMaxChars` 제한은 바에 표시하는 `text`에만 적용하고, 패널 내부 문자열에는 적용하지 않습니다. 패널 쪽에는 게이지 개수 상한(예: 16개)과 문자열 길이 상한(예: 128자)을 따로 둡니다.

#### 7-2. 상태 항목을 클릭하면 팝업 패널을 연다

1단계에서 만든 `PopupSurface`를 그대로 씁니다. `StatusPanelContent : public PopupContent`를 새로 만들어 제목, 부제, 진행 바, 구분선, 액션 버튼을 Direct2D로 그립니다. `Anchor::BelowAt` 모드를 사용해 상단바 아래로 펼칩니다.

독 메뉴와 상태 패널이 같은 팝업 인프라를 쓰므로, 1단계에서 해결한 "닫히지 않는 문제"가 상단바 패널에서 재발하지 않습니다. **이것이 1단계를 먼저 하는 이유입니다.**

#### 7-3. 예제 클라이언트를 함께 만든다

`examples/status_push.py` 정도로 짧은 스크립트를 두어, 파이프에 JSON 라인을 밀어 넣으면 상단바에 표시되는 것을 바로 확인할 수 있게 합니다. 이 예제가 사실상 프로토콜 명세 역할을 합니다. LLM 사용량 표시 도구는 이 예제를 복사해 시작하면 됩니다.

#### 7-4. 기존 트레이 아이콘 호스팅에 관한 판단

`ARCHITECTURE.md`가 이미 `NOTIFYICON` 호스팅을 1.0 비범위로 두고 있습니다. 이 판단은 유지하는 것이 옳습니다. 트레이 아이콘을 흡수하려면 `Shell_TrayWnd` 아래의 `TrayNotifyWnd`를 리페어런팅하거나 미공개 COM 인터페이스를 다뤄야 하는데, 이는 누적 업데이트마다 깨집니다. 대신 Status Item 프로토콜을 1등 경로로 만들고, 자주 쓰는 앱을 위한 어댑터를 필요할 때 개별로 작성하는 편이 안정적입니다.

---

## 3. 작업 순서와 커밋 단위

| 순서 | 작업 | 커밋 메시지 예시 |
|---|---|---|
| 1 | `PopupSurface` 신규 작성, 독 메뉴 이관, 기존 메뉴 코드 삭제 | `refactor: 독 컨텍스트 메뉴를 캡처 기반 팝업으로 재작성한다` |
| 2 | WinEvent 훅 범위 축소, 트레이 감시 분리 | `perf: WinEvent 훅 범위를 줄이고 트레이 감시를 분리한다` |
| 3 | 50ms 폴링 제거, 이벤트 기반 전환 | `perf: 독 폴링 타이머를 이벤트 구독으로 대체한다` |
| 4 | `CollectDockApps` 캐시화 | `perf: 창 정보 조회를 캐시하고 프로퍼티 스토어를 한 번만 연다` |
| 5 | GDI 자원 수명 정리 | `refactor: 폰트와 아이콘 캐시 수명을 정리한다` |
| 6 | 릴리스 빌드 옵션 | `build: 릴리스 최적화와 LTCG를 켠다` |
| 7 | Status Item 스키마 확장, 패널 렌더 | `feat: 상태 항목에 아이콘과 팝업 패널을 추가한다` |

1단계와 2단계는 서로 독립적이므로 순서를 바꿔도 됩니다. 다만 7단계는 반드시 1단계 이후에 진행합니다.

---

## 4. 성능 목표

리팩터링 완료 시점에 다음 수치를 만족해야 합니다. 각 항목은 측정 가능한 형태로 적었습니다.

| 항목 | 현재 (추정) | 목표 | 측정 방법 |
|---|---|---|---|
| 유휴 상태 CPU 점유율 | 0.5 ~ 2% | **0.1% 미만** | 작업 관리자에서 5분 평균 |
| 우클릭 메뉴 표시 지연 | 수십에서 수백 ms, 최악에는 멈춤 | **16ms 이하** | 로그의 `menu ... %ums` |
| 메뉴가 닫히지 않는 사례 | 재현됨 | **0건** | 1-5절 검증 시나리오 |
| `Rebuild()` 소요 시간 | 미측정 | **8ms 이하** (캐시 후 2ms) | `Rebuild()` 앞뒤 타임스탬프 |
| 상주 작업 집합(working set) | 미측정 | **30MB 이하** | 작업 관리자 |
| 유휴 상태 컨텍스트 스위치 | 초당 20회 이상 | 초당 2회 이하 | 성능 모니터 |

측정값을 로그에 남기도록 `Log(L"perf", ...)` 항목을 추가하고, 릴리스 빌드에서도 켜 둡니다. 로그 회전이 이미 구현되어 있으므로(`log.cpp`의 2MB 제한) 부담이 없습니다.

---

## 5. 하지 말아야 할 것

리팩터링 중에 다음 유혹을 피합니다. 각 항목은 실제로 문제를 만들 수 있는 것들입니다.

1. **`TrackPopupMenuEx`로 되돌아가지 않습니다.** OS가 닫기를 보장한다는 장점은 있지만, 자체 모달 루프가 도는 동안 독의 다른 메시지가 지연됩니다. 다크 테마와 둥근 모서리를 맞추려면 결국 오너 드로우를 써야 하므로 코드량 이득도 크지 않습니다. 무엇보다 상단바 패널에는 쓸 수 없어 인프라를 두 벌 유지하게 됩니다.
2. **`AttachThreadInput`을 다시 도입하지 않습니다.** 다른 프로세스의 입력 큐에 자신을 묶는 순간, 상대의 응답 지연이 곧바로 자기 UI의 정지가 됩니다.
3. **저수준 훅(`WH_MOUSE_LL`, `WH_KEYBOARD_LL`)을 상시 설치하지 않습니다.** 시스템 전역 입력 지연을 유발합니다. 팝업이 열려 있는 짧은 동안에도 캡처로 충분하므로 필요하지 않습니다.
4. **UI 스레드에서 동기 파일 입출력이나 레지스트리 조회를 하지 않습니다.** 핀 목록 저장(`SaveDockPins`)은 명령 실행 시점에만 일어나므로 현재는 괜찮지만, 이를 자주 호출되는 경로로 옮기지 않도록 주의합니다.
5. **불필요한 추상화를 만들지 않습니다.** `PopupSurface`는 실제로 두 곳(독 메뉴, 상태 패널)에서 쓰이므로 정당합니다. 사용처가 하나뿐인 인터페이스는 만들지 않습니다.
6. **`explorer.exe`에 인젝션하거나 미공개 트레이 COM을 건드리지 않습니다.** `ARCHITECTURE.md`의 설계 원칙이며, 이번 리팩터링으로 바뀌지 않습니다.

---

## 6. 검증 체크리스트

각 단계를 마칠 때마다 아래를 확인합니다.

**빌드**
- [ ] `/W4` 경고 없이 컴파일된다.
- [ ] Debug와 Release 모두 빌드된다.

**독 메뉴**
- [ ] 실행 중인 앱 아이콘을 우클릭하면 창 목록, 구분선, 고정, 모두 보기, 가리기, 종료가 올바르게 나온다.
- [ ] 고정만 되어 있고 실행 중이 아닌 앱을 우클릭하면 고정 해제만 나온다.
- [ ] 바탕화면 클릭, 다른 앱 클릭, `ESC`, `Alt+Tab`, `Win` 키, 다른 아이콘 우클릭 각각에서 메뉴가 닫힌다.
- [ ] 응답 없는 창이 포그라운드일 때도 메뉴가 지연 없이 뜨고 닫힌다.
- [ ] 메뉴에서 명령을 고르면 실제로 실행되고, 메뉴가 먼저 닫힌다.
- [ ] 메뉴가 열린 상태에서 모니터 DPI를 바꿔도 멈추지 않는다.
- [ ] 100회 여닫은 뒤 GDI 객체 수와 USER 객체 수가 늘지 않는다.

**성능**
- [ ] 유휴 상태에서 5분간 CPU 점유율이 0.1% 미만이다.
- [ ] 창을 20개 열고 닫는 동안 독이 끊기지 않는다.
- [ ] 게임을 전체화면으로 실행하면 독과 상단바가 즉시 사라지고, 빠져나오면 즉시 복귀한다.

**회귀**
- [ ] 핀 고정과 해제, 드래그 재배치가 그대로 동작한다.
- [ ] 상단바 시계와 상태 항목이 정상 표시된다.
- [ ] `Win+Space` Spotlight가 그대로 열린다.
- [ ] 시작 메뉴 드롭다운이 그대로 열린다.
- [ ] 프로그램을 강제 종료한 뒤 다시 실행하면 태스크바가 복구되었다가 다시 숨겨진다.
- [ ] `bamti.exe --restore-taskbar`가 동작한다.

---

## 7. 참고: 삭제 대상 목록

1단계 완료 시점에 다음이 코드베이스에서 사라져 있어야 합니다. 하나라도 남아 있으면 이관이 끝나지 않은 것입니다.

```
dock.hpp   : MenuProc, HandleMenu, ShowContextMenu, EnsureMenuWindow, ApplyMenuChrome,
             ActivateMenuWindow, DismissMenuWindow, DestroyMenuWindow, CloseDockMenu,
             PaintMenu, MenuHitTest, MenuRowRect, ArmMenuMouseLeave
dock.hpp   : menu_hwnd_, menu_open_, menu_dismissing_, menu_expect_visible_, menu_hot_,
             menu_guard_until_, pending_menu_cmd_, pending_menu_app_, pending_menu_windows_,
             menu_window_cmds_, menu_rows_, context_index_
dock.cpp   : CreateMenuFont, kMenuPadDip 이하 메뉴 상수(팝업 쪽으로 이동), kDockMenuClass 등록
```

`dock.cpp`는 현재 2137줄입니다. 1단계 이후 **1500줄 이하**로 줄어드는 것이 정상입니다. 줄어들지 않았다면 팝업 쪽으로 옮겨야 할 코드가 독에 남아 있다는 신호입니다.
