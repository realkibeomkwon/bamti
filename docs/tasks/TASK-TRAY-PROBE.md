# 작업 지시서: 알림 영역 탐침 도구

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`PLAN-TRAY-TO-TOPBAR.md`의 1단계를 구현하는 지시서입니다. 이 작업의 목적은 기능 추가가 아니라 **측정**입니다. 트레이 미러의 백엔드를 무엇으로 만들지가 이 도구의 출력으로 결정되며, 계획 문서 5-0절의 판정표가 그 출력을 입력으로 받습니다.

따라서 다음 두 가지를 지켜 주십시오. 첫째로 이 도구는 시스템 상태를 바꾸지 않습니다. 읽기만 합니다. 둘째로 결과를 해석해서 요약하지 말고 **측정한 원본 값을 그대로 남깁니다.** 판정은 사람이 합니다.

---

## 0. 완료 조건

- `bamti.exe --probe-tray`가 실행되고 30초 안에 끝납니다.
- `%USERPROFILE%\.bamti\probe-tray.txt`에 보고서가 생성됩니다.
- `%USERPROFILE%\.bamti\probe-tray.png`에 알림 영역 캡처가 생성됩니다.
- 실행 전후로 태스크바와 알림 영역의 상태가 조금도 변하지 않습니다.
- `/W4` 경고 없이 Debug와 Release 모두 빌드됩니다.

---

## 1. 환경에서 이미 확인된 사실

지시서를 쓰기 전에 확인한 값입니다. 다시 조사하지 않아도 됩니다.

| 항목 | 값 |
|---|---|
| 대상 OS | Windows 11 Pro, 빌드 26200 |
| 빌드 도구 | CMake 3.25 이상, 생성기는 `Visual Studio 17 2022` |
| 서브시스템 | `add_executable(bamti WIN32 ...)`이므로 **표준 출력이 콘솔에 붙지 않습니다** |
| 단일 인스턴스 | `host.cpp`의 `Run()`이 `Local\bamti.singleton` 뮤텍스를 잡습니다 |
| 인자 파싱 선례 | `host.cpp:126` `CommandLineHasRestoreTaskbar()` |
| 로그와 경로 | `log.hpp`의 `Log()`, `paths.hpp`의 `DataDir()` |
| 현재 실행 상태 | bamti가 상주 중이면 알림 영역 창이 `SW_HIDE` 상태로 y=32000에 주차되어 있습니다(`taskbar_controller.cpp:155` `HideTrayWindows`) |

마지막 항목이 중요합니다. 주차된 상태에서 측정한 값과 정상 상태에서 측정한 값이 다를 수 있으므로, **탐침은 두 상태에서 각각 한 번씩 돌립니다.** 4-1절에 방법을 적었습니다.

---

## 2. 파일과 배선

### 2-1. 새 파일

```
src/tray_probe.hpp
src/tray_probe.cpp
```

```cpp
// src/tray_probe.hpp
#pragma once

namespace bamti {

// 알림 영역 구조를 측정해 보고서를 파일로 남긴다. 시스템 상태를 바꾸지 않는다.
// 반환값은 프로세스 종료 코드다. 0은 보고서 생성 성공을 뜻하며, 측정 결과의 좋고 나쁨과는 무관하다.
int RunTrayProbe();

}  // namespace bamti
```

`tray_probe.cpp`는 상주 경로에서 호출되지 않습니다. 다른 모듈이 이 파일을 참조하지 않도록 하고, 여기에 쓴 코드를 나중에 트레이 미러 구현이 재사용하려 하지 마십시오. 미러는 별도 파일에 처음부터 다시 씁니다. 탐침 코드는 진단 품질이면 충분하지만 미러 코드는 상주 품질이어야 하므로 요구 수준이 다릅니다.

### 2-2. CMakeLists.txt

`src/tray_probe.cpp`를 `add_executable` 목록에 추가하고, `target_link_libraries`에 `uiautomationcore`를 추가합니다. 다른 항목은 건드리지 마십시오.

### 2-3. host.cpp 배선

