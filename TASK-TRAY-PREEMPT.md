# 작업 지시서: 트레이 가로채기의 선점과 측정

작성일: 2026-08-31
선행 문서: `TASK-TRAY-INTERCEPT.md`, `RESEARCH-TRAY-REREGISTER.md`
목표: 가로채기 백엔드가 트레이 아이콘을 받는 비율을 올리고, 남는 손실의 원인을 로그로 가려낼 수 있게 만듭니다.

---

## 0. 완료 조건

1. bamti가 로그온 시 자동으로 시작하도록 켜고 끌 수 있습니다.
2. 설정이 `intercept`이면 스파이 창이 프로세스 시작 직후에 서고, 창을 세우기까지 걸린 시간이 로그에 남습니다.
3. `TaskbarCreated`를 브로드캐스트하기 전에 `FindWindowW` 우선순위를 실제로 확보했는지 확인하고 그 결과가 로그에 남습니다.
4. 우선순위 확인 주기가 상황에 따라 촘촘해졌다가 느슨해집니다.
5. explorer가 재시작하면 우선순위를 즉시 되찾고 재등록을 다시 유도하며, 그 과정이 무한 루프에 빠지지 않습니다.
6. 가로채기가 받은 항목과 UIA가 보는 항목을 로그에서 이름으로 대조할 수 있습니다.
7. 빌드 경고가 늘지 않습니다.

---

## 1. 왜 이 작업인가

`RESEARCH-TRAY-REREGISTER.md`가 서드파티 아이콘 여덟 개 가운데 넷만 받는 현상의 원인을 셋으로 나눴습니다.

| 원인 | 우리가 고칠 수 있는가 |
|---|---|
| 앱이 `TaskbarCreated` 재등록을 구현하지 않았다 | 고칠 수 없습니다 |
| 브로드캐스트가 도달하지 않는다(메시지 전용 창, UIPI) | 고칠 수 없습니다 |
| 우리가 늦게 시작해서 우선순위를 잃은 채로 브로드캐스트했다 | **고칠 수 있습니다** |

세 번째 원인을 없애는 가장 확실한 방법은 재등록에 의존하지 않는 것입니다. 트레이 앱은 대부분 로그온 시작 항목이고, 시작 항목은 explorer가 셸로 올라온 다음에 실행됩니다. 그 사이에 bamti가 들어가서 `Shell_TrayWnd`를 선점하면, 앱들이 생애 최초로 보내는 `NIM_ADD`를 우리가 직접 받습니다. 재등록 응답률이라는 변수 자체가 사라집니다.

다만 이 방법으로도 100%는 되지 않습니다. 시작 항목의 실행 순서는 보장되지 않고, explorer가 재시작하면 판이 다시 뒤집히며, 권한 경계는 순서와 무관합니다. 그래서 이번 작업의 목표는 "전부 받는다"가 아니라 **"받는 비율을 최대로 올리고, 못 받은 것이 무엇인지 이름으로 알아낸다"**입니다.

---

## 2. 범위 밖

다음은 이번에 하지 않습니다. 착수하지 마십시오.

- UIA 목록과 가로채기 아이콘을 합치는 하이브리드(`RESEARCH-TRAY-REREGISTER.md` 6-2절). 이번 측정 결과를 보고 다음에 정합니다.
- 관리자 권한 승격, 셸 대체(`Shell` 레지스트리 변경), explorer 작업 표시줄 강제 종료.
- explorer 프로세스 메모리 접근. `TASK-TRAY-INTERCEPT.md` 11절의 금지가 그대로 유효합니다.
- UIA 백엔드의 동작 변경. 8절의 진단 로그를 더하는 것만 허용합니다.
- 사용자 화면에 마우스나 키보드 입력을 합성하는 검증. 필요하면 보고서에 절차를 적어 사용자에게 부탁하십시오.

---

## 3. 항목 1: bamti 자체 로그온 자동 시작

