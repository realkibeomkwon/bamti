# 작업 지시서: 트레이 미러

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`PLAN-TRAY-TO-TOPBAR.md`의 5단계를 구현하는 지시서입니다. 계획 문서와 이 지시서가 어긋나면 **계획 문서가 우선**입니다. 다만 1단계 탐침이 끝난 뒤에 확정된 사항은 이 지시서가 계획 문서보다 최신이므로, 아래에서 "탐침 이후 확정"이라고 표시한 항목은 이 지시서를 따르십시오.

이 단계는 목표 조건 2를 만족시키는 부분입니다. 원칙은 하나입니다. **explorer의 알림 영역에 쓰기를 하지 않습니다.** 읽기와 클릭 위임만 합니다.

---

## 0. 완료 조건

- 알림 영역에 있는 서드파티 아이콘이 상단바에 아이콘 그림 그대로 나타납니다.
- 미러 아이콘을 좌클릭하면 원래 앱의 기본 동작이 실행됩니다.
- 미러 아이콘을 우클릭하면 bamti 자체 메뉴가 열리고, 그 안의 항목으로 원래 앱의 컨텍스트 메뉴에 도달할 수 있습니다.
- 트레이 앱을 종료하면 2초 안에 상단바에서 사라지고, 새로 실행하면 2초 안에 나타납니다.
- explorer를 강제로 재시작해도 미러가 스스로 복구됩니다.
- 태스크바를 다시 표시했을 때 explorer의 알림 영역에 아이콘이 그대로 남아 있습니다. bamti가 아무것도 가져가지 않았음을 확인합니다.
- 미러를 켠 채로 10분 동안 유휴 상태를 유지했을 때 bamti.exe의 CPU 사용률이 0.2% 미만입니다.
- explorer가 응답하지 않아도 상단바가 멈추지 않습니다.
- `/W4` 경고 없이 Debug와 Release가 모두 빌드됩니다.

---

## 1. 탐침 결과가 확정한 전제

`PROBE-TRAY.md`의 측정 결과로 다음이 확정되었습니다. 이 전제를 다시 검토하지 말고 그대로 받아들이십시오.

1. `ToolbarWindow32`는 이 컴퓨터에 존재하지 않고 검증 통과 버튼은 0개입니다. **`tray_backend_toolbar.cpp`를 만들지 마십시오.** 계획 문서 5-2절은 구현 대상이 아닙니다.
2. UI Automation의 ControlView는 알림 영역 버튼을 열거합니다. 아이콘은 `Shell_TrayWnd`의 직계 자식인 `Windows.UI.Composition.DesktopWindowContentBridge` 창에 붙은 트리에서 보입니다. `Shell_TrayWnd` 자체에 붙은 트리에는 나오지 않습니다.
3. 알림 영역 아이콘은 `ControlType=Button`이고 `AutomationId`가 `NotifyItemIcon` 또는 `SystemTrayIcon`이며 `ClassName`이 `SystemTray.`로 시작합니다. `Invoke` 패턴을 가지고 있습니다.
4. bamti가 상주해 태스크바를 주차한 상태에서도 UIA 열거는 그대로 동작하고, `BoundingRectangle`의 y 좌표가 주차 위치(32000대)로 따라옵니다.
5. `PrintWindow`는 태스크바가 보일 때 정상적으로 픽셀을 돌려주지만, 현재의 `SW_HIDE` 주차에서는 전부 검게 나옵니다. 따라서 아이콘 픽셀을 얻으려면 계획 문서 5-7절의 `kParkedVisible` 주차가 필요합니다.
6. 오버플로 창(`TopLevelWindowForOverflowXamlIsland`)은 닫혀 있는 동안 UIA 자식이 비어 있습니다. **숨겨진 아이콘은 열거할 수 없습니다.** 이 지시서는 오버플로 안의 아이콘을 미러하지 않습니다.

이 6번 전제는 2026-08-28 `PROBE-TRAY.md` 측정에 근거한 것이었고, `PROBE-TRAY-OVERFLOW.md`(2026-08-29)가 뒤집었습니다. 닫힌 `TopLevelWindowForOverflowXamlIsland`의 자식 `DesktopWindowContentBridge`에서 `NotifyItemIcon`이 이름과 `RuntimeId`와 함께 열거됩니다. `FIX-TRAY-REACT.md`가 그 전제를 대체합니다.

UIA 백엔드만 남았으므로 계획 문서가 예고한 두 가지 기능 저하가 실제로 발생합니다. 첫째로 아이콘 충실도가 화면 캡처 품질에 종속됩니다. 둘째로 우클릭 컨텍스트 메뉴를 직접 띄우지 못합니다. 이 사실을 숨기지 말고 7-2절과 9-2절이 정한 방식으로 사용자에게 알립니다.

---

## 2. 게이트 작업: 주차 상태 캡처 측정

**이 절을 먼저 끝내고, 그 결과를 문서에 적은 뒤에 3절로 넘어가십시오.** 여기서 무엇이 되고 무엇이 안 되는지 확정하지 않으면 4절의 아이콘 경로를 헛으로 만들게 됩니다.

`SW_HIDE` 없이 화면 밖으로만 옮긴 상태에서 `PrintWindow`가 XAML 알림 영역을 실제로 그려 주는지는 아직 아무도 측정하지 않았습니다. 탐침이 시스템 상태를 바꾸지 않는다는 원칙 때문에 측정하지 못한 항목입니다.

### 2-1. 측정 방법

`src/tray_probe.cpp`에 `--probe-tray-parked` 모드를 추가합니다. 기존 `--probe-tray`는 건드리지 마십시오. 새 모드는 다음 순서로 동작합니다.

