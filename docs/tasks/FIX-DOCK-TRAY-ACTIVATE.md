# 작업 지시서: 트레이에만 있는 앱을 독에서 클릭했을 때 창이 늦게 돌아오는 문제를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/dock.cpp`, `src/dock.hpp`, `src/tray_mirror.cpp`, `src/tray_mirror.hpp`, `src/host.cpp` 입니다.

---

## 1. 증상

카카오톡 창을 모두 닫아 트레이로 보낸 상태에서 독 아이콘을 클릭하면 창이 돌아오기는 하지만 눈에 띄게 오래 걸립니다. 트레이 아이콘을 직접 클릭했을 때와 비교하면 차이가 큽니다.

---

## 2. 무엇이 원인인가 (코드로 확정)

### 2-1. 독은 창이 없으면 exe 를 새로 실행한다

`src/dock.cpp` 1498행의 클릭 분기입니다.

```cpp
} else if (app.running && app.hwnd != nullptr) {
  ActivateHwnd(app.hwnd);
} else {
  LaunchDockApp(app);
}
```

앞선 수정으로 창이 없어도 `app.running` 은 참이 되었지만, `app.hwnd` 는 여전히 `nullptr` 입니다. 그래서 `LaunchDockApp` 으로 갑니다.

`LaunchDockApp` 은 **exe 를 새 프로세스로 실행합니다.** 카카오톡처럼 단일 인스턴스로 도는 앱은 새 프로세스가 떠서 뮤텍스를 확인하고, 기존 인스턴스에 창을 띄우라고 알린 뒤, 스스로 종료합니다. 이 왕복이 지연의 정체입니다.

로그에 그대로 남아 있습니다.

```
17:22:16.519 [dock] click index=16 running=1 hwnd=0000000000000000 name=KakaoTalk
```

### 2-2. 그런데 훨씬 빠른 경로가 이미 있다

가로채기 백엔드는 같은 앱의 트레이 아이콘 소유 창을 이미 붙잡고 있습니다.

```
17:22:05.028 [tray] intercept item tip="KakaoTalk" exe=KakaoTalk.exe hwnd=0x50A9A uid=222
```

상단바 트레이에서 이 항목을 클릭하면 `TrayBackendIntercept::Invoke`(`src/tray_intercept.cpp` 370행)가 `owner` 창에 `callback_message` 를 직접 보냅니다. 새 프로세스를 띄우지 않으므로 즉시 반응합니다.

**독은 이 경로를 쓰지 않고 새 프로세스를 띄우고 있습니다.** 고칠 것은 이 갈림길입니다.

---

## 3. 먼저 측정할 것

구현에 들어가기 전에 두 가지를 확정하십시오.

### 3-1. 카카오톡 트레이 아이콘의 기본 동작

좌클릭으로 창이 뜨는지, 더블클릭이어야 뜨는지 확정해야 합니다. 상단 메뉴바 트레이의 카카오톡 항목을 좌클릭했을 때와 더블클릭했을 때 각각 창이 뜨는지 확인하십시오.

**사용자 화면에 입력을 합성하지 마십시오.** 눌러 봐야 하는 확인은 준비만 해 두고 사용자에게 부탁하십시오. 결과에 따라 4-2 에서 `dblclk` 인자를 정하십시오.

### 3-2. 현재 지연 시간

지금 상태에서 독 클릭부터 창이 뜨기까지 걸리는 시간을 재십시오. `[dock] click` 줄의 시각과, 그 뒤 창이 생겨 `CollectDockApps` 가 `hwnd` 를 다시 잡는 시각의 차이로 잽니다. 고친 뒤 같은 방법으로 다시 재서 비교하십시오.

---

## 4. 무엇을 고치는가

### 4-1. `TrayMirror` 에 exe 로 트레이 항목을 누르는 진입점을 더한다

`src/tray_mirror.hpp` 의 공개 영역에 더합니다.

```cpp
// exe 경로에 대응하는 트레이 항목을 클릭한 것처럼 동작시킨다.
// 대응 항목이 없으면 거짓을 돌려주고 아무것도 하지 않는다.
bool InvokeByExe(const std::wstring& exe_path);
```

구현은 다음 규칙을 지키십시오.