지금 `dock.cpp`에 있는 `Run` 키 코드는 독에 고정한 **사용자 앱**을 자동 시작시키는 기능이고, bamti 자신을 등록하는 기능은 없습니다. 새로 만듭니다.

### 3-1. 새 파일

`src/autostart.hpp` / `src/autostart.cpp`를 만듭니다.

```cpp
namespace bamti {

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run 의 "bamti" 값이 있고,
// StartupApproved 로 꺼져 있지 않으면 true 를 돌려준다.
bool AutostartEnabled();

// 값을 만들거나 지운다. 성공하면 true.
bool SetAutostart(bool on);

}  // namespace bamti
```

- 값 이름은 `bamti`입니다. 독 앱은 `bamti-dock-` 접두사를 쓰므로 충돌하지 않습니다.
- 명령 문자열은 `GetModuleFileNameW(nullptr, ...)`로 얻은 절대 경로를 큰따옴표로 감싼 것입니다. 인자는 붙이지 않습니다.
- **상태 판정에 `StartupApproved\Run`을 반드시 함께 보십시오.** `Run` 값이 있어도 사용자가 작업 관리자에서 꺼 두면 실행되지 않습니다. `FIX-LOGIN-ITEM.md`에서 이미 한 번 고친 부분이므로, `dock.cpp` 440행부터 580행 사이의 기존 판정 로직을 그대로 따르십시오. 코드를 복사하지 말고 같은 규칙을 쓰라는 뜻입니다.
- 상태를 `settings.json`에 저장하지 마십시오. 레지스트리가 유일한 진실입니다. 사용자가 작업 관리자에서 끈 것을 우리가 되돌리면 안 됩니다.

### 3-2. 메뉴 배선

`menu_bar.cpp` 1205행의 `kTrayPeekCmd` 항목 다음, 구분선 앞에 넣습니다.

```cpp
AppendMenuW(menu, MF_STRING | (AutostartEnabled() ? MF_CHECKED : 0), kAutostartCmd,
            L"로그인 시 bamti 시작");
```

명령 처리에서 `SetAutostart(!AutostartEnabled())`를 부르고, 실패하면 로그만 남기십시오. 대화 상자를 띄우지 마십시오.

명령 id 상수는 기존 `kTray*` 상수 옆에 새 값으로 더합니다.

### 3-3. 이 방법의 한계를 알고 진행하십시오

`HKCU\Run` 항목은 Windows 8부터 explorer가 로그온 직후에 의도적으로 지연시켜 실행합니다. 다른 시작 항목도 같은 지연을 받으므로 상대 순서가 크게 뒤집히지는 않지만, 지연이 우리에게 불리하게 작용할 가능성이 남습니다.

**지금은 이 방법으로 진행하고 측정하십시오.** 결과가 부족하면 그때 작업 스케줄러의 로그온 트리거로 올리는 것을 다음 작업에서 검토합니다. 측정 없이 복잡한 쪽을 먼저 고르지 마십시오.

---

## 4. 항목 2: 스파이를 프로세스 시작 직후에 세운다

지금은 `TrayMirror::Start`(`tray_mirror.cpp` 112행)에서 `StartIntercept`를 부릅니다. 상태 소스 등록 시점이므로 창 생성과 여러 초기화가 끝난 뒤입니다. 이 시점을 앞으로 당깁니다.

### 4-1. 선기동 진입점

`tray_intercept.hpp`에 둘을 더합니다.

```cpp
// 프로세스 진입점에서 부른다. 설정의 tray_backend 가 "intercept" 일 때만
// 스파이를 띄우고, 그 밖에는 아무 일도 하지 않는다.
void PrestartInterceptTrayBackend();

// 선기동한 인스턴스의 소유권을 넘긴다. 선기동하지 않았으면 nullptr.
std::unique_ptr<TrayBackend> TakePrestartedInterceptTrayBackend();
```