1. 현재 태스크바 상태를 기록합니다.
2. `Shell_TrayWnd`와 보조 태스크바를 `ShowWindow`를 부르지 않고 `SetWindowPos`로만 `y=32000`으로 옮깁니다. 즉 `kParkedVisible` 주차를 흉내 냅니다.
3. 3초 기다립니다. XAML이 새 위치에서 한 번 그릴 시간을 줍니다.
4. UIA로 알림 영역 버튼을 열거해 각 버튼의 `BoundingRectangle`을 기록합니다.
5. `Windows.UI.Composition.DesktopWindowContentBridge`와 `TrayNotifyWnd` 두 창을 각각 `PrintWindow(PW_RENDERFULLCONTENT)`로 캡처하고, PNG를 `%USERPROFILE%\.bamti\probe-parked-bridge.png`와 `probe-parked-notify.png`로 저장합니다.
6. 각 캡처에 대해 4번에서 얻은 아이콘 사각형을 창 좌표로 변환하고, **사각형 안쪽 픽셀의 채널별 표준편차**를 계산해 보고서에 적습니다.
7. 태스크바를 원래 상태로 되돌립니다. 되돌리기는 실패하더라도 반드시 시도해야 하며, `--restore-taskbar` 경로와 같은 방식을 씁니다.

### 2-2. 판정 기준

아이콘이 실제로 캡처되었는지는 사람의 눈이 아니라 다음 기준으로 판정합니다.

| 조건 | 판정 |
|---|---|
| 아이콘 사각형 안쪽 표준편차가 8 이상인 사각형이 절반을 넘는다 | 캡처 경로를 씁니다. 표준편차가 더 큰 쪽 창을 캡처 대상으로 확정합니다. |
| 그렇지 않다 | 캡처 경로를 포기하고 4-5절의 글리프 폴백만 구현합니다. |

배경만 찍힌 사각형은 표준편차가 1 미만으로 나옵니다. 아이콘이 있으면 대체로 20을 넘습니다. 경계에 걸리면 PNG를 사용자에게 보여 주고 판단을 요청하십시오.

### 2-3. 결과 기록

측정 결과를 저장소 루트의 `PROBE-TRAY-PARKED.md`에 적습니다. 형식은 `PROBE-TRAY.md`를 따릅니다. 최소한 다음을 담습니다.

- 두 창 각각의 `PrintWindow` 성공 여부, 크기, 비검정 비율.
- 아이콘 사각형별 표준편차 표.
- 2-2절 표를 적용한 결과와 확정한 캡처 대상 창.
- 주차와 복귀 과정에서 태스크바가 화면 가장자리에 잠깐이라도 보였는지 여부.

이 파일을 `docs:` 커밋으로 먼저 남긴 뒤에 3절로 넘어갑니다.

---

## 3. 파일 구성과 인터페이스

만들 파일은 셋입니다.

| 파일 | 역할 |
|---|---|
| `src/tray_backend.hpp` | 백엔드 인터페이스. 구현체가 하나뿐이어도 인터페이스는 둡니다. |
| `src/tray_backend_uia.cpp` | UI Automation 백엔드. 열거와 `Invoke`를 담당합니다. |
| `src/tray_mirror.hpp` / `.cpp` | `StatusSource` 구현체. 작업자 스레드, 변경 감지, 아이콘 픽셀, 항목 발행을 담당합니다. |

고칠 파일은 `src/taskbar_controller.hpp` / `.cpp`, `src/menu_bar.hpp` / `.cpp`, `src/settings.hpp` / `.cpp`, `src/tray_probe.cpp`, `CMakeLists.txt`, `bamti.vcxproj`, `docs/STATUS-PROTOCOL.md`, `ARCHITECTURE.md`입니다.

### 3-1. 백엔드 인터페이스

계획 문서 5-1절의 구조체에서 레거시 전용 필드를 덜어내고, UIA가 실제로 주는 값만 남깁니다. 이것이 탐침 이후 확정된 형태입니다.

```cpp
struct TrayIconInfo {
  uint64_t key = 0;            // 회차 사이에 안정적인 식별자. 3-3 참고
  std::wstring tip;            // UIA Name. 툴팁으로 쓴다
  std::wstring automation_id;  // "NotifyItemIcon" 또는 "SystemTrayIcon"
  std::wstring class_name;     // "SystemTray.NormalButton" 등
  RECT screen{};               // UIA BoundingRectangle. 주차 좌표를 그대로 담는다
  int order = 0;               // 화면 왼쪽부터 0
  bool system_icon = false;    // automation_id == "SystemTrayIcon"
  bool has_image_child = false;  // 5-1절의 오버플로 단추 판정에 쓴다
};

class TrayBackend {
 public:
  virtual ~TrayBackend() = default;
  virtual const char* Name() const = 0;
  virtual bool Probe() = 0;
  virtual bool Enumerate(std::vector<TrayIconInfo>* out) = 0;
  virtual bool Invoke(const TrayIconInfo& icon) = 0;
  virtual void Reset() = 0;   // explorer 재시작 때 캐시한 핸들을 버린다
};
```

`Invoke`에 `bool right`와 `POINT screen` 인자를 두지 않습니다. UIA에는 우클릭에 대응하는 패턴이 없고 좌표도 쓰지 않으므로, 쓰지 않는 인자를 인터페이스에 남기면 나중에 누군가 그것이 동작한다고 오해합니다.

`Enumerate`와 `Invoke`는 **작업자 스레드에서만** 호출됩니다. UI 스레드에서 부르면 explorer가 멈출 때 상단바가 함께 멈춥니다. 이 규칙에는 예외가 없습니다.

`TrayMirror::Start()`가 백엔드를 `Probe()` 하고 성공한 것을 씁니다. 구현체가 하나뿐이므로 순회 자체는 형식적이지만, 어떤 백엔드를 골랐는지는 반드시 로그에 남깁니다.

### 3-2. UIA 백엔드의 열거

성능이 이 절에 달려 있습니다. 탐침은 노드마다 프로퍼티를 하나씩 읽어서 트리 하나를 순회하는 데 390ms를 썼습니다. 그 방식을 그대로 옮기면 예산을 백 배 넘깁니다. 다음을 지킵니다.

