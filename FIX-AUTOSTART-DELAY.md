# 작업 지시서 25: 부팅 후 bamti가 뜨기까지 1분 넘게 걸린다

로그온한 뒤 상단바와 독이 나타날 때까지 1분 이상 비어 있습니다. 원인은 앱의 기동 속도가 아니라 **자동 시작 방식**입니다.

---

## 1. 원인

### 1-1. 앱 자체는 느리지 않다

로그에서 `[host] start`와 `menu bar ready` 사이를 재면 값이 이렇습니다.

```
2026-09-02 07:39:08.023 [host] start ...
2026-09-02 07:39:09.398 [host] menu bar ready taskbar_hidden=1
```

1.4초입니다. 다른 세션도 0.7~2.2초 범위이므로 기동 자체는 문제가 아닙니다.

### 1-2. 지연은 시작 항목이 실행되기까지의 시간이다

`RESEARCH-TRAY-REREGISTER.md` 6-1절에 재부팅 측정이 남아 있습니다.

> 재부팅 시각은 20:54:57이고, 로그온 시작 항목으로 자동 기동한 세션은 20:56:04입니다.

67초입니다. 이 시간은 우리 코드가 도는 시간이 아니라 **Windows가 우리를 실행해 주기까지 기다린 시간**입니다.

