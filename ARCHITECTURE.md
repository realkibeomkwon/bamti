# bamti 아키텍처

Windows 11 셸을 교체하지 않고, explorer 옆에서 **상단 메뉴 바**와 **독**을 제공하는 네이티브 오버레이입니다. 트레이 아이콘을 클릭해야만 보이던 상태(예: LLM 사용량)를 상단바에 상시 표시하는 것이 핵심 목적입니다.

이 문서는 구현 기준입니다. 성능, 빌드 호환성, 기존 Windows 앱 사용을 스택 선택보다 앞에 둡니다.

## 1. 목표와 비범위

### 목표

- 상단바에 시계, 시스템 상태, 앱이 푸시한 텍스트를 클릭 없이 표시한다.
- 화면 하단(또는 사용자가 고른 가장자리)에 맥 스타일 독을 둔다.
- 기존 Win32 / WinUI / WPF / Store 앱은 그대로 실행되고, 최대화 시 작업 영역이 바와 겹치지 않는다.
- 상주 프로세스의 RAM·입력 지연을 최소화한다.
- Windows 11 Home / Pro / Enterprise / Education에서 동일 바이너리로 동작한다. SKU가 아니라 OS 빌드 차이를 호환 축으로 본다.

### 비범위 (1.0)

- explorer.exe 인젝션, IAT 후킹, 미공개 태스크바 COM 패치
- Winlogon 셸을 bamti로 교체
- Electron, Tauri, WebView2로 상단바·독을 그리는 일
- 기존 `NOTIFYICON`을 맥 메뉴바 텍스트로 “자동 변환”하는 일. 트레이 미러는 아이콘을 다시 그리는 것이지 툴팁을 텍스트 항목으로 바꾸는 일이 아니므로 이 비범위와 충돌하지 않는다.
- 독 show/hide 애니메이션
- Windows 11 위젯 보드를 상단바 안에 임베드하는 일

## 2. 설계 원칙

1. **오버레이하고, 패치하지 않는다.** explorer는 살린 채 기본 태스크바만 숨기거나 자동 숨김으로 둔다. 누적 업데이트마다 깨지는 경로를 제품 핵심에 두지 않는다.
2. **문서화된 API를 기본으로 한다.** `SHAppBarMessage`, WinEvent, DWM, Direct2D처럼 OS에 들어 있는 API를 우선한다. 미공개 API는 트레이 호스팅처럼 격리된 뒤에만 검토한다.
3. **상주 UI는 가장 가벼운 네이티브 경로만 쓴다.** 설정 창처럼 가끔 여는 표면에만 무거운 UI 프레임워크를 허용한다.
4. **상태 텍스트는 1등 시민이다.** 트레이 아이콘 호스팅은 하위 호환이다. LLM 사용량 같은 문자열은 Status Item 프로토콜로만 보장한다.
5. **모션은 입력 지연이 아닐 때만 넣는다.** 독이 나타나고 사라질 때는 즉시 토글한다.

## 3. 기술 스택

상주 크롬(메뉴 바, 독)과 설정 UI를 분리한다.

| 역할 | 선택 | 이유 |
|---|---|---|
| 언어 | C++23, MSVC | COM·HWND·훅과 맞고, 상주 RAM이 가장 작다 |
| 빌드 | CMake | 단일 솔루션, x64와 ARM64를 같은 트리에서 만든다 |
| 윈도우 | Win32 HWND | AppBar, layered window, 히트 테스트의 본진 |
| 셸 | `shell32` / `SHAppBarMessage` | 작업 영역 예약, 태스크바와의 공존 |
| 렌더 | Direct2D, DirectWrite | 상시 텍스트와 아이콘을 GPU로 그림 |
| 재질 | `DwmSetWindowAttribute` + `DWMWA_SYSTEMBACKDROP_TYPE` | OS 내장 Mica / Acrylic. WASDK 런타임이 필요 없다 |
| WinRT | C++/WinRT | 가상 데스크톱, 썸네일, 테마 등 WinRT/COM |
| RAII | WIL | HRESULT와 핸들 수명 |
| 배포 | unpackaged (WiX 또는 MSI) | 셸 도구에 MSIX 샌드박스가 짐이 된다 |
| 설정 UI | 동일 프로세스 Win32, 또는 별도 WinUI 3 창 | 상주 경로가 아니면 WinUI 3를 허용한다 |

### 쓰지 않는 것