`CommandLineHasRestoreTaskbar()` 옆에 같은 모양으로 `CommandLineHasProbeTray()`를 만듭니다. 인자는 `--probe-tray`입니다.

`Run()`의 **가장 앞**, 즉 단일 인스턴스 뮤텍스를 잡기 전에 처리합니다. 이유는 두 가지입니다. 상주 인스턴스가 떠 있는 상태에서도 측정할 수 있어야 하고, 탐침이 뮤텍스를 잡으면 상주 인스턴스가 죽은 것으로 오인될 수 있기 때문입니다.

```cpp
int Run(HINSTANCE instance) {
  if (CommandLineHasProbeTray()) {
    LogInit();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int code = RunTrayProbe();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    LogShutdown();
    return code;
  }

  const bool restore_taskbar = CommandLineHasRestoreTaskbar();
  ...
}
```

UI Automation은 STA에서 호출해도 되지만, 이 프로세스는 메시지 루프를 돌리지 않으므로 콜백을 등록하지 마십시오. 동기 조회만 씁니다.

### 2-4. 출력 경로와 콘솔

보고서는 `JoinPath(DataDir(), L"probe-tray.txt")`에, 캡처는 `JoinPath(DataDir(), L"probe-tray.png")`에 씁니다. 기존 파일은 덮어씁니다.

WIN32 서브시스템이라 표준 출력이 없으므로, 다음을 최선 노력으로 시도합니다. 실패해도 무시합니다.

```cpp
if (AttachConsole(ATTACH_PARENT_PROCESS)) {
  FILE* out = nullptr;
  freopen_s(&out, "CONOUT$", "w", stdout);
}
```

콘솔에는 보고서 전문을 쏟지 말고 요약 다섯 줄과 보고서 파일 경로만 출력합니다. 전문은 파일에 있습니다.

---

## 3. 측정 항목

보고서는 사람이 읽는 텍스트입니다. 각 항목을 `## 1. ...` 형태의 제목으로 구분하고, 값은 표나 한 줄 레코드로 남깁니다.

### 3-1. 환경 헤더

- 실행 시각(현지 시각과 UTC)
- `RtlGetVersion`으로 얻은 OS 빌드 번호. `GetVersionEx`는 매니페스트에 따라 거짓 값을 돌려주므로 쓰지 마십시오.
- 프로세스 아키텍처와 `IsWow64Process2` 결과
- 프로세스가 관리자 권한으로 올라갔는지 여부(`TOKEN_ELEVATION`)
- **bamti 상주 인스턴스가 실행 중인지 여부.** `OpenMutexW(SYNCHRONIZE, FALSE, L"Local\bamti.singleton")`으로 판정하고 즉시 닫습니다. 이 값이 보고서 해석의 전제가 되므로 반드시 남깁니다.

### 3-2. 창 트리 덤프

다음 최상위 창을 찾습니다. 각각 여러 개일 수 있으므로 `FindWindowExW(nullptr, prev, class, nullptr)` 반복으로 전부 열거합니다.

```
Shell_TrayWnd
Shell_SecondaryTrayWnd
NotifyIconOverflowWindow
TopLevelWindowForOverflowXamlIsland
```

찾은 창마다 `EnumChildWindows`로 **깊이 4까지** 재귀 덤프합니다. 노드마다 다음을 한 줄로 남깁니다.

```
depth  hwnd  class  title  style  exstyle  rect(l,t,r,b)  visible  pid
```

- 클래스명과 제목은 `GetClassNameW`, `GetWindowTextW`로 얻습니다. 제목 조회가 크로스 프로세스에서 블로킹될 수 있으므로 전체 덤프에 5초 상한을 두고, 상한을 넘기면 그 지점에서 중단한 사실을 남깁니다.
- 각 노드의 PID도 남깁니다. explorer가 아닌 프로세스가 섞여 있는지 확인하기 위함입니다.
- 트리 전체 노드 수가 500을 넘으면 거기서 중단하고 중단 사실을 남깁니다.

### 3-3. ToolbarWindow32 탐색과 버튼 수

