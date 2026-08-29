# 작업 지시서: 트레이 가로채기 백엔드

미러가 아이콘 그림 대신 툴팁 첫 글자를 그리고, 우클릭 컨텍스트 메뉴를 앱에 전달하지 못하는 문제를 함께 해결합니다. 두 결함의 원인이 같기 때문에 한 번에 풀립니다.

`TASK-TRAY-ICONS.md`가 제안한 Windows.Graphics.Capture 경로보다 **이 방식을 먼저 시도합니다.** 순서를 바꾼 근거는 1절에 있습니다.

---

## 0. 완료 조건

- 상단바에 툴팁 첫 글자가 아니라 실제 아이콘 그림이 나옵니다. Outlook과 Teams가 서로 다른 그림으로 구별됩니다.
- 아이콘 우클릭이 원래 앱의 컨텍스트 메뉴를 띄우고, 항목을 고르면 실행됩니다.
- 태스크바를 다시 표시하면 explorer의 알림 영역에 아이콘이 그대로 있습니다. **가로채기를 켠 상태에서도 explorer가 모든 아이콘을 알고 있어야 합니다.**
- bamti를 종료하면 트레이가 원래대로 돌아옵니다. 강제 종료 뒤에도 마찬가지입니다.
- 유휴 CPU가 0.2% 미만입니다.
- `/W4` 경고 없이 Debug와 Release가 빌드됩니다.

---

## 1. 왜 이 방법인가

### 1-1. 확인된 사실

`PLAN-TRAY-TO-TOPBAR.md` 5-6절이 이 방식을 서술하면서 "시간이 부족하면 통째로 생략하십시오. 필수가 아닙니다"라고 적었습니다. 그 판단의 전제는 UIA 백엔드로 충분한 결과를 얻을 수 있다는 것이었습니다. **그 전제가 틀렸습니다.** UIA는 아이콘 픽셀을 주는 패턴이 없고, 우클릭에 대응하는 패턴도 없습니다. 5단계를 끝내고 나서 남은 것이 글자와 좌클릭뿐입니다.

2026-08-29에 Seelen UI(MIT, `eythaann/Seelen-UI`)의 구현을 조사해 다음을 확인했습니다.

| 확인한 것 | 내용 |
|---|---|
| 커밋 이력 | `5dc081a9a2`(2025-09-26)에서 트레이 모듈을 제거했다가 `4216b3e1a3`(2025-11-08)에서 새로 만들었습니다. 현재도 유지됩니다. |
| 방식 | `Shell_TrayWnd` 클래스로 창을 만들어 `WM_COPYDATA`를 받습니다. 화면 캡처가 아닙니다. |
| 아이콘 | 받는 페이로드에 `HICON`이 그대로 들어 있습니다. |
| 레거시 툴바 | 소스 주석에 "Only available on Windows 10, since tray windows are XAML islands in Windows 11"이라고 적혀 있습니다. 우리 1단계 탐침 결과와 일치합니다. |

`src/background/modules/system_tray/application/tray_spy.rs`가 그 구현입니다. **코드를 옮겨 쓰지 마십시오.** 구조와 메시지 규약만 참고하고, 우리 `StatusSource` 구조에 맞게 C++로 새로 쓰십시오.

### 1-2. WGC보다 이것을 먼저 하는 이유

| | 가로채기 | WGC 캡처 |
|---|---|---|
| 아이콘 | `HICON` 원본 | 화면 캡처를 잘라내고 배경을 걷어내야 합니다 |
| 우클릭 메뉴 | 소유 창과 콜백 메시지 번호를 함께 받으므로 해결됩니다 | 여전히 불가능합니다 |
| 검증 | 프로덕션에서 동작하는 구현이 존재합니다 | 화면 밖 창을 캡처할 수 있는지 미검증입니다 |

WGC는 이 방식이 실패했을 때의 대안으로 남깁니다. `TASK-TRAY-ICONS.md`는 지우지 말고, 순서가 바뀌었다는 한 줄만 머리에 추가하십시오.