1. COM 아파트먼트는 작업자 스레드에서 `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`로 엽니다. UIA 클라이언트는 MTA를 권장합니다. UIA 인터페이스 포인터를 다른 스레드로 넘기지 마십시오.
2. 루트 HWND는 `FindWindowW(L"Shell_TrayWnd", nullptr)`로 얻은 창의 직계 자식 가운데 클래스가 `Windows.UI.Composition.DesktopWindowContentBridge`인 첫 번째 창입니다. `FindWindowExW`로 찾습니다. **이 HWND를 캐시하고 회차마다 다시 찾지 마십시오.** `IsWindow()`가 거짓이 되거나 `TaskbarCreated`를 받으면 그때 버리고 다시 찾습니다.
3. 조건은 `CreatePropertyCondition(UIA_AutomationIdPropertyId, ...)` 두 개를 `CreateOrCondition`으로 묶고, 다시 `CreatePropertyCondition(UIA_ControlTypePropertyId, UIA_ButtonControlTypeId)`와 `CreateAndCondition`으로 묶습니다.
4. 캐시 요청을 만듭니다. `AddProperty`로 `UIA_NamePropertyId`, `UIA_AutomationIdPropertyId`, `UIA_ClassNamePropertyId`, `UIA_BoundingRectanglePropertyId`, `UIA_IsOffscreenPropertyId`, `UIA_RuntimeIdPropertyId`를 담고, `AddPattern`으로 `UIA_InvokePatternId`를 담습니다. `TreeScope`는 `TreeScope_Descendants`입니다.
5. `IUIAutomationElement::FindAllBuildCache`를 한 번 호출해 배열을 받고, 이후에는 **`GetCached*` 계열만 씁니다.** `GetCurrent*`를 한 번이라도 부르면 그 자리에서 크로스 프로세스 왕복이 생깁니다.
6. 받은 요소를 `BoundingRectangle.left` 오름차순으로 정렬하고 `order`를 채웁니다.
7. 열거 1회에 걸린 시간을 밀리초로 재서 미러가 보관합니다. 이 값이 6-1절의 폴링 주기를 결정합니다.

`has_image_child`는 캐시 요청의 `TreeFilter`에 자식 `Image`를 포함시켜 채우거나, 그것이 번거로우면 캐시된 자식 배열이 비어 있는지로 판정합니다. 이 값 하나 때문에 회차마다 추가 왕복을 만들지는 마십시오. 얻기 어려우면 항상 참으로 두고 5-1절의 오버플로 판정을 위치 조건만으로 수행합니다.

`Invoke`는 열거 때 캐시한 요소를 다시 쓰지 말고, 호출 시점에 같은 조건으로 한 번 더 `FindAll`을 돌려 `key`가 일치하는 요소를 찾은 뒤 그 요소의 `InvokePattern`을 호출합니다. 캐시한 요소는 explorer가 XAML 노드를 재활용한 뒤 엉뚱한 아이콘을 가리킬 수 있습니다. 클릭은 초당 여러 번 일어나지 않으므로 이 왕복 비용은 문제가 되지 않습니다.

### 3-3. key 만들기

계획 문서는 `hash(AutomationId, Name)`을 제안했지만, 탐침 결과를 보면 이 조합은 안정적이지 않습니다. `AutomationId`는 모든 서드파티 아이콘이 `NotifyItemIcon`으로 같고, `Name`은 앱 상태에 따라 수시로 바뀌기 때문입니다. 툴팁이 바뀔 때마다 항목이 사라졌다가 새로 생기면 상단바가 깜빡입니다.

**탐침 이후 확정:** `key`는 다음 우선순위로 만듭니다.

1. `GetCachedPropertyValue(UIA_RuntimeIdPropertyId)`로 얻은 정수 배열 전체를 `Fnv1a64`로 해싱합니다. RuntimeId는 UIA가 요소 수명 동안 보장하는 식별자입니다.
2. RuntimeId를 얻지 못하면 `hash(automation_id, class_name, order)`로 대신합니다.

RuntimeId가 실제로 안정적인지는 구현하고 나서 측정해야 합니다. 미러가 시작한 뒤 다섯 회차 동안 key 집합이 그대로인지 검사해 `Log(L"tray", L"key stable=%d churn=%u")` 한 줄을 남기십시오. 회차마다 key가 바뀐다면 이 방식은 실패한 것이므로 2번 폴백으로 고정하고 그 사실을 보고하십시오.

---

## 4. 아이콘 픽셀

2-2절 판정이 캡처 경로를 허용했을 때만 4-1절부터 4-4절까지를 구현합니다. 허용하지 않았다면 4-5절만 구현합니다.

### 4-1. 캡처 시점

캡처는 비쌉니다. **주기적으로 캡처하지 마십시오.** 다음 두 경우에만 합니다.

- 열거 결과의 key 집합이 직전 회차와 달라졌을 때.
- 어떤 항목의 `Name`이 달라졌을 때. 아이콘 그림이 상태와 함께 바뀌는 앱을 따라가기 위한 조건입니다.

연속된 변경이 몰려 오면 합칩니다. 마지막 캡처로부터 500ms가 지나지 않았으면 다음 회차로 미룹니다.

### 4-2. 캡처 절차

탐침의 `CaptureThread` 방식을 그대로 옮깁니다. `PrintWindow`는 explorer의 응답을 기다리므로 **캡처 전용 스레드에 넣고 `WaitForSingleObject`로 300ms 시한을 겁니다.**

시한을 넘겼을 때의 처리가 탐침과 다릅니다. 탐침은 곧 종료하므로 DC와 DIB를 그냥 두었지만, 상주 프로세스는 그럴 수 없습니다. 다음과 같이 합니다.