3-2에서 덤프한 트리 전체에서 클래스명이 `ToolbarWindow32`인 노드를 모읍니다. 각각에 대해 다음을 남깁니다.

```
hwnd  부모 경로(클래스명을 > 로 연결)  pid  버튼수  실패사유
```

버튼 수는 `SendMessageTimeoutW(hwnd, TB_BUTTONCOUNT, 0, 0, SMTO_ABORTIFHUNG, 200, &result)`로 얻습니다. **`SendMessageW`를 쓰지 마십시오.** explorer가 응답하지 않으면 탐침이 멈춥니다.

이 절의 결과가 계획 문서 5-0절 판정표의 첫 번째 입력입니다.

### 3-4. 버튼 데이터 덤프

3-3에서 버튼 수가 1 이상인 툴바가 있을 때만 수행합니다. 없으면 "해당 없음"을 남기고 넘어갑니다.

절차는 다음과 같습니다.

1. `GetWindowThreadProcessId`로 PID를 얻고 `OpenProcess`를 `PROCESS_VM_OPERATION`, `PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_QUERY_LIMITED_INFORMATION` 권한으로 호출합니다. 실패하면 오류 코드를 남기고 이 툴바를 건너뜁니다.
2. `VirtualAllocEx(process, nullptr, 4096, MEM_COMMIT, PAGE_READWRITE)`로 버퍼를 하나 잡습니다. 함수 끝에서 반드시 `VirtualFreeEx`와 `CloseHandle`로 정리합니다.
3. 버튼마다 `TB_GETBUTTON`을 보내 원격 버퍼에 `TBBUTTON`을 채우게 하고, `ReadProcessMemory`로 우리 쪽에 읽어 옵니다.
4. `TBBUTTON.dwData`가 가리키는 주소를 다시 `ReadProcessMemory`로 읽습니다. 이 구조는 문서화되어 있지 않으며 다음 배치로 알려져 있습니다.

```cpp
// 비공개 구조다. 배치가 맞는지 확인하는 것이 이 탐침의 목적 중 하나다.
struct TrayItemData {
  HWND  hwnd;
  UINT  uID;
  UINT  uCallbackMessage;
  DWORD reserved[2];
  HICON hIcon;
};
```

explorer와 bamti가 모두 x64이므로 포인터 크기가 같습니다. 구조 배치를 임의로 조정하지 마십시오.

5. 버튼 텍스트는 `TB_GETBUTTONTEXTW`로 얻습니다. 같은 원격 버퍼를 재사용하되 `TBBUTTON` 영역과 겹치지 않게 오프셋을 나눕니다.

각 버튼에 대해 다음을 남깁니다. **검증 결과를 반드시 함께 남기십시오.** 값이 그럴듯한지가 이 절의 핵심입니다.

```
idx  idCommand  fsState  fsStyle  dwData
     hwnd=0x...  IsWindow=yes/no  ownerPid=...  ownerClass=...  ownerTitle=...
     uID=...  uCallbackMessage=0x...  (WM_USER 이상인가: yes/no)
     hIcon=0x...  GetIconInfo=ok/fail  크기=WxH
     text="..."
```

세 가지 검증(`IsWindow`, 콜백 메시지가 `WM_USER` 이상, `GetIconInfo` 성공)을 통과한 버튼 수를 절 끝에 합계로 남깁니다. **이 합계가 0보다 커야 레거시 백엔드가 실현 가능합니다.**

읽기 실패 가능성이 있는 구간이므로, 한 버튼이 실패해도 다음 버튼으로 계속 진행합니다.

### 3-5. UI Automation 열거

`CoCreateInstance(CLSID_CUIAutomation, ..., IID_IUIAutomation, ...)`로 시작합니다. 실패하면 HRESULT를 남기고 이 절을 건너뜁니다.

3-2에서 찾은 최상위 창마다 `ElementFromHandle`로 요소를 얻고, `RawViewWalker`가 아니라 `ControlViewWalker`로 **깊이 6까지** 순회합니다. 노드마다 다음을 남깁니다.