- Electron, Tauri, WebView2를 상단바·독에 사용
- 상주 바에 WinUI 3 XAML Island
- 상주 바에 WPF / Qt / Flutter
- explorer 인젝션 프레임워크 (Windhawk 모듈 형태 포함)
- ATL/MFC를 새 코드의 UI 기반으로 사용

Windows App SDK는 기본 의존성이 아니다. `MicaController` 등이 필요해지면 설정 창이나 후순위 경로에서만 검토한다. 상단바와 독의 Mica는 DWM 시스템 백드롭으로 충분하다.

## 4. 프로세스와 모듈

권한은 일반 사용자로 유지한다. 항상 관리자 권한으로 뜨는 설계는 하지 않는다.

```
bamti.exe                          unpackaged, 낮은 권한
 ├─ host                           메시지 루프, 싱글 인스턴스, 디스플레이 변경
 ├─ menu_bar                       상단 AppBar HWND, Direct2D 텍스트
 ├─ dock                           오버레이 HWND, 실행 중 앱·핀 아이콘
 ├─ status                         named pipe 서버, Status Item 레지스트리
 ├─ work_area                      AppBar 등록, 모니터별 작업 영역
 ├─ tasks                          창 열거, 활성화, 플래시, (추후) 진행률
 ├─ fullscreen                     IAppVisibility / ABN_FULLSCREENAPP
 └─ tray_host                      1.0 이후. 기존 NotifyIcon 호스팅

설정
 └─ settings                       같은 프로세스의 Win32 창, 또는 별도 WinUI 3
```

모듈 경계는 정적 라이브러리로 나눈다. 상주 경로에 DLL 플러그인 호스트를 두지 않는다. Status Item은 프로세스 밖 앱이 파이프로 붙는 방식이다.

멀티 모니터는 모니터마다 메뉴 바 HWND를 둔다. 독은 주 모니터 하나, 또는 모니터별 표시를 설정으로 연다. 좌표와 DPI는 창이 있는 모니터의 `GetDpiForWindow`를 따른다.

## 5. 셸 통합

### 5.1 explorer와 공존

bamti는 로그인 셸이 아니다. explorer가 데스크톱, 파일 창, 알림 영역을 계속 담당한다.

시작 시 대략 다음 순서를 따른다.

1. 다른 bamti 인스턴스가 있으면 종료하거나 포그라운드로 올린다.
2. 기본 태스크바를 숨기거나 자동 숨김으로 바꾼다. 실패하면 태스크바와 바가 겹칠 수 있으므로, 사용자에게 알리고 메뉴 바만 켠 채 독은 끈다.
3. 모니터마다 상단 AppBar를 등록한다 (`ABM_NEW` → `ABM_QUERYPOS` → `ABM_SETPOS`).
4. 독을 만든다. 독은 기본값으로 AppBar에 등록하지 않는다.
5. WinEvent와 디스플레이 변경을 구독한다.

종료 시 `ABM_REMOVE`를 보내고 태스크바 표시를 복구한다. 비정상 종료에 대비해 `%LOCALAPPDATA%\bamti\taskbar.guard`에 원래 `ABM_GETSTATE` 값을 남기고, 다음 시작 때 복구한 뒤 다시 숨긴다. 프로세스를 강제 종료하면 `SW_HIDE`가 남을 수 있으므로, 그 경우에는 `bamti.exe --restore-taskbar`로 응급 복구한다. `WM_ENDSESSION`에서도 복구한다.

### 5.2 작업 영역

최대화 창이 상단바와 겹치지 않게 하는 수단은 AppBar다. `SPI_SETWORKAREA`를 직접 만지는 경로는 다른 도구와 싸우므로 쓰지 않는다.

독은 맥과 같이 **작업 영역을 먹지 않는 오버레이**가 기본이다. 최대화 창 아래쪽에 여백을 남기는 모드는 후순위 설정이다.

### 5.3 전체화면

게임·영상 전체화면에서는 메뉴 바와 독이 즉시 사라져야 한다. 복귀도 즉시여야 한다. 페이드나 슬라이드를 넣지 않는다.

우선 신호는 `ABN_FULLSCREENAPP`과 `IAppVisibility`다. 부족하면 포그라운드 HWND의 모니터 작업 영역 비교로 보완한다.

### 5.4 태스크바 버튼 계약

앱은 진행률·오버레이 아이콘을 explorer 태스크바(`ITaskbarList3`)로 보낸다. 태스크바를 숨기면 bamti가 그 메시지를 자동으로 받지 못한다. 1.0 독은 창 열거와 아이콘으로 전환·실행을 제공한다. 진행률 배지는 후순위에서 UI Automation 또는 창 속성 관찰로 재구성한다. explorer 태스크바 HWND를 후킹해 가로채지 않는다.