1. 시한을 넘긴 작업의 스레드 핸들과 DC와 DIB와 비트 버퍼를 고아 목록에 넣습니다.
2. 다음 회차마다 고아 목록을 훑어 `WaitForSingleObject(thread, 0)`이 `WAIT_OBJECT_0`이면 그때 자원을 해제하고 목록에서 뺍니다.
3. 시한 초과가 연속 세 번 일어나면 캡처를 영구히 비활성화하고 4-5절의 글리프 폴백으로 내려갑니다. 로그에 한 줄 남깁니다.

캡처 대상 창은 2-3절에서 확정한 쪽입니다. 캡처한 뒤 창의 화면 좌표를 `GetWindowRect`로 얻어, 각 아이콘의 `screen` 사각형을 캡처 이미지의 상대 좌표로 변환합니다. 변환한 사각형이 이미지 밖으로 벗어나면 그 항목은 이번 회차에 아이콘 없이 발행합니다.

### 4-3. 아이콘 잘라내기와 배경 제거

잘라낸 셀에는 아이콘뿐 아니라 태스크바 배경이 함께 들어 있습니다. 상단바 배경은 태스크바 배경과 다르므로, 그대로 붙이면 아이콘마다 네모난 얼룩이 생깁니다. 배경이 셀 안에서 거의 균일하다는 점을 이용해 다음처럼 알파를 복원합니다.

1. 셀 테두리 2픽셀 링에서 채널별 중앙값을 구해 배경색 `B`로 삼습니다.
2. 각 픽셀 `P`에 대해 `d = max(|P.r-B.r|, |P.g-B.g|, |P.b-B.b|)`를 구합니다.
3. `a = clamp((d - kMatteLo) / (kMatteHi - kMatteLo), 0, 1)`로 알파를 만듭니다. `kMatteLo = 6`, `kMatteHi = 24`로 시작합니다.
4. `a > 0`인 픽셀의 색은 `C = (P - (1 - a) * B) / a`로 복원하고 0에서 255 사이로 클램프합니다. `a == 0`이면 완전 투명으로 둡니다.
5. 결과를 straight alpha PNG로 인코딩합니다. WIC 인코더는 이미 `icon_cache.cpp`가 쓰고 있으므로 그 경로를 재사용하십시오.

셀 전체가 아니라 셀 중앙의 정사각형만 씁니다. 알림 영역 셀은 세로가 아이콘보다 훨씬 길기 때문입니다. 중앙에서 셀 짧은 변의 60%에 해당하는 정사각형을 취하고, 그 결과를 24픽셀 정사각형으로 맞춥니다.

`kMatteLo`와 `kMatteHi`는 파일 상단의 상수로 빼 두십시오. 눈으로 보고 조정할 값입니다.

### 4-4. 상단바로 넘기기

`StatusIcon`을 `IconKind::kPng`로 채우고 `bytes`에 인코딩한 PNG를 담습니다. `cache_key`는 `HashStatusIcon(icon)`으로 채웁니다. `builtin.cpp`가 하는 것과 같습니다. 픽셀이 바뀌면 PNG 바이트가 바뀌므로 캐시 키가 저절로 달라지고, 바뀌지 않으면 `IconCache`가 디코드를 건너뜁니다.

PNG는 `kStatusIconPngMaxBytes`(8192)를 넘으면 안 됩니다. 24픽셀 정사각형이면 여유가 충분하지만, 인코딩 결과가 상한을 넘으면 그 항목은 아이콘 없이 발행하고 로그에 남깁니다.

### 4-5. 글리프 폴백

캡처가 없을 때는 `IconKind::kGlyph`로 툴팁의 첫 글자를 씁니다. 툴팁이 비어 있으면 `?`를 씁니다. 첫 글자는 서로게이트 쌍을 고려해 잘라야 합니다. 이 폴백은 미러가 쓸모없어지지 않게 하는 최소한의 장치이지 목표 품질이 아닙니다. 폴백으로 동작 중이라는 사실을 9-2절이 정한 방식으로 사용자에게 알립니다.

---

## 5. 항목 매핑

발행하는 `StatusItem`은 다음과 같습니다.

| 필드 | 값 |
|---|---|
| `id` | `bamti.tray/` 뒤에 `key`를 16진수 16자리로 붙입니다. |
| `source` | `tray` |
| `icon` | 4절이 만든 PNG 또는 글리프. |
| `text` | 비웁니다. 미러는 아이콘만 보여 줍니다. |
| `tooltip` | UIA `Name`을 그대로 씁니다. 여러 줄이어도 자르지 않고 툴팁 상한만 적용합니다. |
| `state` | 항상 `kNormal`입니다. 앱의 상태를 우리가 알 수 없으므로 추측하지 않습니다. |
| `priority` | `kTrayPriorityBase - order`. `kTrayPriorityBase = 5`로 시작합니다. |
| `visible` | `IsOffscreen`이 참이면 거짓으로 둡니다. |
| `panel` | 채우지 않습니다. 미러 항목에는 패널이 없습니다. |

`priority`를 항목마다 다르게 주는 이유는 `StatusRegistry::Snapshot`이 같은 우선순위 안에서 `id` 오름차순으로 정렬하기 때문입니다. `id`는 안정적이어야 하므로 순서를 담을 수 없고, 대신 `priority`가 화면 순서를 담습니다. 내장 위젯은 10 이상을 쓰므로 미러 항목은 항상 그 반대편에 모입니다. 실제로 띄웠을 때 미러가 내장 위젯의 오른쪽에 붙는다면 `kTrayPriorityBase`만 조정해 왼쪽으로 옮기십시오.

`bamti.`으로 시작하는 식별자는 이미 파이프 클라이언트에게 금지되어 있으므로 충돌 방지를 새로 만들 필요가 없습니다. `docs/STATUS-PROTOCOL.md`의 예약 표에 `bamti.tray/<key>` 한 줄만 더합니다.

### 5-1. 무엇을 미러하고 무엇을 빼는가