```
depth  ControlType  Name  AutomationId  ClassName  BoundingRectangle  IsOffscreen
       patterns: Invoke=yes/no  LegacyIAccessible=yes/no  ExpandCollapse=yes/no
```

패턴 지원 여부는 `GetCurrentPropertyValue(UIA_IsInvokePatternAvailablePropertyId)` 같은 속성 조회로 확인합니다. 실제로 `Invoke()`를 호출하지 마십시오. 시스템 상태를 바꾸면 안 됩니다.

노드 수가 300을 넘으면 중단하고 중단 사실을 남깁니다. 전체 순회에 10초 상한을 둡니다.

절 끝에 **트레이 아이콘으로 보이는 요소의 개수**를 별도로 셉니다. 판정 기준은 `ControlType`이 `Button`이고 `Name`이 비어 있지 않으며 `BoundingRectangle`의 넓이가 0보다 큰 요소입니다. 이 수가 실제 트레이 아이콘 수와 맞는지를 사람이 비교할 것이므로, 개수와 함께 각 `Name`을 나열합니다.

### 3-6. PrintWindow 캡처

3-2에서 찾은 `Shell_TrayWnd` 중 첫 번째를 대상으로 합니다. 대상 창의 자식 중 알림 영역에 해당하는 것을 찾을 수 있으면 그것을 우선합니다.

```cpp
const UINT kRenderFullContent = 0x00000002;  // PW_RENDERFULLCONTENT
BOOL ok = PrintWindow(target, mem_dc, kRenderFullContent);
```

32비트 top-down DIB(`BI_RGB`, `biHeight`를 음수로)에 그리고 WIC로 PNG로 저장합니다. `windowscodecs`는 이미 링크되어 있습니다.

**저장만 하지 말고 판정에 쓸 수치를 함께 계산합니다.**

- 캡처 크기(픽셀)
- `PrintWindow` 반환값
- 전체 픽셀 중 검정이 아닌 픽셀의 비율(백분율, 소수점 둘째 자리)
- 알파 채널이 0이 아닌 픽셀의 비율

캡처가 사실상 검게 나왔는지를 눈으로 확인하지 않고 수치로 판정할 수 있어야 합니다. 계획 문서 5-0절 판정표의 세 번째 줄이 이 수치를 봅니다.

대상 창이 `IsWindowVisible`에서 거짓이면 그 사실을 캡처 결과 옆에 분명하게 남기십시오. 주차된 상태의 캡처 실패는 정상 상태의 실패와 뜻이 다릅니다.

---

## 4. 실행과 결과 정리

### 4-1. 두 상태에서 각각 실행합니다

1. **상주 인스턴스가 떠 있는 상태**에서 한 번 실행합니다. 결과 파일 두 개를 `probe-running.txt`, `probe-running.png`로 복사해 둡니다.
2. bamti를 종료하고 태스크바가 정상 복구된 것을 확인한 뒤 다시 실행합니다. 결과를 `probe-normal.txt`, `probe-normal.png`로 복사합니다.
3. 두 결과의 차이, 특히 3-3의 버튼 수와 3-6의 비검정 픽셀 비율이 달라지는지 비교합니다.

측정 중에는 트레이 아이콘이 **5개 이상** 떠 있어야 합니다. 부족하면 상주 앱을 몇 개 띄운 뒤 측정하고, 무엇을 띄웠는지 보고서에 적습니다.

### 4-2. PROBE-TRAY.md 작성

저장소 루트에 `PROBE-TRAY.md`를 만들고 다음 구조로 정리합니다.

```markdown
# 알림 영역 탐침 결과

측정 일시, OS 빌드, 측정 시 떠 있던 트레이 아이콘 목록

## 판정

| 항목 | 결과 |
|---|---|
| ToolbarWindow32 버튼 수 (상주 중 / 정상) | |
| 검증 통과 버튼 수 | |
| UIA 트레이 아이콘 후보 수 | |
| PrintWindow 비검정 픽셀 비율 (상주 중 / 정상) | |
| 계획 문서 5-0절 판정표 적용 결과 | 레거시 / UIA / 축소 대안 |

## 원본 보고서

(probe-normal.txt 전문)

## 상주 중 보고서

(probe-running.txt 전문)
```