### 1-3. 감수하기로 한 위험

사용자가 위험을 알고 진행하기로 정했습니다. 다만 다음 넷은 설계로 눌러야 합니다.

1. `FindWindowW`가 우리 창을 먼저 돌려주지 않으면 아무것도 받지 못합니다(무해).
2. 우리가 받은 것을 explorer로 전달하지 못하면 **explorer가 그 앱을 영영 모릅니다.** 이것이 가장 큰 위험이며 5절이 다룹니다.
3. 이미 실행 중인 앱의 아이콘은 오지 않습니다. 7절이 다룹니다.
4. 관리자 권한으로 실행된 앱의 메시지는 UIPI 때문에 받지 못합니다. 이것은 해결할 수 없으므로 문서에 밝힙니다.

---

## 2. 파일 구성

| 파일 | 역할 |
|---|---|
| `src/tray_intercept.hpp` / `.cpp` | 스파이 창, 메시지 파싱, 전달, 아이콘 변환. **다른 모듈은 이 파일의 내부를 참조하지 않습니다.** |

고칠 파일은 `src/tray_backend.hpp`, `src/tray_mirror.hpp` / `.cpp`, `src/settings.hpp` / `.cpp`, `src/menu_bar.cpp`, `CMakeLists.txt`, `bamti.vcxproj`, `ARCHITECTURE.md`, `docs/STATUS-PROTOCOL.md`입니다.

가로채기는 `TrayBackend` 인터페이스의 두 번째 구현체로 만듭니다. `TrayMirror`는 어느 백엔드를 쓰는지만 알고, 그 안이 UIA인지 가로채기인지 신경 쓰지 않아야 합니다.

다만 가로채기는 폴링이 아니라 밀어 넣기(push) 방식이므로 인터페이스에 하나를 더합니다.

```cpp
// 백엔드가 스스로 변경을 알릴 수 있으면 이 콜백을 쓴다.
// UIA 백엔드는 쓰지 않고, 가로채기 백엔드는 메시지를 받을 때마다 부른다.
virtual void SetChangeSink(std::function<void()> on_change) { (void)on_change; }
```

`Enumerate`는 가로채기 백엔드에서도 그대로 동작해야 합니다. 내부에 보관 중인 목록을 복사해 돌려주면 됩니다. 그러면 `TrayMirror`의 변경 감지와 발행 경로를 그대로 재사용할 수 있습니다.

---

## 3. 스파이 창

### 3-1. 만들기

`Shell_TrayWnd` 클래스를 **우리 프로세스에** 등록합니다. 창 클래스는 프로세스별이므로 explorer의 등록과 충돌하지 않습니다.

```cpp
WNDCLASSW wc{};
wc.lpszClassName = L"Shell_TrayWnd";
wc.style = CS_HREDRAW | CS_VREDRAW;
wc.lpfnWndProc = SpyProc;
wc.hInstance = GetModuleHandleW(nullptr);
RegisterClassW(&wc);

// 메시지 전용 창(HWND_MESSAGE)으로 만들지 마십시오.
// FindWindowW는 최상위 창만 열거하므로 메시지 전용 창은 발견되지 않습니다.
HWND spy = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, L"Shell_TrayWnd", L"Shell_TrayWnd",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                           CW_USEDEFAULT, nullptr, nullptr, wc.hInstance, nullptr);
```

`ShowWindow`를 부르지 마십시오. 만들기만 하고 보여 주지 않습니다. `WS_EX_APPWINDOW`는 넣지 마십시오. 참고한 구현에는 있지만 우리에게는 필요 없고, 작업 표시줄에 항목이 생길 수 있습니다.

이 창은 **전용 스레드**에서 만들고 그 스레드에서 메시지 루프를 돌립니다. UI 스레드에 두면 트레이 메시지가 상단바 그리기와 같은 큐에 섞입니다.