기본값은 `AutomationId == "NotifyItemIcon"`인 항목만 미러합니다. `SystemTrayIcon`은 설정으로 켤 때만 미러합니다. 이유는 배터리와 네트워크가 4단계 내장 위젯과 겹치기 때문입니다. 같은 정보를 두 번 보여 주는 것이 기본값이어서는 안 됩니다.

`SystemTrayIcon`을 켜더라도 다음 셋은 언제나 제외합니다.

| 제외 대상 | 판정 | 이유 |
|---|---|---|
| 시계 | `ClassName == "SystemTray.OmniButton"` | bamti가 자체 시계를 그립니다. `Name`이 매초 바뀌어 변경 감지를 계속 깨웁니다. |
| 바탕 화면 보기 | `ClassName == "SystemTray.ShowDesktopButton"` | 상단바에서 누를 이유가 없습니다. |
| 숨겨진 아이콘 표시 | 아래 문단 참고 | 눌러도 explorer가 오버플로 팝업을 주차된 태스크바 근처, 즉 화면 밖에 띄웁니다. |

세 번째 항목의 판정에는 주의가 필요합니다. 이 컴퓨터의 탐침 결과에서 오버플로 단추의 `ClassName`은 `SystemTray.NormalButton`으로 나왔으므로 클래스 이름만으로는 가릴 수 없습니다. **열거 결과에서 가장 왼쪽에 있으면서 `AutomationId == "SystemTrayIcon"`이고 자식 `Image`가 없는 버튼**을 오버플로 단추로 보고 제외하십시오. 이 판정이 빗나가면 미러에 쓸모없는 아이콘 하나가 남을 뿐이므로, 정확도보다 안전이 우선입니다.

오버플로 안의 아이콘은 `TopLevelWindowForOverflowXamlIsland`의 자식 브리지를 두 번째 열거 루트로 써서 미러합니다. 좌표가 전부 원점이므로 열거 순서를 유지하고, `IsOffscreen` 대신 `from_overflow`로 그립니다. 설정 `tray_overflow_icons`(기본 켜짐)으로 끌 수 있습니다.

숨김 목록에 들어 있는 `key`는 열거 단계에서 걸러 냅니다. 목록은 9-1절의 설정에 있습니다.

---

## 6. 폴링과 변경 감지

`BuiltinWidgets`의 작업자 스레드 구조를 그대로 따릅니다. 정지 이벤트와 깨우기 이벤트를 두고 `WaitForMultipleObjects`로 기다립니다. 새 패턴을 만들지 마십시오.

미러가 꺼져 있으면 **작업자 스레드를 아예 만들지 않습니다.** 설정으로 켜는 순간에 만들고, 끄는 순간에 정리합니다. 4단계에서 정한 규칙과 같습니다.

### 6-1. 주기

계획 문서는 1초 주기와 열거 1회 1ms 미만을 요구했습니다. 이 예산은 레거시 백엔드를 전제한 값이며 UIA로는 달성할 수 없습니다. **탐침 이후 확정:** 주기를 실측에 연동합니다.

```
interval_ms = clamp(last_enum_ms * 100, 1000, 5000)
```

열거가 10ms 걸리면 1초마다, 50ms 걸리면 5초마다 돕니다. 어느 경우에도 열거에 쓰는 시간이 전체의 1%를 넘지 않습니다. 실측값과 그때 정한 주기를 5분에 한 번 로그로 남기십시오.

열거가 200ms를 넘는 회차가 연속 세 번 나오면 미러를 자동으로 멈추고 로그에 남깁니다. 그런 환경에서는 미러를 켜 두는 것이 이득보다 손해입니다.

### 6-2. 변경 감지

`(key, tip, order, visible)` 네 값의 튜플을 회차마다 해싱해 직전 값과 비교합니다. 같으면 **아무것도 하지 않습니다.** `Upsert`도 부르지 않고 캡처도 하지 않습니다.

달라졌으면 다음을 합니다.

1. 사라진 key에 대해 `Remove`를 부릅니다.
2. 4-1절 조건에 해당하면 캡처를 한 번 합니다.
3. 새로 생겼거나 값이 바뀐 항목에 대해 `Upsert`를 부릅니다. 아이콘 픽셀이 그대로인 항목은 이전 PNG를 재사용해 인코딩을 건너뜁니다.

### 6-3. 깨우는 신호

- `RegisterWindowMessageW(L"TaskbarCreated")` 브로드캐스트를 `MenuBar`가 받으면 미러에게 알립니다. 미러는 백엔드의 `Reset()`을 부르고 캐시한 HWND를 버린 뒤 즉시 한 회차를 돕니다.
- `SetActive(false)`를 받으면 폴링을 멈춥니다. 전체화면 가림, 세션 잠금, 화면 꺼짐에서 `MenuBar::UpdateProviderActive`가 이미 이 신호를 보내고 있으므로 새로 배선할 것은 없습니다.
- `SetActive(true)`로 돌아오면 즉시 한 회차를 돕니다. 멈춘 동안 아이콘이 늘거나 줄었을 수 있습니다.

### 6-4. Stop

`Stop()`은 정지 이벤트를 올리고 스레드를 `join`합니다. 고아 목록에 남은 캡처 스레드는 최대 1초까지 기다렸다가, 그래도 끝나지 않으면 핸들만 닫고 자원은 프로세스 종료에 맡깁니다. 이 경우에만 누수를 허용하며 로그에 남깁니다.

---

## 7. 클릭 전달

### 7-1. 좌클릭

`MenuBar`가 이미 `StatusEvent{event:"click", button:"left"}`를 소스로 보내고 있습니다. `TrayMirror::OnEvent`가 이것을 받아 **작업자 스레드에 일감으로 넣습니다.** UI 스레드에서 곧바로 `Invoke`를 부르면 explorer가 느릴 때 상단바가 멈춥니다.