`src/autostart.cpp`는 자동 시작을 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`에 값을 써서 구현하고 있습니다.

```cpp
constexpr wchar_t kRunSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
```

이 키의 항목은 explorer가 셸 초기화를 마친 뒤에 실행합니다. 게다가 Windows는 로그온 직후 체감 속도를 지키려고 시작 항목 실행을 의도적으로 미루고, 항목들을 하나씩 차례로 띄웁니다. 시작 앱이 여러 개일수록 뒤로 밀립니다. `Run` 키를 쓰는 한 이 지연은 우리가 줄일 수 없습니다.

### 1-3. 대안은 작업 스케줄러의 로그온 트리거다

작업 스케줄러의 로그온 트리거는 explorer의 시작 항목 대기열을 거치지 않습니다. 지연을 `PT0S`로 두면 로그온 직후 곧바로 실행됩니다.

덤으로 얻는 것이 하나 더 있습니다. 트레이 가로채기는 우리가 `Shell_TrayWnd` 우선순위를 얼마나 빨리 잡느냐에 성패가 달려 있는데(`RESEARCH-TRAY-REREGISTER.md` 4절), 더 일찍 뜰수록 유리합니다.

---

## 2. 수정 내용

### 2-1. 자동 시작 백엔드를 작업 스케줄러로 바꾼다

`src/autostart.cpp`를 고칩니다. 공개 인터페이스(`AutostartEnabled`, `SetAutostart`)는 그대로 두고 안쪽만 바꾸십시오. 호출하는 쪽을 건드릴 필요가 없습니다.

필요한 헤더와 라이브러리입니다.

```cpp
#include <taskschd.h>
#include <wrl/client.h>
```

`CMakeLists.txt`의 `target_link_libraries`에 `taskschd`와 `secur32`를 더하십시오.

작업 이름은 `bamti`로 하고 루트 폴더(`\`)에 등록합니다.

```cpp
constexpr wchar_t kTaskName[] = L"bamti";
```

### 2-2. 등록 규격

`ITaskService`로 연결한 뒤 다음 값을 설정합니다. 지연을 만드는 설정이 기본값에 들어 있으므로 **아래 항목을 빠짐없이 지정해야 합니다.**

| 대상 | 값 | 이유 |
|---|---|---|
| `IPrincipal::put_LogonType` | `TASK_LOGON_INTERACTIVE_TOKEN` | 로그온한 사용자의 세션에서 돌아야 창을 띄울 수 있다 |
| `IPrincipal::put_RunLevel` | `TASK_RUNLEVEL_LUA` | 매니페스트가 `asInvoker`다. 승격을 요구하면 안 된다 |
| `IPrincipal::put_UserId` | 현재 사용자 (`DOMAIN\user`) | `GetUserNameExW(NameSamCompatible, ...)`로 얻는다 |
| `ILogonTrigger::put_Delay` | `PT0S` | 이 지시서의 목적 그 자체다 |
| `ILogonTrigger::put_UserId` | 현재 사용자 | 다른 계정으로 로그온했을 때 뜨지 않게 한다 |
| `ITaskSettings::put_DisallowStartIfOnBatteries` | `VARIANT_FALSE` | 기본값이 `TRUE`다. 이대로 두면 노트북에서 아예 실행되지 않는다 |
| `ITaskSettings::put_StopIfGoingOnBatteries` | `VARIANT_FALSE` | 전원을 뽑으면 상단바가 사라진다 |
| `ITaskSettings::put_ExecutionTimeLimit` | `PT0S` | 기본값은 3일이고, 그 뒤에 강제 종료된다 |
| `ITaskSettings::put_MultipleInstances` | `TASK_INSTANCES_IGNORE_NEW` | 이미 도는 인스턴스를 죽이지 않는다 |
| `ITaskSettings::put_StartWhenAvailable` | `VARIANT_TRUE` | 트리거를 놓쳤을 때 뒤늦게라도 실행한다 |
| `IIdleSettings::put_StopOnIdleEnd` | `VARIANT_FALSE` | 유휴 상태를 이유로 내리지 않는다 |
| `IExecAction::put_Path` | 현재 실행 파일의 전체 경로 | `GetModuleFileNameW`로 얻는다. **따옴표를 붙이지 않는다** |

등록은 `ITaskFolder::RegisterTaskDefinition`에 `TASK_CREATE_OR_UPDATE`와 `TASK_LOGON_INTERACTIVE_TOKEN`을 넘겨 부릅니다.

`_bstr_t`와 `_variant_t`는 이 저장소가 쓰지 않는 헤더에 있습니다. `SysAllocString`/`SysFreeString`과 `VARIANT`를 직접 다루되, 해제를 빠뜨리지 않도록 작은 RAII 도우미를 파일 지역에 하나 두십시오. COM 포인터는 이 저장소의 다른 파일처럼 `Microsoft::WRL::ComPtr`를 쓰십시오.

`CoInitializeEx`는 `Run`이 이미 호출한 뒤이므로 다시 부르지 마십시오. 다만 `SetAutostart`가 COM 초기화 이전에 불릴 가능성이 있다면 `CoInitializeEx`의 반환값을 보고 `S_FALSE`와 `RPC_E_CHANGED_MODE`를 성공으로 취급한 뒤, 자기가 초기화했을 때만 `CoUninitialize`를 부르는 형태로 감싸십시오.

### 2-3. 판정과 해제

```cpp
bool AutostartEnabled();   // 작업이 있고 사용 가능하면 참. 없으면 기존 Run 값으로 판정한다.
bool SetAutostart(bool on);
```

- `AutostartEnabled()`는 `ITaskFolder::GetTask`로 작업을 찾고 `IRegisteredTask::get_Enabled`가 `VARIANT_TRUE`인지 봅니다. 작업이 없으면 지금의 `RunValueExists() && StartupApprovedAllows()` 판정을 그대로 씁니다.
- `SetAutostart(true)`는 작업을 등록하고, **성공했을 때만** `Run` 값을 지웁니다. 등록이 실패하면 지금의 `Run` 방식으로 되돌아가고 로그를 남기십시오. 자동 시작이 아예 걸리지 않는 것보다 늦게라도 걸리는 편이 낫습니다.
- `SetAutostart(false)`는 작업 삭제(`ITaskFolder::DeleteTask`)와 `Run` 값 삭제를 **둘 다** 합니다. 한쪽만 지우면 끈 뒤에도 계속 뜹니다.

### 2-4. 기존 사용자를 옮긴다

이미 `Run` 값으로 자동 시작을 켜 둔 상태이므로, 앱이 스스로 한 번 옮겨야 합니다.

```cpp
// Run 키로 걸려 있던 자동 시작을 작업 스케줄러로 옮긴다. 이미 옮겼거나 자동 시작이
// 꺼져 있으면 아무 일도 하지 않는다.
void AutostartMigrate();
```

`src/host.cpp`의 `Run`에서 `MenuBar`를 만들기 전에 한 번 부르십시오. 동작 규칙은 다음과 같습니다.

1. 작업이 이미 있으면 아무것도 하지 않고 끝냅니다.
2. `Run` 값이 없으면(자동 시작이 꺼져 있으면) 아무것도 하지 않고 끝냅니다. **사용자가 끈 것을 우리가 다시 켜면 안 됩니다.**
3. 둘 다 아니면 작업을 등록하고, 성공했을 때만 `Run` 값을 지웁니다.
4. 어느 쪽으로 갈렸는지 로그를 남깁니다. `[host] autostart migrate task=1 run_removed=1` 형태면 충분합니다.

### 2-5. 셸보다 먼저 떴을 때를 대비한다

로그온 직후에 실행되면 explorer의 `Shell_TrayWnd`가 아직 없을 수 있습니다. 그러면 `TaskbarController::Hide()`가 숨길 창을 찾지 못해 `bar.taskbar_hidden()`이 거짓이 되고, `src/host.cpp`의 이 분기 때문에 **독이 아예 만들어지지 않습니다.**

```cpp
      if (bar.taskbar_hidden()) {
        if (!dock.Create(instance, bar.hwnd())) {
```

`MenuBar::Create`를 부르기 전에 셸을 기다리십시오.

```cpp
// 작업 스케줄러로 로그온 직후에 뜨면 explorer의 셸 창이 아직 없을 수 있다.
// 최대 30초 동안 200ms 간격으로 기다린다. 시간이 다 되어도 그냥 진행한다.
// 뒤늦게 셸이 뜨는 경우는 TaskbarCreated 처리가 회복시킨다.
void WaitForShell(DWORD timeout_ms);
```

판정 조건은 `FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr`입니다. 기다린 시간을 로그로 남기십시오.

```
[host] shell wait ms=1400 found=1
```

`PrestartInterceptTrayBackend()`는 지금 위치를 그대로 두십시오. 그 함수는 셸보다 먼저 자리를 잡는 것이 목적이므로 대기 앞에 있어야 합니다.

---

## 3. 검증

### 3-1. 등록 상태 확인 (조회만 한다)

앱의 설정에서 자동 시작을 껐다가 다시 켠 뒤, PowerShell에서 **읽기 명령만** 써서 확인합니다.

```powershell
schtasks /query /tn bamti /fo list /v
reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v bamti
```

확인할 것은 다음과 같습니다.

1. 작업이 존재하고 `사용 안 함`이 아닙니다.
2. 트리거가 로그온이고 지연이 없습니다.
3. `Run` 값은 **없어야** 합니다(`오류: 지정된 레지스트리 키 또는 값을 찾을 수 없습니다`가 정상입니다).
4. 자동 시작을 끄면 작업과 `Run` 값이 **둘 다** 사라집니다.

**레지스트리에 값을 쓰거나 지우는 명령을 검증 절차로 실행하지 마십시오.** 확인은 앱의 설정 토글로만 하고, 명령줄은 조회에만 씁니다.

### 3-2. 실제 재부팅 측정

재부팅한 뒤 다음 세 값을 재서 보고하십시오.

```powershell
(Get-CimInstance Win32_OperatingSystem).LastBootUpTime
(Get-Process explorer | Sort-Object StartTime | Select-Object -First 1).StartTime
Select-String -Path "$env:USERPROFILE\.bamti\bamti.log" -Pattern "\[host\] start" | Select-Object -Last 1
```

판정 기준입니다.

- **explorer 시작 시각과 `[host] start` 시각의 차이가 10초 이내**여야 합니다. 이 값이 이번 작업의 성패입니다. 이전 측정값은 부팅 시각 대비 67초였습니다.
- 로그의 `shell wait ms=` 값을 함께 적어 주십시오. 이 값이 0에 가까우면 explorer가 먼저 떠 있었다는 뜻이고, 수천 ms면 우리가 먼저 떴다는 뜻입니다. 어느 쪽이든 정상이지만, 30000에 가까우면 대기 조건이 잘못된 것입니다.

### 3-3. 기능 확인

재부팅 뒤에 다음이 모두 정상이어야 합니다.

1. 상단바가 보이고 시계와 위젯이 갱신됩니다.
2. **독이 보입니다.** 2-5절의 대기가 없으면 여기가 깨집니다.
3. 작업 표시줄이 숨겨져 있습니다.
4. 트레이 아이콘이 상단바에 미러링되어 있습니다. 로그의 `intercept roster`와 `uia roster`를 대조해 서드파티 손실이 없는지 확인하십시오. 판정 방법은 `RESEARCH-TRAY-REREGISTER.md` 6-1절의 "측정 방법"에 있습니다.
5. bamti가 두 번 뜨지 않았습니다. 작업 관리자에서 프로세스가 하나인지 확인합니다. 마이그레이션이 `Run` 값을 지우지 못하면 여기서 드러납니다.

---

## 4. 주의 사항

- 작업 이름을 `\bamti`가 아닌 하위 폴더에 만들지 마십시오. 폴더를 만들면 삭제 경로도 함께 관리해야 하는데 얻는 것이 없습니다.
- `TASK_RUNLEVEL_HIGHEST`를 쓰지 마십시오. 이 앱은 승격이 필요 없고, 승격된 프로세스는 일반 권한 프로세스와 드래그 앤드 드롭 같은 상호 작용이 막힙니다.
- `IExecAction::put_Path`에 따옴표를 감싸지 마십시오. `Run` 키와 달리 여기는 명령줄이 아니라 경로 필드입니다. 따옴표를 넣으면 파일을 찾지 못합니다.
- `StartupApproved` 키(`Explorer\StartupApproved\Run`)를 계속 다루는 코드는 `Run` 폴백 경로에서만 의미가 있습니다. 작업 스케줄러 경로에는 대응물이 없으니 억지로 연결하지 마십시오.
- 실행 파일을 다른 경로로 옮기면 작업이 깨집니다. 지금의 `Run` 방식도 같은 한계를 갖고 있으므로 이번 범위에서는 다루지 않습니다. 다만 `AutostartEnabled()`가 참인데 작업의 등록된 경로가 현재 실행 파일과 다르면 조용히 다시 등록하는 코드를 넣어 두면 좋습니다. 넣는다면 `AutostartMigrate` 안에서 처리하십시오.
- 지연을 줄이려고 `Run` 키와 작업 스케줄러를 **둘 다** 켜 두지 마십시오. 단일 인스턴스 뮤텍스가 중복 실행을 막기는 하지만, 매 로그온마다 프로세스가 하나 더 떠서 즉시 죽는 낭비가 생깁니다.