`ChangeWindowMessageFilterEx(spy, WM_COPYDATA, MSGFLT_ALLOW, nullptr)`를 부르십시오. 권한이 다른 프로세스의 메시지를 최대한 받기 위해서입니다. 관리자 권한 앱은 그래도 오지 않습니다.

### 3-2. 우선순위 유지

`FindWindowW`의 열거 순서는 Z-order를 따르므로, 우리 창이 explorer의 트레이 창보다 앞에 있어야 메시지를 받습니다. 100ms 타이머로 `SetWindowPos(spy, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE)`를 반복합니다.

**주기를 측정으로 정하십시오.** 100ms는 참고한 구현의 값이고, 우리는 유휴 CPU 0.2% 예산이 있습니다. 1000ms로 시작해서 실제로 우선순위를 뺏기는지 확인하고, 뺏기면 낮추십시오. 어떤 값으로 정했고 왜 그렇게 정했는지 보고하십시오. 이 타이머가 예산을 넘기면 가로채기 자체를 재검토해야 합니다.

우리가 우선순위를 잃었는지는 `FindWindowW(L"Shell_TrayWnd", nullptr)`가 우리 창을 돌려주는지로 알 수 있습니다. 타이머마다 그것을 확인하고, 잃었을 때만 `SetWindowPos`를 부르는 편이 더 쌉니다.

---

## 4. 메시지 파싱

`Shell_NotifyIcon`이 보내는 것은 `WM_COPYDATA`이고 `COPYDATASTRUCT::dwData`가 종류를 가릅니다.

| `dwData` | 내용 | 처리 |
|---|---|---|
| 1 | 아이콘 등록·변경·삭제 | 파싱하고 **전달합니다** |
| 3 | `Shell_NotifyIconGetRect` 질의 | 6-3절대로 응답합니다. 전달하지 않습니다 |
| 그 밖 | 알 수 없음 | 파싱하지 말고 그대로 전달합니다 |

### 4-1. 구조체

**이 레이아웃은 비공개입니다.** 문서화된 적이 없고 빌드마다 바뀔 수 있습니다. 반드시 검증한 뒤에 쓰십시오.

```cpp
#pragma pack(push, 1)
struct TrayNotifyIconData {
  uint32_t callback_size;     // 보낸 쪽 NOTIFYICONDATA 크기
  uint32_t window_handle;     // 32비트로 잘린 HWND
  uint32_t uid;
  uint32_t flags;             // NIF_*
  uint32_t callback_message;
  uint32_t icon_handle;       // 32비트로 잘린 HICON
  wchar_t  tooltip[128];
  uint32_t state;
  uint32_t state_mask;
  wchar_t  size_info[256];
  uint32_t anonymous;         // uTimeout / uVersion 공용체
  wchar_t  info_title[64];
  uint32_t info_flags;
  GUID     guid_item;
  uint32_t balloon_icon_handle;
};

struct ShellTrayMessage {
  int32_t  magic_number;
  uint32_t message_type;      // NIM_ADD / NIM_MODIFY / NIM_DELETE / NIM_SETVERSION
  TrayNotifyIconData icon_data;
  uint32_t version;
};
#pragma pack(pop)
```

핸들이 32비트인 것은 오타가 아닙니다. `Shell_NotifyIcon`이 이 메시지를 만들 때 핸들을 32비트로 담습니다. 64비트에서 복원할 때는 **부호 확장 없이** 올려야 합니다.

```cpp
HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(data.window_handle));
HICON icon = reinterpret_cast<HICON>(static_cast<uintptr_t>(data.icon_handle));
```

### 4-2. 검증

다음을 전부 통과해야 그 메시지를 씁니다. 하나라도 실패하면 **파싱 결과를 버리고, 전달은 그대로 하십시오.**