작업자 스레드는 3-2절이 정한 대로 요소를 다시 찾아 `IUIAutomationInvokePattern::Invoke`를 부릅니다. 실패하면 `IUIAutomationLegacyIAccessiblePattern::DoDefaultAction`을 한 번 더 시도합니다. 탐침에서 모든 요소가 `LegacyIAccessible` 패턴을 가지고 있음을 확인했습니다. 둘 다 실패하면 로그만 남기고 조용히 넘어갑니다.

**클릭을 전달하기 전에 열려 있는 `PopupSurface`를 닫으십시오.** 우리 팝업이 마우스를 캡처한 상태에서 다른 프로세스가 메뉴를 띄우면 두 모달이 겹칩니다.

미러 항목에는 패널이 없으므로 `MenuBar::OpenStatusPanel`은 아무 일도 하지 않고 돌아옵니다. 이 경로를 새로 막을 필요는 없습니다.

### 7-2. 우클릭

UIA에는 대응하는 패턴이 없습니다. 계획 문서 5-4절의 메시지 직접 전달은 `owner`와 `callback_msg`를 요구하는데 UIA는 둘 다 주지 않으므로 쓸 수 없습니다. 마우스 입력을 합성하는 방법도 쓰지 않습니다. 사용자의 화면에 우리가 만든 입력을 흘려보내는 것은 이 프로젝트가 하지 않기로 한 일입니다.

대신 미러 아이콘을 우클릭하면 bamti 자체 메뉴를 띄웁니다. 항목은 셋입니다.

| 항목 | 동작 |
|---|---|
| 알림 영역 잠시 표시 | 8-2절의 임시 복귀를 시작합니다. |
| 이 아이콘 숨기기 | 이 항목의 `key`를 설정의 숨김 목록에 넣고 즉시 `Remove`합니다. |
| 트레이 미러 끄기 | 설정의 `tray_mirror`를 끕니다. |

첫 항목이 우클릭 메뉴에 도달하는 실질적인 경로입니다. 이것이 계획 문서 5-5절의 축소 대안을 우클릭 자리로 옮겨 놓은 형태입니다. 계획 문서는 갈매기 아이콘을 따로 두라고 했지만, 아이콘을 하나 더 두는 것보다 미러 항목의 우클릭에 붙이는 편이 사용자가 찾기 쉽습니다.

이 메뉴는 `MenuBar::ShowContextMenu`가 쓰는 방식을 그대로 씁니다. 새 팝업 구조를 만들지 마십시오.

---

## 8. TaskbarController 확장

### 8-1. 주차 모드 둘로 나누기

계획 문서 5-7절대로 주차 방식을 둘로 나눕니다.

```cpp
enum class HideMode { kHidden, kParkedVisible };
bool Hide(HideMode mode);
```

- `kHidden`은 지금 동작과 완전히 같습니다. `ShowWindow(SW_HIDE)` 뒤에 `y=32000`으로 옮깁니다.
- `kParkedVisible`은 `ShowWindow`를 부르지 않고 `SetWindowPos`로만 옮깁니다.

자동 숨김 설정과 작업 영역 반환은 두 모드에서 동일합니다. 현재 모드를 멤버로 보관하고 `EnsureHidden`과 `Rehide`가 그 모드를 따르게 합니다.

**`Rehide`를 반드시 함께 고치십시오.** 지금 구현은 `IsWindowVisible(hwnd)`가 참이면 재숨김이 필요하다고 판단합니다. `kParkedVisible`에서는 창이 계속 보이는 상태이므로, 고치지 않으면 재숨김이 쉬지 않고 헛돌면서 태스크바를 계속 흔듭니다. 이것은 실제로 발생할 결함이지 이론적인 위험이 아닙니다. `kParkedVisible`에서는 위치만 검사하십시오.

```cpp
// kHidden:        보이거나 제자리로 돌아왔으면 다시 숨긴다
// kParkedVisible: 제자리로 돌아왔을 때만 다시 옮긴다
const bool need = (mode_ == HideMode::kHidden && IsWindowVisible(hwnd)) || rc.top < kParkY / 2;
```

모드를 고르는 주체는 `MenuBar`입니다. 미러가 켜져 있고 캡처 경로가 살아 있으면 `kParkedVisible`을, 그 밖에는 `kHidden`을 씁니다. 설정을 바꿔 모드가 달라지면 `Restore()` 뒤에 새 모드로 `Hide()`를 다시 부릅니다. **전환하는 동안 태스크바가 화면 가장자리에 잠깐이라도 보이면 안 됩니다.** 위치를 먼저 옮기고 나서 `ShowWindow` 상태를 바꾸는 순서를 지키십시오.

`kParkedVisible`이 기존 동작보다 위험한 지점이 하나 있습니다. 보이는 창이므로 다른 코드가 `Z` 순서나 창 목록을 훑을 때 태스크바가 후보에 들어옵니다. `fullscreen.cpp`의 전체화면 판정과 `dock.cpp`의 트레이 재숨김 검사와 `task_list.cpp`의 창 수집이 이 창을 어떻게 다루는지 확인하고, 화면 밖 창을 무시하도록 되어 있지 않으면 무시하게 고치십시오. 이 확인을 건너뛰면 독에 태스크바가 항목으로 나타나거나 전체화면 판정이 뒤집힐 수 있습니다.

### 8-2. 임시 복귀

7-2절의 "알림 영역 잠시 표시"는 다음처럼 동작합니다.

1. `Restore()`를 불러 태스크바를 원래 자리로 되돌립니다.
2. 10초 타이머를 겁니다.
3. 타이머가 만료되면 현재 모드로 `Hide()`를 다시 부릅니다.
4. 이미 임시 복귀 중에 다시 요청이 오면 타이머만 새로 시작합니다.

사용자가 그 10초 안에 알림 영역에서 우클릭을 하면 앱의 컨텍스트 메뉴가 뜹니다. 메뉴가 떠 있는 동안 태스크바가 다시 숨어도 메뉴는 그대로 남으므로 조작을 마칠 수 있습니다. 마우스 위치를 추적하는 복잡한 판정은 넣지 마십시오. 시간만으로 충분합니다.