- `items_` 를 훑어 `ItemState::owner_pid` 가 0 이 아닌 항목만 후보로 삼습니다. 이 필드는 `Publish` 가 이미 채우고 있습니다.
- 후보의 `owner_pid` 로 `OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, ...)` 와 `QueryFullProcessImageNameW` 를 써서 실행 파일 경로를 얻고, `SameDockPin` 으로 인자와 비교합니다.
- **툴팁 문자열로 대조하지 마십시오.** 오탐이 납니다.
- 찾으면 `pending_invoke_` 와 `pending_right_`(거짓), `pending_dblclk_` 를 세우고 `SetEvent(wake_event_)` 로 워커를 깨운 뒤 참을 돌려줍니다. 기존 `DrainInvoke` 경로를 그대로 재사용하십시오. **호출한 스레드에서 직접 `Invoke` 를 부르지 마십시오.** 독의 클릭 처리가 막힙니다.
- 보충(fill)으로 들어온 항목은 `owner_pid` 가 0 이므로 자연히 제외됩니다. 이것은 의도한 동작입니다. 보충 항목은 소유 프로세스를 알 수 없어 잘못된 앱을 누를 위험이 있습니다.

### 4-2. 독의 클릭 분기에 이 경로를 끼운다

```cpp
} else if (app.running && app.hwnd != nullptr) {
  ActivateHwnd(app.hwnd);
} else if (app.running && tray_invoke_ && tray_invoke_(app.exe_path)) {
  // 트레이 항목을 눌렀다. 창은 앱이 띄운다.
} else {
  LaunchDockApp(app);
}
```

어느 경로로 갔는지 로그로 남기십시오.

```
[dock] click ... route=activate|tray|launch
```

**타임아웃 폴백을 두지 마십시오.** 트레이 항목을 누른 뒤 일정 시간 안에 창이 안 뜬다고 `LaunchDockApp` 을 부르면 프로세스가 중복 실행됩니다.

### 4-3. 배선

독은 `TrayMirror` 를 직접 참조하지 않습니다. `host.cpp` 에서 `MenuBar bar;` 와 `Dock dock;` 가 나란히 만들어지므로, 그 자리에서 독에 콜백을 주입하는 것이 가장 단순합니다.

```cpp
// dock.hpp
void SetTrayInvoke(std::function<bool(const std::wstring&)> fn);
```

`TrayMirror` 인스턴스를 `MenuBar` 가 들고 있다면 접근자를 하나 열어 쓰십시오. `WM_COPYDATA` 로 상단바에 문자열을 넘기는 방식은 배선이 늘어나므로, 콜백으로 닿을 수 있다면 그쪽을 고르십시오.

`TrayMirror` 가 아직 시작되지 않았거나 트레이 미러 설정이 꺼져 있으면 콜백은 거짓을 돌려주어야 합니다. 그러면 기존 `LaunchDockApp` 경로로 그대로 떨어집니다.

---

## 5. 검증

측정값으로 판정하십시오.

1. 카카오톡 창을 모두 닫아 트레이로 보낸 뒤 독 아이콘을 클릭합니다.
   - 로그에 `route=tray` 가 찍혀야 합니다.
   - 바로 뒤에 `[tray] intercept invoke` 계열 줄이 나와야 합니다.
   - `LaunchDockApp` 은 호출되면 안 됩니다.
   - 3-2 에서 잰 지연 시간과 비교해 눈에 띄게 줄었는지 확인하십시오.
2. 카카오톡을 **완전히 종료**한 상태에서 독 아이콘을 클릭합니다. `running=0` 이므로 `route=launch` 로 가서 정상 실행되어야 합니다.
3. 카카오톡 창이 **열려 있는** 상태에서 독 아이콘을 클릭합니다. `route=activate` 로 가야 합니다.
4. 트레이 아이콘이 없는 앱(예: Google Chrome)을 완전히 종료한 뒤 독에서 클릭해 정상 실행되는지 확인하십시오. 4-1 의 대조가 엉뚱한 항목을 잡으면 여기서 드러납니다.
5. 상단바 트레이 항목을 직접 클릭하는 기존 동작이 그대로인지 확인하십시오.

화면을 눈으로 봐야 하는 항목은 사용자에게 부탁하십시오.

---

## 6. 건드리지 말 것

- `ActivateHwnd` 경로와 `LaunchDockApp` 구현
- 독 우클릭 메뉴의 창 목록 경로(`src/dock.cpp` 2443행, 2477행)
- `DrainInvoke` 의 보충 항목 라우팅
- `TrayBackendIntercept::Invoke` 내부
- `CollectDockApps` 의 `running` 판정