`TrayMirror::StartIntercept`(`tray_mirror.cpp` 169행)는 먼저 `TakePrestartedInterceptTrayBackend()`를 시도하고, `nullptr`이면 지금처럼 `MakeInterceptTrayBackend()`로 새로 만듭니다. 선기동한 것을 넘겨받았을 때는 `Probe()`를 다시 부르지 마십시오. 이미 서 있습니다.

### 4-2. 호출 위치

`main.cpp`와 `host.cpp`의 진입 경로에서, 창을 만들기 전에, 설정 파일을 읽을 수 있게 된 직후에 `PrestartInterceptTrayBackend()`를 부릅니다. 스파이는 자기 전용 스레드에서 `CoInitializeEx`를 스스로 하므로 앞당겨도 됩니다.

### 4-3. 넘겨받지 않은 인스턴스를 정리하십시오

사용자가 설정을 바꿨거나 트레이 미러가 꺼져 있으면 아무도 선기동 인스턴스를 가져가지 않습니다. 그대로 두면 explorer 대신 우리가 메시지를 받아 놓고 아무도 쓰지 않는 상태가 됩니다. `TrayMirror::Start`가 끝난 뒤에도 남아 있으면 버리십시오. 소멸자가 스파이를 정리하고 종료 브로드캐스트를 보내므로 앱들은 explorer에 다시 등록합니다.

### 4-4. 로그

진입점에서 `GetTickCount64()`를 한 번 찍어 두고, 스파이 창을 만든 직후에 차이를 남깁니다.

```
intercept prestart elapsed_ms=<n> spy=0x<hwnd>
```

이 값이 크면 선점이 무의미해지므로, 측정 보고에 반드시 포함하십시오.

---

## 5. 항목 3: 브로드캐스트 전에 우선순위를 확보한다

`tray_intercept.cpp` 437행부터 448행이 지금 이렇게 되어 있습니다.

```cpp
SetTimer(spy, kPrioTimerId, kPrioTimerMs, nullptr);
SetWindowPos(spy, HWND_TOPMOST, ...);
// ready_ 이벤트, 로그
const UINT created = RegisterWindowMessageW(L"TaskbarCreated");
if (created != 0) {
  SendNotifyMessageW(HWND_BROADCAST, created, 0, 0);
}
```

`SetWindowPos`를 부르기만 하고 실제로 `FindWindowW`가 우리를 돌려주는지 확인하지 않습니다. 확보하지 못한 상태로 브로드캐스트하면 앱들의 재등록이 전부 explorer로 갑니다. 이것이 로그에서 관측된 문제입니다.

### 5-1. 요구 사항

`SetWindowPos` 다음, 브로드캐스트 앞에 확인 구간을 넣습니다.

- `FindWindowW(kSpyClass, nullptr) == spy`가 될 때까지 20ms 간격으로 확인하고, 최대 500ms까지 기다립니다.
- 확인 사이마다 `SetWindowPos(spy, HWND_TOPMOST, ...)`를 다시 부릅니다.
- 확보에 성공하면 `intercept priority acquired ms=<n>`를 남깁니다.
- 500ms 안에 확보하지 못하면 `intercept priority not acquired first=0x<hwnd>`를 남깁니다.
- **어느 쪽이든 브로드캐스트는 보냅니다.** 확보하지 못했더라도 explorer가 받아 정상 동작하므로 사용자에게 해가 없습니다.

이 구간은 스파이 전용 스레드이고 아직 메시지 루프에 들어가기 전입니다. 최대 500ms 동안 메시지를 꺼내지 않지만, 그동안 도착한 메시지는 스레드 큐에 쌓였다가 루프 진입 후 처리되므로 유실되지 않습니다.

### 5-2. `ready_` 이벤트 순서를 바꾸지 마십시오