임시 복귀 중에는 미러 폴링을 멈추십시오. 태스크바가 제자리에 있는 동안 열거한 좌표는 다시 주차하는 순간 전부 무효가 됩니다.

---

## 9. 설정과 고지

### 9-1. 설정 항목

`WidgetSettings`에 필드를 더합니다. 파일 형식은 `topbar.widgets` 아래에 그대로 붙입니다. 새 최상위 키를 만들면 `json::ParsePreserve`의 보존 규칙까지 함께 손봐야 하므로, 이번에는 기존 객체를 씁니다.

```json
{
  "topbar": {
    "widgets": {
      "battery": false, "cpu": false, "network": false, "widget_board": false,
      "tray_mirror": true, "tray_system_icons": false,
      "tray_hidden_keys": ["0x1a2b3c4d5e6f7788"]
    }
  }
}
```

| 키 | 기본값 | 뜻 |
|---|---|---|
| `tray_mirror` | `true` | 미러 전체를 켜고 끕니다. |
| `tray_system_icons` | `false` | `SystemTrayIcon` 항목까지 미러합니다. |
| `tray_hidden_keys` | 빈 배열 | 사용자가 숨긴 항목의 `key` 목록입니다. |

`WidgetSettings::Any()`에 트레이 필드를 넣지 마십시오. 그 함수는 위젯 작업자 스레드를 만들지 결정하는 값이고, 미러는 자기 스레드를 따로 가집니다.

`tray_hidden_keys`는 문자열 배열이므로 `json_line.hpp`에 배열 읽기와 쓰기가 없으면 그것부터 더해야 합니다. 목록 상한은 64개로 두고 넘으면 앞에서부터 버립니다.

상단바 빈 곳 우클릭 메뉴에 "트레이 미러"와 "시스템 아이콘도 표시"와 "숨긴 아이콘도 표시" 항목을 더합니다. 4단계에서 위젯 토글을 붙인 자리와 같은 방식입니다.

### 9-2. 기능 저하 고지

다음 두 가지를 사용자가 볼 수 있는 자리에 적습니다.

1. 우클릭 컨텍스트 메뉴를 앱에 직접 전달하지 못하며, 대신 "알림 영역 잠시 표시"를 거쳐야 한다는 사실.
2. 오버플로에 숨긴 아이콘도 미러합니다. 닫힌 상태 `Invoke`는 `HRESULT 0x00000000`을 돌려줬고 `DoDefaultAction`은 부르지 않았습니다. 전면 창이 바로 보이지는 않았으므로, 반응이 없으면 "알림 영역 잠시 표시"로 여십시오.

적는 자리는 셋입니다. 첫째로 미러 항목의 우클릭 메뉴 맨 아래에 회색 안내 한 줄을 둡니다. 둘째로 `ARCHITECTURE.md`의 트레이 미러 절에 적습니다. 셋째로 미러가 시작할 때 로그에 백엔드 이름과 함께 한 줄로 남깁니다. 7단계에서 설정 화면이 생기면 그때 옮깁니다.

글리프 폴백으로 동작 중이라면 그 사실도 같은 자리에 더합니다.

---

## 10. 로그

미러는 다음을 남깁니다. 남기는 양이 이보다 많으면 유휴 상태에서 로그 파일이 계속 자랍니다.

| 시점 | 내용 |
|---|---|
| 시작 | 고른 백엔드 이름, 캡처 경로 사용 여부, 주차 모드. |
| 5분마다 한 번 | 마지막 열거 소요 시간, 현재 주기, 항목 수. |
| 항목이 늘거나 줄 때 | 늘어난 수와 줄어든 수. 개별 항목의 툴팁은 적지 않습니다. |
| 캡처 시한 초과 | 연속 횟수. |
| 클릭 전달 실패 | 실패한 패턴 이름과 `HRESULT`. |
| 자동 정지 | 6-1절의 200ms 조건으로 멈췄을 때. |

2026-08-31: 가로채기 선점 작업에서 진단 목적으로 이 결정을 뒤집었습니다. 가로채기와 UIA는 항목이 목록에 처음 들어올 때 툴팁을 키당 프로세스 수명에 한 번 남깁니다. 진단 창이 닫힐 때 로스터 한 줄씩을 남깁니다. `TASK-TRAY-PREEMPT.md` 8절, `FIX-TRAY-PREEMPT-DIAG.md`.

변경이 없는 회차에는 **아무것도 남기지 마십시오.**

---

## 11. 성능 예산

| 항목 | 예산 | 확인 방법 |
|---|---|---|
| 열거 1회 | 50ms 이하 | 로그의 실측값. |
| 열거가 쓰는 CPU 비율 | 전체의 1% 이하 | 6-1절 수식이 구조적으로 보장합니다. |
| 캡처 1회 | 300ms 이하 | 시한이 강제합니다. |
| 유휴 CPU | 0.2% 미만 | 10분 측정. |
| 상주 작업 집합 증가 | 미러를 켜고 끈 차이가 8MB 이하 | `WorkingSetPrivate` 비교. |

마지막 항목은 절대값이 아니라 차이를 봅니다. bamti의 상주 작업 집합이 이미 예산을 넘고 있다는 사실은 4단계에서 확인되었고, 그 원인 규명은 이 단계의 범위가 아닙니다. 여기서는 미러가 그 문제를 더 키우지 않는지만 봅니다.

---

## 12. 하지 말아야 할 것