## 6. 상단 메뉴 바

### 6.1 창

- `WS_EX_TOOLWINDOW` | `WS_EX_NOACTIVATE` (클릭 시에만 활성화)
- 가장자리: `ABE_TOP`
- 높이: 시스템 캡션 메트릭과 DPI를 기준으로 고정. 사용자가 px를 미세 조정할 수 있게 한다
- 배경: `DWMSBT_MAINWINDOW` (Mica). 투명 효과 끄기·고대비에서는 단색 폴백
- 글꼴: Segoe UI Variable, DirectWrite
- 색: 시스템 Light/Dark와 고대비를 따른다

레이아웃은 왼쪽(앱/메뉴)·가운데(선택적 전역 상태)·오른쪽(Status Item, 시계, 트레이 진입점)이다. 맥 메뉴바와 같이 오른쪽이 상태 영역이다.

### 6.2 표시 내용

항상 그리는 것:

- 시계 (초 표시는 설정)
- bamti 자체 메뉴 (설정, 종료, 태스크바 복구)

Status Item 프로토콜로 붙는 것:

- 짧은 텍스트 (예: `Opus 42%`)
- 선택 아이콘
- 클릭 시 앱이 지정한 동작 (HWND로 메시지, 또는 앱이 연 팝업)

알림 영역 아이콘은 explorer가 계속 소유한다. bamti는 읽기 전용으로 미러하고 좌클릭만 UIA Invoke로 위임한다. 우클릭 메뉴는 미러하지 않으며, 필요하면 알림 영역을 잠시 되돌린다. 오버플로에 숨긴 아이콘은 두 번째 UIA 루트로 미러한다.

## 7. 독

### 7.1 창

- 작업 영역을 예약하지 않는 오버레이 HWND
- 기본 위치: 주 모니터 하단 중앙
- 아이콘: `IShellItemImageFactory` / 창 아이콘, DPI별 캐시
- 실행 중 창은 `SetWinEventHook` + `EnumWindows`로 추적한다. 최소화·가상 데스크톱은 `IVirtualDesktopManager`로 걸러 낸다

### 7.2 나타남과 사라짐

독 자동 숨김은 **애니메이션이 없다.** 보이거나 안 보이거나, 한 프레임에 전환한다. `transform` 트랜지션, 페이드, 슬라이드를 넣지 않는다.

맥에서도 동일하게 쓰는 설정(`autohide-time-modifier 0`)을 기본 동작으로 삼는다.

타이밍은 모션과 별개다.

| 파라미터 | 기본 | 역할 |
|---|---|---|
| show delay | 0 ms | 가장자리에 닿으면 바로 보인다 |
| hide delay | 100 ms | 아이콘으로 포인터를 옮기는 동안 꺼지지 않게 한다 |
| show/hide animation | 없음 | 입력 지연을 만들지 않는다 |

전체화면 진입·해제에도 같은 즉시 토글을 쓴다.

숨김 유예가 0이면 화면 하단을 가로지를 때 깜빡일 수 있다. 이는 히트 영역과 타이밍 문제이지, 애니메이션을 다시 넣어야 한다는 신호가 아니다.

### 7.3 입력

아이콘은 pointer-down에서 눌림 상태를 즉시 그린다. 이 눌림 표시는 show/hide 애니메이션이 아니다. 클릭 피드백이 없으면 실행 여부를 알 수 없다.

독 확대(magnification)는 1.0에 넣지 않는다. 넣더라도 자동 숨김과 독립된 옵션이다.

## 8. Status Item 프로토콜

공개 명세는 `docs/STATUS-PROTOCOL.md`다. v1 클라이언트는 계속 동작하고, v2는 아이콘·상태·패널 행을 같은 규격으로 보낸다.

bamti는 named pipe `\\.\pipe\bamti-status`의 서버다. 사용량 표시 앱과 내장 위젯, 트레이 미러가 모두 이 경로로 상단바에 붙는다.

## 9. 기존 앱과 위젯