`ready_` 이벤트는 지금처럼 창을 만든 직후에 세팅합니다. 확인 구간 뒤로 미루면 `TrayMirror::StartIntercept`가 최대 500ms 동안 막힙니다.

---

## 6. 항목 4: 우선순위 확인 주기를 상황에 맞춘다

지금은 `kPrioTimerMs = 1000` 고정입니다(`tray_intercept.cpp` 30행). 1초 주기이므로 우선순위를 잃고 되찾기까지 최대 1초의 구멍이 열립니다. 재등록이 몰리는 구간에서는 이 구멍이 치명적입니다.

### 6-1. 두 단계 주기

```cpp
constexpr UINT kPrioTimerFastMs = 100;
constexpr UINT kPrioTimerSlowMs = 1000;
constexpr ULONGLONG kFastWindowMs = 5000;
```

- 브로드캐스트를 보낸 직후에 fast로 전환합니다.
- fast로 들어간 뒤 5초가 지나면 slow로 되돌립니다.
- `KeepPriority`에서 우선순위 상실을 감지하면 다시 fast로 전환하고 5초를 새로 셉니다.

현재 주기를 멤버 변수로 들고 있다가, 값이 바뀔 때만 같은 타이머 id로 `SetTimer`를 다시 부르십시오. 매 틱마다 `SetTimer`를 부르지 마십시오.

### 6-2. `FlushPending`을 분리하십시오

지금 `Handle`의 `WM_TIMER` 분기(508행)가 `KeepPriority`와 `FlushPending`을 함께 부릅니다. fast 구간에서 `FlushPending`이 100ms마다 도는 것이 비용을 키운다면, `FlushPending`은 별도의 1초 타이머로 분리하십시오. 비용을 먼저 재고 판단하십시오.

### 6-3. 예산

유휴 CPU 0.2% 예산이 있습니다. fast는 5초 한정이므로 유휴 상태에는 영향이 없어야 하지만, 우선순위 상실이 반복되면 fast가 계속 유지될 수 있습니다. fast 구간에서 bamti의 CPU 사용률을 측정해 보고하십시오. 상시 fast 상태에서 예산을 넘기면 fast 주기를 200ms로 올리고 그 근거를 적으십시오.

---

## 7. 항목 5: explorer 재시작에 대응한다

explorer가 죽었다 살아나면 새 `Shell_TrayWnd`가 생기면서 우리 앞으로 올라갑니다. 그리고 곧바로 explorer가 `TaskbarCreated`를 뿌리므로 모든 앱이 동시에 재등록합니다. 우리가 밀려 있는 그 순간에 **여덟 개를 한꺼번에 놓칠 수 있습니다.**

지금 `menu_bar.cpp` 675행이 `TaskbarCreated`를 받아 `tray_.OnExplorerRestart()`를 부르지만, `TrayMirror::OnExplorerRestart`(`tray_mirror.cpp` 267행)는 `reset_pending_`만 세우고 가로채기 백엔드에게는 아무것도 알리지 않습니다.

### 7-1. 백엔드까지 전달

`tray_backend.hpp`에 가상 함수를 더합니다.

```cpp
// 셸이 재시작했다. 기본 구현은 아무 일도 하지 않는다.
virtual void OnShellRestart() {}
```

`TrayMirror::OnExplorerRestart`가 `intercept_`가 있으면 `OnShellRestart()`를 부르게 합니다. UIA 백엔드는 재정의하지 않습니다.

### 7-2. 가로채기 쪽 동작

`OnShellRestart`는 임의의 스레드에서 불릴 수 있으므로, 스파이 스레드에 전용 메시지를 `PostMessageW`로 보내고 즉시 돌아오십시오. `kStopMsg` 옆에 `kShellRestartMsg = WM_APP + 41`을 더합니다.

스파이 스레드에서 처리할 일은 셋입니다.