- `Shell_TrayWnd` 클래스로 창을 만들어 `WM_COPYDATA`를 가로채지 마십시오. 계획 문서 5-6절의 방식은 이번 범위가 아닙니다.
- explorer의 알림 영역에 어떤 쓰기 연산도 하지 마십시오. 창을 옮기는 것은 `TaskbarController`만 합니다.
- UI 스레드에서 UIA를 부르지 마십시오.
- `SendMessageW`를 explorer 창에 보내지 마십시오. 이 지시서에는 그럴 일이 없지만, 편의를 위해 추가하고 싶어지면 `SendMessageTimeoutW`와 `SMTO_ABORTIFHUNG`을 쓰십시오.
- 마우스나 키보드 입력을 합성하지 마십시오.
- 아이콘 픽셀을 매 회차 캡처하지 마십시오.
- `tray_backend_toolbar.cpp`를 만들지 마십시오.

---

## 13. 검증

### 기능

- [ ] 트레이 앱을 5개 이상 실행한 상태에서 서드파티 아이콘이 전부 상단바에 나타납니다.
- [ ] 아이콘 그림이 원래 모양과 같고, 아이콘 주변에 네모난 배경 얼룩이 없습니다.
- [ ] 아이콘 좌클릭이 원래 앱의 동작을 실행합니다. 창을 복원하는 앱과 팝업을 여는 앱을 각각 하나씩 확인합니다.
- [ ] 아이콘 우클릭이 bamti 메뉴를 띄우고, "알림 영역 잠시 표시"를 고르면 태스크바가 돌아오며, 10초 뒤에 스스로 다시 숨습니다.
- [ ] "이 아이콘 숨기기"를 고르면 즉시 사라지고, 다시 실행해도 숨어 있습니다.
- [ ] 트레이 앱을 종료하면 2초 안에 상단바에서 사라집니다.
- [ ] `tray_system_icons`를 켜면 입력 표시기와 볼륨이 나타나고, 시계와 바탕 화면 보기와 오버플로 단추는 나타나지 않습니다.
- [ ] `tray_mirror`를 끄면 미러 항목이 전부 사라지고 스레드 수가 하나 줄어듭니다.

### 안정성

- [ ] explorer를 강제로 종료하고 다시 시작해도 미러가 스스로 복구됩니다.
- [ ] 태스크바를 다시 표시하면 explorer의 알림 영역에 아이콘이 그대로 있습니다.
- [ ] `kParkedVisible`로 주차하는 동안 태스크바가 화면 어디에도 보이지 않습니다.
- [ ] 주차 모드를 오가며 전환해도 화면 가장자리에 태스크바가 번쩍이지 않습니다.
- [ ] 독의 창 목록에 태스크바가 항목으로 나타나지 않습니다.
- [ ] 전체화면 게임에서 상단바 숨김 판정이 그대로 동작합니다.
- [ ] explorer를 디버거로 멈춘 상태에서 상단바가 계속 그려지고 클릭에 반응합니다.
- [ ] 미러를 100번 켜고 끈 뒤 스레드 수와 핸들 수와 GDI 객체 수가 늘지 않습니다.

### 절전

- [ ] 미러를 켠 채 10분 유휴 상태에서 CPU 사용률이 0.2% 미만입니다. 측정은 4단계와 같습니다.

  ```powershell
  $p = Get-Process bamti; $a = $p.TotalProcessorTime
  Start-Sleep -Seconds 600
  $p.Refresh(); $b = $p.TotalProcessorTime
  ($b - $a).TotalSeconds / 600 / [Environment]::ProcessorCount * 100
  ```

- [ ] `Win+L`로 잠근 동안 열거가 멈추고, 풀면 한 회차가 즉시 돕니다. 로그로 확인합니다.
- [ ] 절전에서 복귀했을 때 미러가 정상으로 돌아옵니다.

### 회귀

- [ ] 내장 위젯 네 개가 그대로 동작합니다.
- [ ] `examples/ticker.py`와 `examples/status_push.py`가 그대로 동작합니다.
- [ ] 항목이 많아지면 미러 아이콘도 오버플로 팝업에 정상적으로 접힙니다.
- [ ] 다크와 라이트, 100%와 150%와 200% DPI에서 아이콘 크기와 정렬이 깨지지 않습니다.

---

## 14. 커밋

| 순서 | 작업 | 커밋 메시지 |
|---|---|---|
| 1 | `--probe-tray-parked` 모드와 측정 | `feat: 주차 상태 캡처를 측정하는 탐침 모드를 추가한다` |
| 2 | 측정 결과 기록 | `docs: 주차 상태 캡처 측정 결과를 남긴다` |
| 3 | `HideMode`와 `Rehide` 수정, 다른 모듈의 주차 창 처리 확인 | `feat: 태스크바를 보이는 채로 주차하는 모드를 추가한다` |
| 4 | 백엔드 인터페이스와 UIA 열거, 미러 뼈대, 글리프 폴백 | `feat: 알림 영역 아이콘을 상단바에 미러한다` |
| 5 | 캡처와 배경 제거, PNG 발행 | `feat: 미러 아이콘에 실제 트레이 아이콘 그림을 쓴다` |
| 6 | 좌클릭 위임과 우클릭 메뉴, 임시 복귀 | `feat: 미러 아이콘의 클릭을 원래 앱에 전달한다` |
| 7 | 설정 항목과 메뉴 토글, 숨김 목록 | `feat: 트레이 미러 표시 범위를 설정에서 고른다` |
| 8 | 명세와 구조 문서 | `docs: 트레이 미러의 제약과 식별자를 문서에 적는다` |

4번 커밋을 마친 시점에 빌드하고 실제로 띄워서 글리프만으로도 항목이 올바른 개수와 순서로 나오는지 눈으로 확인한 뒤 5번으로 넘어가십시오. 이 확인 없이 캡처까지 한꺼번에 넣으면, 아이콘이 보이지 않을 때 원인이 열거인지 캡처인지 가릴 수 없습니다.

2번 커밋의 측정 결과가 캡처 경로를 허용하지 않았다면 3번과 5번을 건너뛰고 글리프 폴백으로 마무리한 뒤, 그 사실을 보고하십시오.