1. `copy_data->cbData`가 `sizeof(ShellTrayMessage)` 이상이다.
2. `magic_number`가 회차마다 같은 값이다. 첫 메시지의 값을 기록해 두고 이후 비교하십시오. 값을 코드에 상수로 박지 말고 **관측한 값을 로그에 남기십시오.** 빌드마다 다를 수 있습니다.
3. `icon_data.callback_size`가 알려진 `NOTIFYICONDATAW` 크기 가운데 하나다.
4. `flags`에 `NIF_ICON`이 있을 때만 `icon_handle`을 씁니다.
5. `icon_handle != 0`이고 `GetIconInfo`가 성공한다.

연속 열 번 검증에 실패하면 파싱을 끄고 전달만 하는 모드로 내려간 뒤 로그에 남기십시오. 그 상태에서는 UIA 백엔드가 계속 동작하므로 미러가 멈추지 않습니다.

---

## 5. 전달 (이 절이 가장 중요합니다)

**우리가 받은 것을 explorer에 넘기지 못하면 그 앱의 아이콘은 explorer에서 영영 사라집니다.** 태스크바를 복구해도 없고, bamti를 지워도 앱을 다시 실행하기 전에는 돌아오지 않습니다. 이 절을 대충 만들면 사용자의 트레이를 망가뜨립니다.

### 5-1. 진짜 트레이 창 찾기

```cpp
HWND real = FindWindowW(L"Shell_TrayWnd", nullptr);
while (real == spy_) {
  real = FindWindowExW(nullptr, real, L"Shell_TrayWnd", nullptr);
}
```

찾지 못하면 전달할 곳이 없습니다. 그때는 **그 메시지를 버리지 말고**, 실패를 로그에 남기고 6절의 보류 목록에 넣어 두었다가 다음에 트레이 창을 찾으면 다시 보내십시오. explorer 재시작 중에 실제로 일어납니다.

### 5-2. 무엇을 전달하는가

`WM_COPYDATA`, `WM_ACTIVATEAPP`, `WM_COMMAND`, 그리고 `WM_USER` 이상 전부입니다. 나머지는 `DefWindowProcW`로 넘깁니다.

`WM_USER` 이상은 `PostMessageW`로, 그 아래는 `SendMessageW`로 보냅니다. `WM_COPYDATA`는 반드시 `SendMessageW`여야 합니다. 비동기로 보내면 데이터 수명이 보장되지 않습니다.

`SendMessageW`가 아니라 `SendMessageTimeoutW`를 쓰고 시한을 1000ms, `SMTO_ABORTIFHUNG`으로 두십시오. explorer가 멈췄을 때 우리 스파이 스레드가 함께 멈추면 안 됩니다. 시한을 넘기면 보류 목록에 넣고 다시 시도합니다.

### 5-3. 순서

파싱을 먼저 하고 전달을 나중에 하십시오. 전달이 실패해도 우리는 이미 아이콘을 얻은 상태여야 합니다. 반대로 하면 전달이 느릴 때 상단바 반영이 함께 늦어집니다.

### 5-4. 종료

`Stop()`에서 다음 순서를 지키십시오.

1. 우선순위 타이머를 멈춥니다.
2. 스파이 창을 파괴합니다. 이 시점부터 새 등록은 explorer로 직행합니다.
3. 보류 목록에 남은 것을 마지막으로 한 번 더 전달해 봅니다.
4. `TaskbarCreated`를 브로드캐스트합니다. 우리가 살아 있는 동안 등록에 실패했을 수 있는 앱들이 explorer에 다시 등록하게 합니다.
5. 메시지 루프를 끝내고 스레드를 join합니다.

강제 종료로 이 절차를 건너뛰어도, 5-2절의 전달이 제대로 동작했다면 explorer는 이미 모든 아이콘을 알고 있습니다. **전달이 안전장치입니다.**

---

## 6. 아이콘과 항목

### 6-1. 아이콘 변환

`HICON`을 보관하지 마십시오. 보낸 프로세스가 언제든 파괴할 수 있습니다. **받는 즉시 픽셀로 바꾸십시오.**