| 대상 | 1.0 기대 |
|---|---|
| 일반 데스크톱 앱 | 실행·전환·최대화(상단바 여백) |
| Store / PWA | 창이 있으면 독에 나타낸다 |
| `ITaskbarList3` 진행률 | 미구현. 창은 보인다 |
| Jump List | 후순위 |
| Windows 11 위젯 보드 | 임베드하지 않는다. 보드를 여는 버튼과 내장 위젯 공급자(배터리, CPU, 네트워크)를 제공한다 |
| Rainmeter 등 데스크톱 위젯 | 작업 영역 변경만 영향을 준다. bamti가 호스팅하지 않는다 |
| 기존 트레이 아이콘 | 기본은 UIA 읽기 전용 미러. 실험 가로채기를 켜면 `HICON`과 우클릭을 함께 받는다. explorer에는 받은 메시지를 그대로 넘긴다 |

“기존 Windows 앱을 새 UI에서 사용”은 **창이 작업 영역 안에서 정상 동작하는 것**을 뜻한다. 트레이 본문이 상단바로 이사하는 것은 Status Item을 구현한 앱에만 해당한다.

### 9.1 트레이 미러

기본 백엔드는 UI Automation이다. 설정 `tray_backend`는 `"uia"`(기본) 또는 `"intercept"`다. 두 백엔드를 아이콘 소스로 동시에 쓰지 않는다.

UIA는 알림 영역 버튼을 열거하고 좌클릭은 `Invoke`로 위임한다. 아이콘 픽셀을 주는 패턴이 없고, 우클릭 컨텍스트 메뉴를 앱에 넘기는 패턴도 없다. 그래서 기본 경로에서는 툴팁 첫 글자 글리프와 bamti 자체 우클릭 메뉴(“알림 영역 잠시 표시”)만 제공한다. 오버플로에 숨긴 아이콘은 `TopLevelWindowForOverflowXamlIsland`의 자식 브리지에서 열거하며, `tray_overflow_icons`로 끌 수 있다.

가로채기 백엔드는 `Shell_TrayWnd` 클래스의 숨은 창으로 `WM_COPYDATA`를 받은 뒤, 같은 메시지를 explorer의 진짜 `Shell_TrayWnd`로 전달한다. 페이로드의 `HICON`을 즉시 24px PNG로 바꿔 상단바에 그리고, 소유 창과 콜백으로 우클릭을 앱에 전달한다. 전달에 실패하면 explorer가 그 앱을 영영 모르게 되므로, 파싱 실패와 무관하게 전달은 유지한다. `dwData == 3`(`Shell_NotifyIconGetRect`)만 전달하지 않고 상단바 좌표로 응답한다. 가로채기 항목의 키는 GUID가 있으면 GUID에서, 없으면 `(hwnd, uid)`에서 만든다. hwnd 기반 키는 앱을 다시 띄우면 바뀌므로 숨김이 풀린다.

설정이 `intercept`이면 메뉴 바 창을 만들기 전에 스파이를 세운다(`PrestartInterceptTrayBackend`). 브로드캐스트 전에 `FindWindowW`가 스파이 창을 돌려줄 때까지 최대 500ms 기다리고, 직후 5초는 우선순위 확인을 100ms로 촘촘히 한다. explorer가 재시작하면 우선순위를 다시 잡고, 잡혔을 때만 `TaskbarCreated`를 다시 보낸다. 자체 브로드캐스트는 2초 억제와 60초 3회 상한으로 루프를 막는다.

bamti 자신은 `HKCU\...\Run`의 `bamti` 값으로 로그온 자동 시작을 켠다. `StartupApproved\Run`으로 꺼져 있으면 켜진 것으로 보지 않으며, 이 상태는 `settings.json`에 저장하지 않는다. 상단바 우클릭 메뉴의 “로그인 시 bamti 시작”으로 토글한다.

가로채기 한계는 문서화된 제약이다.

- 관리자 권한 앱의 메시지는 UIPI 때문에 받지 못한다.
- `TaskbarCreated`에 재등록하지 않는 앱은, bamti가 켜진 뒤에 등록한 것만 미러에 나온다.
- `TaskbarCreated` 브로드캐스트는 explorer도 받아 트레이를 재구성한다. 태스크바가 주차된 상태면 `EnsureHidden()`으로 다시 숨긴다.
- explorer가 자기 `Shell_NotifyIcon`으로 등록하는 아이콘 가운데 일부는 클릭을 XAML 트리에서 처리하므로, 콜백 메시지를 보내도 반응하지 않는다. 볼륨과 배터리가 그렇고 블루투스는 정상 동작한다. 반응하지 않는 항목은 상단바 우클릭 메뉴의 `트레이 아이콘` 하위 메뉴에서 숨긴다.
- Windows.Graphics.Capture는 가로채기가 실패했을 때의 대안으로 남긴다. `TASK-TRAY-ICONS.md`를 따른다.