1. 5절과 같은 방식으로 우선순위를 재확보합니다. 최대 500ms, 20ms 간격입니다.
2. fast 주기로 전환하고 5초를 셉니다.
3. 우선순위를 되찾았으면 `TaskbarCreated`를 다시 브로드캐스트합니다. 되찾지 못했으면 보내지 마십시오. 보내 봐야 explorer가 받으므로 이득이 없고 아이콘만 깜빡입니다.

### 7-3. 무한 루프를 반드시 막으십시오

**이것이 이 항목에서 가장 위험한 부분입니다.** 우리가 보낸 `TaskbarCreated` 브로드캐스트는 우리 프로세스의 MenuBar 창도 받습니다. 그러면 `OnExplorerRestart`가 다시 불리고, 다시 브로드캐스트가 나가고, 끝없이 반복됩니다.

억제 규칙을 이렇게 둡니다.

- 자체 브로드캐스트를 보내기 직전에 `GetTickCount64()`를 기록합니다.
- `OnShellRestart`가 불렸을 때 마지막 자체 브로드캐스트로부터 2000ms가 지나지 않았으면 **아무 일도 하지 않고 돌아옵니다.** 이때 `intercept shell restart suppressed`를 남깁니다.
- 억제와 무관하게 60초 안에 자체 브로드캐스트는 최대 3회까지만 보냅니다. 초과하면 건너뛰고 `intercept rebroadcast throttled`를 남깁니다.

억제 판정은 **가로채기 백엔드 안에서** 하십시오. `MenuBar`가 `TaskbarCreated`를 받아 하는 다른 일(`TaskbarController::RewatchTray()`, `taskbar_.EnsureHidden()`)은 억제하면 안 됩니다.

기존 스파이 기동 시 브로드캐스트(447행)와 종료 시 브로드캐스트(465행)도 같은 타임스탬프를 갱신하게 하십시오. 기동 직후에 들어오는 자기 브로드캐스트가 재브로드캐스트를 유발하면 안 됩니다.

---

## 8. 항목 6: 항목 이름을 로그에 남긴다

지금 로그로는 어떤 앱이 잡혔고 어떤 앱이 빠졌는지 가릴 수 없습니다. `TASK-TRAY-MIRROR.md` 10절이 "개별 항목의 툴팁은 적지 않습니다"로 정한 결과입니다. **이번에 그 결정을 진단 목적으로 뒤집습니다.** 해당 문서에 결정이 바뀌었다는 한 줄을 남기십시오.

### 8-1. 가로채기 쪽

항목이 목록에 처음 들어올 때 한 줄 남깁니다.

```
intercept item tip="<툴팁>" exe=<실행 파일 이름> hwnd=0x<owner> uid=<n> guid=<0|1>
```

실행 파일 이름은 `owner` 창에서 얻습니다.

```cpp
DWORD pid = 0;
GetWindowThreadProcessId(owner, &pid);
HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
// QueryFullProcessImageNameW 로 경로를 얻고 파일명만 남긴다
```

실패해도 로그는 남기고 `exe=?`로 적으십시오. 진단이 목적이므로 실패가 흐름을 막으면 안 됩니다.

### 8-2. UIA 쪽

UIA 열거 결과에 새 항목이 나타날 때 같은 방식으로 한 줄 남깁니다.

```
uia item tip="<툴팁>" system=<0|1> overflow=<0|1>
```

두 줄을 대조하면 UIA가 보는 여덟 개 가운데 가로채기가 못 받은 것이 무엇인지 이름으로 나옵니다.

### 8-3. 폭주 방지

키마다 최초 1회만 남깁니다. 항목이 사라졌다가 다시 나타나면 다시 1회를 허용하되, **프로세스 수명 동안 같은 키에 대해 최대 5회**로 제한하십시오. 트레이 아이콘을 빠르게 갱신하는 앱이 있으므로 상한이 필요합니다.

---

## 9. 검증

### 9-1. 재부팅 없이 확인할 것