`icon_cache.cpp`의 `BitmapFromIcon(HICON, int px)`과 `BitmapToBgra`가 이미 있습니다. 그것으로 24픽셀 BGRA를 만들고, straight alpha PNG로 인코딩해 `IconKind::kPng`로 발행합니다. `cache_key`는 `HashStatusIcon`으로 채웁니다.

트레이 아이콘은 이미 알파를 가지고 있으므로 **배경 제거를 하지 마십시오.** `TASK-TRAY-MIRROR.md` 4-3절의 매트 알고리즘은 화면 캡처용이며 여기서는 필요 없습니다. `HasStraightAlpha`와 `StraightToPremul`이 이미 있으니 기존 경로를 그대로 타면 됩니다.

`NIM_MODIFY`가 왔을 때 `icon_handle`이 직전과 같으면 다시 변환하지 마십시오. 툴팁만 바뀌는 경우가 잦습니다.

### 6-2. 항목 구성

| 필드 | 값 |
|---|---|
| `key` | `hash(guid_item)`. `GUID`가 비어 있으면 `hash(window_handle, uid)` |
| `id` | 기존과 같이 `bamti.tray/<16진수>` |
| `tooltip` | `tooltip` 배열. 널 종료가 없을 수 있으므로 128자에서 잘라 쓰십시오 |
| `icon` | 6-1절의 PNG |
| `visible` | `state`에 `NIS_HIDDEN`이 없으면 참 |
| `order` | 등록 순서. 새 항목은 뒤에 붙입니다 |

`NIM_DELETE`는 목록에서 지웁니다. 소유 창이 죽었는데 `NIM_DELETE`를 보내지 않는 앱이 있으므로, `IsWindow(owner)`가 거짓인 항목은 다음 회차에 지우십시오.

### 6-3. `dwData == 3` 응답

`Shell_NotifyIconGetRect`가 오는 자리입니다. 앱이 자기 아이콘의 화면 위치를 물어서 팝업을 띄울 곳을 정합니다. 전달하지 말고 **우리가 응답하십시오.**

```cpp
struct NotifyIconIdentifier {
  int32_t  magic_number;
  int32_t  message;      // 1이면 x, 2면 y를 묻는다
  int32_t  callback_size;
  int32_t  padding;
  uint32_t window_handle;
  uint32_t uid;
  GUID     guid_item;
};
```

`message`가 1이면 x를, 2면 y를 `LRESULT`에 상하위 16비트로 같은 값을 채워 돌려줍니다. **커서 위치가 아니라 상단바에서 그 아이콘이 차지한 사각형의 좌표를 돌려주십시오.** `MenuBar`의 히트 영역에서 얻을 수 있습니다. 그러면 앱 팝업이 상단바 바로 아래에 열립니다.

해당 아이콘을 찾지 못하면 커서 위치를 돌려주십시오. 아무것도 안 돌려주면 앱이 화면 구석에 팝업을 띄웁니다.

---

## 7. 이미 실행 중인 앱

`WM_COPYDATA`는 등록 시점에만 옵니다. bamti가 시작하기 전에 실행된 앱의 아이콘은 오지 않습니다.

스파이 창을 만든 직후에 `TaskbarCreated`를 브로드캐스트해 재등록을 유도하십시오.

```cpp
const UINT msg = RegisterWindowMessageW(L"TaskbarCreated");
SendNotifyMessageW(HWND_BROADCAST, msg, 0, 0);
```

`SendMessageW`가 아니라 `SendNotifyMessageW`를 쓰십시오. 응답하지 않는 앱이 있으면 브로드캐스트가 막힙니다.

알려진 한계가 둘 있습니다. 문서에 적으십시오.