`판정` 표의 마지막 줄이 이 작업의 최종 산출물입니다.

### 4-3. 계획 문서 갱신

`PLAN-TRAY-TO-TOPBAR.md`의 다음 두 곳을 측정 결과로 고칩니다.

- 5-0절: 판정표는 그대로 두고, 표 아래에 측정 결과와 그에 따라 구현할 백엔드 이름을 한 줄 추가합니다. 근거로 `PROBE-TRAY.md`를 가리킵니다.
- 10절 미해결 항목 1번과 2번: 답이 나왔으므로 해결 표시를 하고 결과를 적습니다.

---

## 5. 하지 말아야 할 것

1. **시스템 상태를 바꾸지 않습니다.** `ShowWindow`, `SetWindowPos`, `SetForegroundWindow`, UIA `Invoke`처럼 상태를 바꾸는 호출을 전부 쓰지 않습니다. 허용되는 것은 조회성 메시지(`TB_BUTTONCOUNT`, `TB_GETBUTTON`, `TB_GETBUTTONTEXTW`)뿐입니다.
2. **explorer에 인젝션하지 않습니다.** `VirtualAllocEx`와 `ReadProcessMemory`는 툴바 조회에 필요한 최소한이며, 원격 스레드 생성이나 코드 주입은 하지 않습니다.
3. **`SendMessageW`를 쓰지 않습니다.** 크로스 프로세스 조회는 전부 `SendMessageTimeoutW`에 `SMTO_ABORTIFHUNG`과 200ms 상한을 씁니다.
4. **탐침 코드를 상주 경로에서 부르지 않습니다.** `--probe-tray`로만 진입합니다.
5. **결과를 해석해서 요약하지 않습니다.** 값을 그대로 남깁니다. "레거시 백엔드가 동작함" 같은 결론을 코드가 출력하게 만들지 마십시오. 3-4절의 검증 통과 합계처럼 기계적으로 셀 수 있는 수치만 남깁니다.
6. **핸들을 흘리지 않습니다.** `OpenProcess`, `VirtualAllocEx`, `CreateDIBSection`, WIC 인터페이스를 전부 정리합니다. 짧게 사는 도구라도 정리 코드를 생략하지 마십시오. 이 코드를 읽고 미러를 구현할 사람이 그대로 따라 씁니다.

---

## 6. 검증

- [ ] `/W4` 경고 없이 Debug와 Release가 빌드됩니다.
- [ ] `bamti.exe --probe-tray`가 상주 인스턴스가 떠 있는 상태에서도 실행되고, 상주 인스턴스가 죽지 않습니다.
- [ ] 실행이 30초 안에 끝납니다.
- [ ] 보고서에 3-1부터 3-6까지 여섯 절이 모두 있습니다. 측정하지 못한 절도 사유와 함께 존재해야 합니다.
- [ ] explorer가 응답하지 않는 상황을 만든 뒤 실행해도 탐침이 멈추지 않고 시간 초과로 끝납니다.
- [ ] 실행 전후로 태스크바 상태, 알림 영역 아이콘 개수, 상주 bamti의 동작이 동일합니다.
- [ ] 핸들 누수가 없습니다. 실행 직전과 직후에 시스템 핸들 수를 비교합니다.

---

## 7. 커밋

| 순서 | 작업 | 커밋 메시지 |
|---|---|---|
| 1 | 탐침 도구 구현과 배선 | `chore: 알림 영역 구조를 진단하는 탐침을 추가한다` |
| 2 | 측정 결과 기록과 계획 갱신 | `docs: 알림 영역 탐침 결과를 기록하고 백엔드를 확정한다` |

두 커밋을 분리하십시오. 첫 커밋은 코드이고 두 번째 커밋은 측정 결과입니다. 나중에 다른 컴퓨터에서 다시 측정할 때 코드 커밋만 골라 쓸 수 있어야 합니다.