Cursor가 직접 확인하고 결과를 보고하십시오.

| 확인 항목 | 방법 |
|---|---|
| 빌드 | 기존 절차 그대로. 경고가 늘지 않아야 합니다 |
| 선기동 로그 | 설정을 `intercept`로 두고 bamti를 재실행합니다. `intercept prestart elapsed_ms=`가 나오는지 봅니다 |
| 우선순위 확보 | `intercept priority acquired ms=` 또는 `not acquired`가 나오는지 봅니다 |
| 진단 로그 형식 | `intercept item`과 `uia item` 두 줄이 툴팁과 실행 파일명을 담고 나오는지 봅니다 |
| 자동 시작 토글 | 메뉴에서 켜고 끈 뒤 `HKCU\...\Run`의 `bamti` 값이 생기고 사라지는지 봅니다 |
| explorer 재시작 | 9-2절 |

### 9-2. explorer 재시작 검증

`taskkill /f /im explorer.exe`를 실행하면 Windows가 explorer를 자동으로 되살립니다. 되살아나지 않으면 `start explorer.exe`로 직접 띄우십시오. 실행 전에 저장하지 않은 작업이 없는지 확인하십시오.

확인할 것은 셋입니다.

1. `intercept shell restart`와 우선순위 재확보 로그가 나옵니다.
2. 재브로드캐스트가 **한 번만** 나가고 `suppressed`가 뒤따릅니다. 반복되면 7-3절의 억제가 깨진 것입니다.
3. 상단바의 트레이 아이콘이 되살아납니다.

### 9-3. 사용자가 확인할 것

로그온 선점의 실제 효과는 재부팅해야 알 수 있습니다. Cursor는 재부팅하지 말고, 보고서에 사용자가 밟을 절차를 적으십시오.

1. 메뉴에서 `트레이 아이콘 가로채기(실험)`와 `로그인 시 bamti 시작`을 켭니다.
2. 재부팅합니다.
3. 로그온 후 2분쯤 지난 뒤 `%USERPROFILE%\.bamti\bamti.log`를 확인합니다.
4. `intercept item` 줄의 개수와 `uia item` 줄의 개수를 비교합니다.

---

## 10. 보고 형식

작업을 마치면 다음을 적으십시오.

- 항목별로 고친 파일과 함수
- 우선순위 타이머 주기를 어떤 값으로 정했고 그 근거가 무엇인지
- fast 구간에서 측정한 bamti의 CPU 사용률
- 9-1절과 9-2절의 실제 결과. 로그 줄을 그대로 붙이십시오
- 9-3절 절차를 사용자가 밟을 수 있게 정리한 문단
- 구현하면서 이 지시서와 다르게 판단한 부분이 있으면 그 이유

문서 갱신도 함께 하십시오.

- `ARCHITECTURE.md`에 자동 시작과 선기동 경로를 반영합니다.
- `TASK-TRAY-MIRROR.md` 10절에 툴팁 로깅 결정이 바뀌었다는 한 줄을 더합니다.
- `RESEARCH-TRAY-REREGISTER.md` 6-1절에 이번 작업으로 무엇을 했는지 적습니다. 측정 결과는 사용자가 재부팅한 뒤에 채우므로 자리만 비워 두십시오.

---

## 11. 금지 사항

1. explorer 프로세스를 열거나 그 주소 공간에 접근하지 마십시오.
2. 관리자 권한으로 승격하지 마십시오.
3. `Shell` 레지스트리를 건드리거나 explorer를 셸에서 밀어내지 마십시오.
4. UIA 백엔드의 열거와 호출 동작을 바꾸지 마십시오. 8-2절의 로그만 더합니다.
5. 사용자 화면에 마우스나 키보드 입력을 합성하지 마십시오.
6. 하이브리드 병합에 착수하지 마십시오. 측정 결과를 보고 다음에 정합니다.