1. 재등록에 응답하지 않는 앱이 있습니다. 그런 앱은 이번 세션 동안 미러에 나타나지 않습니다.
2. 이 브로드캐스트는 explorer도 받아 트레이를 재구성합니다. 태스크바가 주차된 상태에서 재구성이 일어나면 `TaskbarController::Rehide()`가 다시 숨겨야 합니다. 브로드캐스트 직후 한 번 `EnsureHidden()`을 부르십시오.

---

## 8. 클릭 전달

이제 소유 창과 콜백 메시지 번호가 있으므로 `PLAN-TRAY-TO-TOPBAR.md` 5-4절의 메시지 직접 전달이 가능합니다.

```cpp
DWORD pid = 0;
GetWindowThreadProcessId(icon.owner, &pid);
AllowSetForegroundWindow(pid);

PostMessageW(icon.owner, icon.callback_msg, icon.uid, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN);
PostMessageW(icon.owner, icon.callback_msg, icon.uid, right ? WM_RBUTTONUP   : WM_LBUTTONUP);
if (right) {
  PostMessageW(icon.owner, icon.callback_msg, icon.uid, WM_CONTEXTMENU);
}
```

`NOTIFYICON_VERSION_4`로 등록한 앱은 `wParam`에 좌표를, `lParam`에 이벤트와 `uid`를 담은 형태를 기대합니다. **두 형태를 모두 보내지 마십시오.** 메뉴가 두 번 뜨는 앱이 생깁니다. 기본은 구형 규약이고, `NIM_SETVERSION`으로 받은 `anonymous` 값이 4 이상인 항목만 새 규약으로 보내십시오. 그 값이 규약 버전을 담고 있습니다.

클릭을 전달하기 전에 `PopupSurface`를 닫으십시오.

우클릭이 동작하게 되면 `MenuBar::ShowTrayIconMenu`의 자체 메뉴는 **가로채기 백엔드에서 띄우지 마십시오.** UIA 백엔드일 때만 띄웁니다. "알림 영역 잠시 표시"는 두 백엔드 모두에서 유지합니다.

---

## 9. 백엔드 고르기

두 백엔드를 동시에 쓰지 마십시오. 같은 아이콘이 두 번 나옵니다.

설정에 `tray_backend`를 두고 `"uia"`와 `"intercept"` 가운데 하나를 고릅니다. **기본값은 `"uia"`입니다.** 가로채기는 검증을 마친 뒤에 기본값 전환을 따로 정합니다.

`TrayMirror::Start()`가 설정을 보고 백엔드를 만듭니다. 가로채기가 스파이 창 생성에 실패하거나 4-2절의 검증이 계속 실패하면, 로그를 남기고 UIA 백엔드로 자동 전환하십시오. **미러가 아예 비는 상태를 만들지 마십시오.**

설정을 바꾸면 미러를 정지했다가 새 백엔드로 다시 시작합니다. 상단바 우클릭 메뉴에 "트레이 아이콘 가로채기(실험)" 항목을 두십시오.

---

## 10. 성능

| 항목 | 예산 | 확인 |
|---|---|---|
| 우선순위 타이머 | 유휴 CPU 기여 0.05% 미만 | 3-2절에서 정한 주기로 10분 측정 |
| 메시지 처리 1건 | 1ms 미만 | 로그 |
| 아이콘 변환 1건 | 5ms 미만 | 로그 |
| 유휴 CPU 전체 | 0.2% 미만 | 10분 측정 |
| 작업 집합 증가 | 가로채기를 켜고 끈 차이가 8MB 이하 | `WorkingSetPrivate` |

가로채기는 폴링이 없으므로 UIA 백엔드보다 유휴 CPU가 낮아야 정상입니다. 그렇지 않으면 우선순위 타이머가 원인입니다.

---

## 11. 하지 말아야 할 것