상단바 우클릭 메뉴의 “트레이 아이콘 가로채기(실험)”으로 켠다. 스파이 창 생성 실패나 파싱 연속 실패 때는 UIA로 자동 전환한다.

## 10. 그래픽과 테마

- 상주 프레임: Direct2D 즉시 모드. 상태 문자열이 바뀔 때만 해당 영역만 다시 그린다.
- 텍스트: DirectWrite, 모니터 DPI, ClearType
- 재질: `DWMWA_SYSTEMBACKDROP_TYPE`. Win11 22H2(22621) 미만은 지원하지 않는다.
- 라운드 코너: `DWMWA_WINDOW_CORNER_PREFERENCE` (독). 메뉴 바는 화면 폭을 가득 채우므로 라운드하지 않는다.
- 다크/라이트: `WM_SETTINGCHANGE` / `ImmersiveColorSet`
- 접근성: 고대비 테마에서는 백드롭을 끄고 시스템 색만 쓴다. OS “애니메이션 효과” 설정과 독 show/hide는 무관하다. 독에는 켤 애니메이션이 없다.

Composition 스프링은 독 확대나 제스처를 넣을 때에만 검토한다. 자동 숨김에는 쓰지 않는다.

## 11. 빌드, 아키텍처, 배포

- 대상 OS: Windows 11 버전 22H2 (빌드 22621) 이상
- CPU: x64 필수, ARM64 네이티브 필수 (상주 프로세스를 x64 에뮬레이션으로 돌리지 않는다)
- CRT: 정적 링크를 기본으로 검토해 VC++ 재배포 의존을 줄인다
- 패키징: unpackaged MSI/WiX. 시작 시 Run 키 또는 시작 폴더 등록은 설정
- 업데이트: 1.0은 수동 또는 단순 인스톨러. 셸 훅이 없으므로 월별 누적 업데이트에 바이너리를 맞출 필요가 없다
- 로깅: `%LOCALAPPDATA%\bamti\logs`. 기본적으로 경고 이상만

Enterprise GPO가 태스크바 잠금을 켜면 숨기기가 실패할 수 있다. 이 경우 메뉴 바만 동작하고, 태스크바와 겹침을 설정에 분명히 남긴다.

## 12. 구현 순서

스택과 호환 원칙을 검증하는 순서다. 기능을 많이 넣기 전에 상주 경로를 고정한다.

1. unpackaged Win32 프로세스, 싱글 인스턴스, DPI 인식
2. 상단 AppBar + DWM Mica + DirectWrite 시계
3. 모니터 변경·DPI 변경·AppBar 재배치
4. Status Item named pipe + 상단바 텍스트 갱신
5. 기본 태스크바 숨김/복구, 실패 경로
6. 독 HWND, 핀·실행 중 아이콘, 즉시 자동 숨김 (애니메이션 없음)
7. 전체화면 시 즉시 숨김
8. 설정 창
9. 트레이 미러(읽기 전용)
10. (후순위) `ITaskbarList3` 배지, 독 작업 영역 예약 모드

## 13. 리스크

- **트레이 미러(UIA)** 는 explorer 알림 영역을 읽기만 한다. 아이콘 픽셀과 우클릭 전달이 없어 글리프와 “알림 영역 잠시 표시”로 폴백한다.
- **트레이 가로채기**는 기본값 꺼짐인 실험 기능이다. 받은 `WM_COPYDATA`를 explorer로 넘기지 못하면 그 앱의 아이콘이 explorer에서 사라진다. 관리자 앱은 UIPI로 오지 않고, `TaskbarCreated`에 응답하지 않는 앱은 미러에 안 나올 수 있다.
- **태스크바 숨김** API·정책은 버전·GPO에 따라 실패한다. 숨김은 최선의 노력이다.
- **가상 데스크톱** COM은 빌드마다 바뀐 이력이 있다. 실패 시 “모든 데스크톱의 창을 보여 주기”로 폴백한다.
- **동일 가장자리의 다른 AppBar** (다른 상태 표시줄)와 자리를 놓고 싸울 수 있다. `ABM_QUERYPOS` 결과를 따른다.
- Status Item을 쓰지 않는 기존 사용량 앱은 상단바에 숫자가 나타나지 않는다. 이는 프로토콜 한계이지 렌더러 버그가 아니다.