- 받은 메시지를 explorer로 전달하지 않는 경로를 만들지 마십시오. `dwData == 3` 하나만 예외입니다.
- `HICON`을 보관하지 마십시오. 즉시 픽셀로 바꾸십시오.
- 비공개 구조체를 검증 없이 신뢰하지 마십시오.
- 두 백엔드를 동시에 켜지 마십시오.
- `magic_number`를 코드에 상수로 박지 마십시오. 관측하고 비교만 하십시오.
- explorer 프로세스에 코드를 주입하거나 메모리를 쓰지 마십시오. 읽기도 하지 않습니다. 이 지시서의 어디에도 `ReadProcessMemory`가 필요한 곳은 없습니다.
- 참고한 구현의 코드를 그대로 옮기지 마십시오.

---

## 12. 검증

**기능**
- [ ] 상단바에 실제 아이콘 그림이 나옵니다. Outlook과 Teams가 구별됩니다.
- [ ] 아이콘 우클릭이 원래 앱의 컨텍스트 메뉴를 띄우고 항목이 실행됩니다.
- [ ] 좌클릭이 앱의 기본 동작을 실행합니다.
- [ ] 앱이 상태를 바꿔 아이콘 그림을 갱신하면 상단바에도 반영됩니다.
- [ ] 트레이 앱을 종료하면 상단바에서 사라집니다.
- [ ] 앱 팝업이 상단바 바로 아래에 열립니다(6-3절).

**explorer 보전 — 이 항목들이 가장 중요합니다**
- [ ] 가로채기를 켠 채 트레이 앱 다섯 개를 새로 실행한 뒤 태스크바를 복구하면, explorer의 알림 영역에 그 다섯 개가 **전부** 있습니다.
- [ ] bamti를 정상 종료한 뒤 태스크바를 복구해도 같습니다.
- [ ] bamti를 작업 관리자로 강제 종료한 뒤에도 같습니다.
- [ ] explorer를 재시작한 뒤 미러가 복구되고, explorer의 알림 영역도 정상입니다.
- [ ] 가로채기를 켜고 끄기를 20번 반복해도 트레이가 깨지지 않습니다.

**안정성**
- [ ] 스파이 창이 `FindWindowW` 우선순위를 잃었다가 되찾는 것을 로그로 확인했습니다.
- [ ] explorer가 응답하지 않는 상태에서 상단바가 멈추지 않고, 보류 목록이 쌓였다가 복구 후 전달됩니다.
- [ ] 검증 실패를 강제로 만든 임시 빌드에서 UIA 백엔드로 자동 전환됩니다.
- [ ] 관리자 권한 앱의 아이콘이 오지 않는 것을 확인하고 로그에 남겼습니다.
- [ ] 100번 켜고 끈 뒤 스레드·핸들·GDI 객체 수가 늘지 않습니다.

**절전**
- [ ] 유휴 CPU가 0.2% 미만이고, UIA 백엔드보다 낮습니다.

---

## 13. 함께 고칠 것: UIA 백엔드의 이벤트 폭주 판정

가로채기와 별개로, `FIX-TRAY-REACT.md`로 넣은 UIA 이벤트 구독이 실전에서 즉시 꺼지고 있습니다. 이 절만 먼저 고쳐도 됩니다.

```
12:38:23  [tray] backend=uia ... overflow=mirrored structure_changed=1
12:38:23  [tray] items +12 -0 now=12
12:38:30  [tray] structure_changed flood count=51 window_ms=5703; polling only
12:43:24  [tray] enum_ms=32 interval_ms=3200 items=11
```

구독한 지 7초 만에 이벤트 51개가 도착해 폭주 차단이 발동했고, 폴링으로 되돌아갔습니다. 그 뒤 주기가 3.2초이므로 **2초 반영 완료 조건을 여전히 지키지 못합니다.** 반응성을 고치려던 작업이 목적을 달성하지 못한 상태입니다.

### 원인

판정 기준을 잘못 잡았습니다. `FIX-TRAY-REACT.md` 1-3절이 "10초에 50회를 넘으면 구독을 끊는다"고 정했는데, **이벤트 수는 비용과 무관합니다.** 같은 문서 1-2절이 디바운스 300ms와 최소 간격 1000ms를 이미 강제하므로, 이벤트가 초당 100번 와도 실제 열거는 초당 한 번을 넘지 못합니다. 즉 비용은 이미 묶여 있는데 그것과 상관없는 값으로 구독을 끊고 있습니다.

`TreeScope_Subtree`로 태스크바 전체를 구독했으니 이벤트가 많은 것은 정상입니다. 작업 표시줄 버튼과 시계가 모두 그 안에 있습니다.

### 고치는 방법

**13-1.** 폭주 판정을 이벤트 수가 아니라 **실제 열거 횟수**로 바꿉니다. 10초 동안 열거를 8회보다 많이 했으면 그때 구독을 끊습니다. 디바운스가 정상 동작하는 한 이 값에 도달할 수 없으므로, 도달했다는 것은 디바운스가 망가졌다는 뜻입니다. 그때가 구독을 끊어야 할 때입니다.

**13-2.** 핸들러에서 `StructureChangeType`으로 한 번 거릅니다. 인자로 그대로 오므로 UIA를 호출하지 않고도 볼 수 있습니다. `ChildAdded`, `ChildRemoved`, `ChildrenBulkAdded`, `ChildrenBulkRemoved`, `ChildrenInvalidated`만 깨우고 나머지는 무시하십시오. **핸들러 안에서 UIA를 호출하지 않는다는 규칙은 그대로입니다.**

**13-3.** 이미 폭주로 꺼진 뒤에도 5분이 지나면 한 번 다시 구독해 보십시오. 일시적인 폭주였을 수 있습니다. 재구독 후에도 다시 꺼지면 그때는 이번 프로세스 수명 동안 포기합니다.

### 검증

- [ ] 상단바를 10분 띄워 둔 뒤에도 `polling only`로 내려가지 않습니다.
- [ ] 트레이 앱을 종료하면 2초 안에 상단바에서 사라집니다. 다섯 번 재서 최댓값을 적으십시오.
- [ ] 창을 20개 빠르게 열고 닫아도 열거가 초당 1회를 넘지 않습니다.
- [ ] 유휴 CPU가 0.2% 미만입니다.

---

## 14. 커밋

| 순서 | 작업 | 커밋 메시지 |
|---|---|---|
| 1 | 스파이 창, 우선순위 유지, **전달만** 하는 통과 모드 | `feat: 트레이 메시지를 가로채 explorer로 전달한다` |
| 2 | 메시지 파싱, 검증, 항목 발행, 아이콘 변환 | `feat: 가로챈 트레이 메시지에서 아이콘을 얻는다` |
| 3 | 클릭과 우클릭 전달, `GetRect` 응답 | `feat: 미러 아이콘의 우클릭 메뉴를 앱에 전달한다` |
| 4 | 백엔드 선택 설정과 자동 전환 | `feat: 트레이 백엔드를 설정에서 고른다` |
| 5 | UIA 이벤트 폭주 판정 수정(13절) | `fix: 이벤트 구독이 폭주 판정으로 꺼지지 않게 한다` |
| 6 | 문서 | `docs: 트레이 가로채기의 근거와 제약을 문서에 적는다` |

**1번 커밋을 반드시 먼저 독립적으로 검증하십시오.** 파싱을 전혀 하지 않고 받은 것을 그대로 explorer에 넘기기만 하는 상태입니다. 이 상태에서 트레이가 평소와 완전히 같아야 합니다. 아이콘이 하나라도 사라지면 전달 로직이 틀린 것이고, 그 위에 나머지를 쌓으면 원인을 가릴 수 없습니다.

확인 방법은 이렇습니다. 1번 커밋을 빌드해 띄운 뒤 트레이 앱 다섯 개를 새로 실행하고, 태스크바를 복구해 알림 영역에 다섯 개가 다 있는지 봅니다. bamti를 강제 종료한 뒤에도 다시 확인합니다. 이 확인 없이 2번으로 넘어가지 마십시오.
