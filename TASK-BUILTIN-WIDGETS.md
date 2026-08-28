# 작업 지시서: 내장 위젯 공급자

`PLAN-TRAY-TO-TOPBAR.md`의 4단계를 구현하는 지시서입니다. 계획 문서와 이 지시서가 어긋나면 **계획 문서가 우선**입니다.

3단계에서 만든 프로토콜과 공급자 레지스트리는 아직 파이프 클라이언트 한 종류만 통과했습니다. 이 단계의 목적은 두 가지입니다. 첫째, bamti 자신이 `StatusSource`의 두 번째 구현체가 되어 레지스트리의 다중 공급자 경로를 실제로 사용합니다. 둘째, 5단계 트레이 미러가 그대로 따라 쓸 **작업자 스레드 수명 관리와 절전 규칙의 본보기**를 만듭니다. 트레이 미러는 이 단계에서 정한 구조를 복제할 예정이므로, 여기서 스레드 수명이 어설프면 다음 단계에서 같은 결함이 두 배로 늘어납니다.

만드는 것은 배터리와 CPU와 네트워크 위젯 세 개, 그리고 위젯 보드를 여는 단추 하나입니다. 날씨는 이 단계의 범위가 아닙니다.

---

## 0. 완료 조건

- 세 위젯과 보드 단추가 각각 독립적으로 켜지고 꺼지며, **기본값은 전부 꺼짐**입니다.
- 상단바 빈 곳을 우클릭하면 위젯 표시 여부를 켜고 끄는 항목이 나오고, 선택이 즉시 반영되며, 다시 실행해도 그 선택이 유지됩니다.
- 세 위젯을 모두 켠 채로 10분 동안 유휴 상태를 유지했을 때 bamti.exe의 CPU 사용률이 0.2% 미만입니다.
- 네 항목을 모두 끈 상태에서는 위젯 작업자 스레드가 **생성되지 않습니다.**
- 전체화면 가림, 세션 잠금, 화면 꺼짐 상태에서 표본 추출이 멈추고, 복귀할 때 값이 튀지 않습니다.
- 기존 v1과 v2 파이프 클라이언트의 동작이 그대로입니다.
- `/W4` 경고 없이 Debug와 Release가 모두 빌드됩니다.

---

## 1. 만들 파일

| 파일 | 역할 |
|---|---|
| `src/settings.hpp` / `.cpp` | `%USERPROFILE%\.bamti\settings.json`에서 `topbar.widgets`를 읽고 씁니다. |
| `src/widgets/builtin.hpp` / `.cpp` | `BuiltinWidgets` 공급자. 세 위젯과 보드 단추를 한 스레드에서 처리합니다. |

고칠 파일은 `src/menu_bar.hpp`, `src/menu_bar.cpp`, `src/pipe_server.cpp`, `CMakeLists.txt`, `bamti.vcxproj`, `docs/STATUS-PROTOCOL.md`입니다.

---

## 2. 설정 파일

### 2-1. 형식

계획 문서 7단계가 정한 최종 형식 가운데 위젯 부분만 이번에 씁니다. 나머지 키는 아직 만들지 않습니다.

```json
{
  "topbar": {
    "widgets": {"battery": false, "cpu": false, "network": false, "widget_board": false}
  }
}
```

### 2-2. 인터페이스

```cpp
// src/settings.hpp
namespace bamti {

struct WidgetSettings {
  bool battery = false;
  bool cpu = false;
  bool network = false;
  bool widget_board = false;
  bool Any() const { return battery || cpu || network || widget_board; }
};

std::wstring SettingsPath();               // paths.cpp의 DataDir() 아래 settings.json
WidgetSettings LoadWidgetSettings();       // 파일이 없거나 깨졌으면 전부 false
bool SaveWidgetSettings(const WidgetSettings& s);

}  // namespace bamti
```

`SettingsPath()`는 `paths.hpp`가 아니라 `settings.hpp`에 둡니다. 경로 조립에는 `paths.hpp`의 `DataDir()`과 `JoinPath()`를 그대로 씁니다.

### 2-3. 읽기

파싱은 `src/json_line.hpp`의 헬퍼로 합니다. 새 JSON 파서를 만들지 마십시오. 이 헬퍼의 `SkipWs`는 줄바꿈을 포함한 모든 공백을 건너뛰므로, 사람이 읽기 좋게 들여쓴 여러 줄 JSON도 그대로 처리됩니다.

```cpp
const std::string text = ReadFileUtf8(SettingsPath());   // 실패하면 기본값을 돌려준다
const auto topbar = json::GetRaw(text, "topbar");
const auto widgets = topbar ? json::GetRaw(*topbar, "widgets") : std::nullopt;
if (widgets) {
  s.battery = json::GetBool(*widgets, "battery").value_or(false);
  // ...
}
```

파일 크기가 64KB를 넘으면 읽지 않고 기본값을 돌려주며 로그를 한 번 남깁니다.

### 2-4. 쓰기

**모르는 키를 지우지 마십시오.** 이 파일은 7단계에서 트레이 미러 설정과 항목 순서를 담게 되고, 사용자가 직접 편집할 수도 있습니다. 위젯 설정을 저장하면서 다른 키를 날려 먹으면 그 손실은 조용히 일어납니다.

규칙은 다음과 같습니다.

1. 기존 파일을 읽어, 최상위에서 `topbar`가 아닌 필드의 **원문 조각을 그대로 보존**합니다.
2. `topbar` 안에서도 `widgets`가 아닌 필드의 원문 조각을 보존합니다.
3. `topbar.widgets`만 네 개의 불리언으로 새로 씁니다.
4. 기존 파일이 없거나 파싱에 실패하면 2-1절의 형태로 새로 만듭니다. 이때 **원본을 덮어쓰기 전에 `settings.json.bak`으로 한 벌 옮기고** 로그를 남깁니다.

원문 조각 순회에는 `json_line.hpp`의 `ForEachField`를 씁니다. 이 함수는 익명 이름 공간에 있지만 헤더를 포함한 번역 단위에서 그대로 부를 수 있습니다.

쓰기는 **원자적으로** 합니다. `settings.json.tmp`에 전부 쓴 뒤 `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)`로 바꿉니다. 쓰다 만 파일이 남으면 다음 실행에서 설정이 통째로 사라집니다.

### 2-5. 어느 스레드에서 하는가

계획 문서 7단계는 설정 파일 입출력을 UI 스레드에서 하지 말라고 정했습니다. 이 지시서는 그 규칙을 다음과 같이 적용합니다.

- **쓰기는 항상 UI 스레드 밖**에서 합니다. 4-6절의 저장 경로를 따릅니다.
- **읽기는 시작할 때 한 번만 UI 스레드에서** 합니다. `MenuBar::Create()`가 창을 보여 주기 전, 메시지 루프가 돌기 전에 일어나는 1KB 미만짜리 파일 읽기 한 번이며, 이 시점에는 응답성이라는 개념 자체가 성립하지 않습니다. 이것을 굳이 백그라운드로 넘기면 "설정을 읽기 전에 위젯을 만들지 말지 판정해야 하는" 경합만 새로 생깁니다. 규칙을 어기는 것이 아니라 규칙의 목적에 맞게 적용하는 것이며, 이 판단의 근거를 코드 주석에 한 줄로 남겨 주십시오.

---

## 3. 공급자 골격

### 3-1. 헤더

```cpp
// src/widgets/builtin.hpp
#pragma once

#include "settings.hpp"
#include "status_source.hpp"

#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include <windows.h>

namespace bamti {

class BuiltinWidgets : public StatusSource {
 public:
  BuiltinWidgets();
  BuiltinWidgets(const BuiltinWidgets&) = delete;
  BuiltinWidgets& operator=(const BuiltinWidgets&) = delete;
  ~BuiltinWidgets() override;

  const char* Name() const override;          // "builtin"
  bool Start(StatusSink* sink) override;
  void Stop() override;
  void OnEvent(const StatusEvent& ev) override;
  void SetActive(bool active) override;

  // 아래 세 개는 UI 스레드에서만 부릅니다.
  WidgetSettings settings() const;
  void SetSettings(const WidgetSettings& next);   // 즉시 반영하고 비동기로 저장한다
  void NotePowerEvent(bool resumed);              // WM_POWERBROADCAST에서 부른다

 private:
  void WorkerLoop();
  void StartWorkerLocked();
  void StopWorker();
  void ResetBaselines();                          // 표본 기준점을 버린다
  // ... 위젯별 표본 추출과 게시
};

}  // namespace bamti
```

### 3-2. 스레드 수명

이 절의 규칙을 정확히 지켜 주십시오. `FIX-HANG.md`와 `FIX-HANG-2.md`와 `FIX-HANG-3.md`가 전부 종료 경로의 대기 때문에 생긴 결함이었습니다.

- 작업자 스레드는 **하나뿐**입니다. 위젯마다 스레드를 만들지 마십시오.
- 스레드를 분리(`detach`)하지 마십시오. 반드시 `join`으로 끝냅니다.
- 이벤트는 두 개입니다. `stop_event_`는 수동 재설정, `wake_event_`는 자동 재설정입니다.
- 스레드는 스스로 끝나지 않습니다. `Stop()`과 `StopWorker()`만 스레드를 끝냅니다.
- `Start()`는 설정을 읽어 하나라도 켜져 있을 때만 스레드를 만듭니다. **전부 꺼져 있으면 스레드를 만들지 않고 `true`를 반환합니다.** `StatusRegistry::StartAll()`은 하나라도 `false`를 받으면 `MenuBar::Create()`를 실패시키므로, 꺼져 있다는 이유로 `false`를 돌려주면 bamti가 아예 뜨지 않습니다.
- `Stop()`은 `stop_event_`를 올리고 `join`을 마친 뒤에 `sink_`를 비웁니다. 순서를 바꾸면 작업자가 이미 사라진 싱크를 만집니다.
- `Stop()`은 `Start()` 없이 불려도, 두 번 불려도 안전해야 합니다. `MenuBar`는 `WM_ENDSESSION`과 `WM_DESTROY`와 소멸자에서 `StopAll()`을 부릅니다.

### 3-3. 대기와 시각

작업자 루프의 뼈대입니다.

```cpp
// 타이머 하나로 여러 주기를 처리한다. 위젯마다 다음 만기 시각을 들고 있다.
HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
for (;;) {
  const ULONGLONG now = GetTickCount64();
  const ULONGLONG due = NextDeadline(now);        // 켜져 있고 활성인 위젯 중 가장 이른 만기
  LARGE_INTEGER rel{};
  rel.QuadPart = -static_cast<LONGLONG>((due - now) * 10000ull);
  SetWaitableTimerEx(timer, &rel, 0, nullptr, nullptr, nullptr, 200);   // 200ms 허용 지연
  const HANDLE waits[] = {stop_event_, wake_event_, timer};
  const DWORD wait = WaitForMultipleObjects(3, waits, FALSE, SinkTimeoutMs());
  // ...
}
```

- `SetWaitableTimerEx`의 마지막 인자인 **허용 지연 200ms를 반드시 넣습니다.** 커널이 다른 타이머와 깨어남을 묶어 주므로, 2초와 5초와 60초 주기가 제각기 CPU를 깨우는 일이 줄어듭니다. 이것이 유휴 CPU 예산과 컨텍스트 스위치 예산의 핵심입니다.
- `SinkTimeoutMs()`는 `sink_->NotifyWaitTimeoutMs()`를 그대로 씁니다. 깨어난 이유가 시간 초과이면 `sink_->Flush()`를 부르고 다시 대기합니다. `PipeServer::ClientLoop`가 이미 같은 방식을 씁니다.
- 활성 위젯이 하나도 없으면 타이머를 걸지 말고 `stop_event_`와 `wake_event_`만 기다립니다. `SetActive(false)` 동안이 이 상태입니다.
- 시각은 전부 `GetTickCount64()`로 잽니다. 표본 사이의 간격은 실제 경과 시간으로 계산하고, 주기 상수를 그대로 나누어 쓰지 마십시오. 타이머가 200ms 늦게 깨면 계산이 그만큼 틀어집니다.

### 3-4. 게시 규칙

- 모든 항목은 `item.source = "builtin"`으로 채웁니다. `StatusRegistry`가 이 이름으로 이벤트를 되돌려 보낼 공급자를 찾습니다.
- `revision`은 `PipeServer::PublishUpsert`와 같은 방식으로 매깁니다. `sink_->Get(id)`로 이전 항목을 얻어 `revision + 1`, 없으면 1입니다.
- **값이 그대로면 아무것도 하지 않습니다.** 위젯마다 마지막으로 게시한 표시 내용의 조합(글리프, 바 텍스트, 상태, 툴팁, 패널의 모든 값 문자열)을 들고 있다가, 전부 같으면 `Upsert`를 부르지 않습니다. CPU 사용률이 12.4%에서 12.6%로 바뀌어도 표시는 둘 다 `12%`이므로 게시하지 않습니다.
- 파서를 거치지 않으므로 **상한을 스스로 지킵니다.** 바 텍스트 32자, 패널 문자열 128자, 글리프 8자, 패널 행 32개입니다. `status_item.hpp`의 `kStatus*` 상수를 쓰고 새 상수를 만들지 마십시오. 패널 렌더러는 값이 이미 검증되었다고 가정하고 그립니다.
- 게이지의 `value`는 0.0 이상 1.0 이하로 클램프한 뒤에 넣습니다.

### 3-5. 글리프

바는 `Segoe UI Variable`, 없으면 `Segoe UI`로 그립니다. 항목마다 폰트를 바꿀 수 없으므로 **DirectWrite 대체 폰트가 흑백으로 그려 주는 기호만** 씁니다. 이모지 표현으로 넘어가는 문자는 쓰지 마십시오. 크기와 세로 정렬이 어긋납니다.

| 위젯 | 글리프 | 비고 |
|---|---|---|
| 배터리 | `▁` `▃` `▅` `▇` `█` | 잔량 20% 구간마다 한 단계. 충전 중이면 바 텍스트 앞에 `+`를 붙입니다. |
| CPU | `▦` | 고정 |
| 네트워크 | `⇅` | 고정 |
| 위젯 보드 | `▤` | 고정 |

글리프는 조정해도 됩니다. 지켜야 할 조건은 두 가지입니다. 이모지로 대체되지 않을 것, 그리고 툴팁 없이도 어느 위젯인지 구분될 것입니다.

---

## 4. 위젯별 사양

우선순위는 큰 값이 시계에 가깝습니다. 배터리 40, CPU 30, 네트워크 20, 보드 단추 10으로 둡니다.

### 4-1. 배터리 (`bamti.widget/battery`)

**표본**: `GetSystemPowerStatus`. 주기는 60초이며, `WM_POWERBROADCAST`를 받으면 즉시 한 번 더 뜹니다(5-2절).

**배터리가 없을 때**: `BatteryFlag`에 `BATTERY_FLAG_NO_BATTERY`(128)가 있거나 `BatteryLifePercent`가 255이면 데스크톱입니다. 이때는 `sink_->Remove(id)`로 항목을 지우고 로그를 한 번만 남깁니다. 0%로 계속 그리지 마십시오.

**세그먼트**

| 필드 | 값 |
|---|---|
| `text` | `82%`, 충전 중이면 `+82%` |
| `tooltip` | `배터리 82% · 충전 중` 또는 `배터리 82% · 남은 시간 2시간 41분` |
| `state` | 충전 중이 아니면서 10% 이하면 `error`, 20% 이하면 `warn`, 그 밖에는 `normal` |

**패널**

| 행 | 내용 |
|---|---|
| `gauge` | 레이블 `잔량`, `value`는 잔량을 100으로 나눈 값, `value_text`는 `82%`, `detail`은 `BatteryLifeTime`이 −1이 아닐 때만 `남은 시간 2시간 41분` |
| `kv` | `전원` → `연결됨` 또는 `배터리 사용 중`. `ACLineStatus`가 255면 `알 수 없음` |
| `kv` | `절전 모드` → `SystemStatusFlag & 1`이면 `켜짐`, 아니면 `꺼짐` |
| `separator` | |
| `button` | `row_id`는 `power_settings`, 레이블은 `전원 설정 열기` |

### 4-2. CPU (`bamti.widget/cpu`)

**표본**: `GetSystemTimes(&idle, &kernel, &user)`. `NtQuerySystemInformation`을 쓰지 마십시오. 주기는 5초입니다.

`kernel`에는 유휴 시간이 포함되어 있습니다. 계산은 다음과 같습니다.

```
total        = kernelΔ + userΔ
usage        = total > 0 ? 1.0 - idleΔ / total : 0.0
user_share   = total > 0 ? userΔ / total : 0.0
kernel_share = total > 0 ? (kernelΔ - idleΔ) / total : 0.0
```

**기준점**: 차분이 필요하므로 첫 표본으로는 게시하지 않습니다. `Start()`와 `SetActive(true)` 직후에 기준점을 한 번 잡고, **첫 게시는 1초 뒤에** 합니다. 그 다음부터 5초 주기입니다. 켜자마자 5초 동안 빈자리로 남지 않게 하려는 것입니다.

**세그먼트**: `text`는 `12%`, 툴팁은 `CPU 12% · 사용자 8% · 커널 4%`, `state`는 항상 `normal`입니다. 사용률이 높다고 경고색을 쓰지 마십시오. 상단바가 붉게 깜빡이는 것은 정보가 아니라 소음입니다.

**패널**: 게이지 `전체 사용률`, `kv` 두 개로 `사용자`와 `커널`, `kv` `논리 프로세서`(`GetSystemInfo`의 `dwNumberOfProcessors`), 구분선, 버튼은 `row_id` `task_manager`에 레이블 `작업 관리자 열기`입니다.

### 4-3. 네트워크 (`bamti.widget/net`)

**표본**: `GetIfTable2`의 `InOctets`와 `OutOctets` 차분. 주기는 2초이며, 기준점 규칙은 CPU와 같습니다. 첫 게시는 1초 뒤입니다.

`WIN32_LEAN_AND_MEAN`이 이미 정의되어 있어 `windows.h`가 옛 `winsock.h`를 끌어오지 않으므로, `iphlpapi.h`는 `windows.h` 뒤에 그대로 포함해도 됩니다. `winsock2.h`를 새로 넣지 마십시오.

**합산 대상**: `OperStatus`가 `IfOperStatusUp`이고, `Type`이 `IF_TYPE_SOFTWARE_LOOPBACK`이 아니며, `InterfaceAndOperStatusFlags.FilterInterface`가 아닌 인터페이스만 더합니다.

**함정 두 가지를 반드시 처리합니다.**

1. `GetIfTable2`가 준 표는 **`FreeMibTable`로 반드시 해제**합니다. 이른 반환 경로마다 새는지 확인하십시오.
2. 인터페이스가 사라지거나(와이파이 끄기, VPN 종료) 카운터가 되돌아가면 합계가 이전보다 **작아질 수 있습니다.** 이때 차분을 음수로 계산하지 말고, 그 회차는 0으로 보고 기준점을 다시 잡습니다.

열거 비용을 재서 1회가 1ms를 넘으면 `Log(L"widget", ...)`로 남겨 주십시오. 넘는 것으로 확인되면, 인터페이스 LUID 목록을 캐시해 매 회차는 `GetIfEntry2`로 조회하고 전체 열거는 30초에 한 번만 하는 방식으로 바꿉니다. **측정하기 전에 미리 캐시를 만들지는 마십시오.**

**속도 표기**: 초당 바이트를 `1.2M`, `120K`, `980`처럼 세 자리 안쪽으로 줄입니다. 1000 미만은 그대로 쓰고, 그 위로는 K와 M과 G를 붙이되 값이 10 미만일 때만 소수 첫째 자리를 답니다. 로캘에 기대지 말고 `swprintf_s`로 만듭니다.

**세그먼트**: `text`는 받기와 보내기를 묶은 `1.2M/120K`, 툴팁은 `받기 1.2 MB/s · 보내기 120 KB/s · Wi-Fi`입니다.

**패널**: `kv` `받기`와 `보내기`, `kv` `인터페이스`(합산 대상 중 누적 옥텟이 가장 많은 인터페이스의 `Alias`), 스타일 `note`인 `text` 행으로 `bamti 시작 이후 받기 3.4G · 보내기 512M`, 구분선, 버튼은 `row_id` `network_settings`에 레이블 `네트워크 설정 열기`입니다.

### 4-4. 위젯 보드 단추 (`bamti.widget/board`)

`panel`이 없는 항목입니다. 글리프 `▤`만 두고 텍스트는 비웁니다. 툴팁은 `위젯 보드 열기`입니다. 표본 추출이 없으므로 타이머 만기를 잡지 않습니다.

클릭하면 `click` 이벤트가 `OnEvent`로 옵니다. `button`이 `left`일 때만 처리하고 우클릭은 무시합니다.

여는 방법은 `SendInput`으로 `LWIN` 누름, `W` 누름, `W` 뗌, `LWIN` 뗌을 보내는 것입니다. 확인해 둔 사실이 하나 있습니다. `menu_bar.cpp`의 저수준 키보드 훅은 `LLKHF_INJECTED`가 붙은 입력을 그대로 통과시키므로, bamti가 자기 시작 메뉴를 여는 일은 생기지 않습니다.

`INPUT` 배열 네 개를 **한 번의 `SendInput` 호출로** 함께 보냅니다. 나누어 보내면 사이에 다른 키 이벤트가 끼어 조합이 깨집니다.

### 4-5. 패널 버튼 처리

`OnEvent`는 **UI 스레드에서** 불립니다. `menu_bar.cpp`의 `status_.Dispatch(ev)`가 호출 지점입니다. 그러므로 여기서 `ShellExecuteW`나 `SendInput`을 직접 부르지 마십시오. 계획 문서 5절의 첫 번째 규칙에 정면으로 어긋납니다.

대신 요청을 큐에 넣고 `wake_event_`를 올려 작업자 스레드가 실행하게 합니다.

| `row_id` | 동작 |
|---|---|
| `power_settings` | `ShellExecuteW(nullptr, L"open", L"ms-settings:powersleep", ...)` |
| `network_settings` | `ShellExecuteW(nullptr, L"open", L"ms-settings:network", ...)` |
| `task_manager` | `ShellExecuteW(nullptr, L"open", L"taskmgr.exe", ...)` |
| 보드 단추의 `click` | 4-4절의 `SendInput` |

작업자 스레드는 시작할 때 `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`를 부르고 끝날 때 `CoUninitialize`를 부릅니다. `ShellExecuteW`가 요구합니다.

버튼을 눌러도 패널은 그대로 열려 있습니다. 닫는 동작을 새로 넣지 마십시오.

### 4-6. 설정 변경과 저장

`SetSettings(next)`는 UI 스레드에서 불립니다. 순서는 다음과 같습니다.

1. 잠금 안에서 설정을 바꿉니다.
2. 이번에 꺼진 위젯의 항목을 `sink_->Remove(id)`로 지웁니다.
3. 하나라도 켜져 있으면 작업자 스레드를 시작하고, 이미 돌고 있으면 `wake_event_`만 올립니다. 전부 꺼졌으면 `StopWorker()`로 `join`합니다.
4. 저장은 **스레드 풀에 넘깁니다.** `TrySubmitThreadpoolCallback`에 설정 사본을 힙으로 넘겨 `SaveWidgetSettings`를 부르게 합니다.

3번과 4번의 순서가 중요합니다. 저장을 작업자 스레드에 맡기면, 마지막 위젯을 끄는 순간 저장할 스레드가 사라져서 그 선택이 파일에 남지 않습니다. 스레드 풀 콜백은 사용자가 메뉴를 누를 때만 잠깐 뜨므로 유휴 예산에 영향을 주지 않습니다.

`Stop()`은 진행 중인 저장 콜백이 끝날 때까지 최대 2초 기다립니다. 저장을 시작할 때 내리고 끝낼 때 올리는 수동 재설정 이벤트 하나면 충분합니다. 기다리지 않으면 종료 도중에 콜백이 이미 사라진 객체를 만집니다.

### 4-7. `SetActive`

`SetActive(false)`: 타이머 만기를 잡지 않아 표본 추출을 멈춥니다. **항목은 지우지 않습니다.** 바가 이미 숨겨져 있거나 화면이 잠겨 있으므로 지울 이유가 없고, 복귀할 때 빈 바가 먼저 보이는 일만 생깁니다.

`SetActive(true)`: `ResetBaselines()`로 CPU와 네트워크의 기준점을 **반드시 버립니다.** 이것을 빠뜨리면 30분 잠겨 있다가 풀었을 때 그 30분 전체의 평균 CPU 사용률과, 30분치 트래픽을 2초로 나눈 터무니없는 전송 속도가 나옵니다. 기준점을 다시 잡고 1초 뒤에 첫 게시를 합니다. 배터리는 차분이 아니므로 즉시 한 번 뜹니다.

---

## 5. `MenuBar` 배선

### 5-1. 등록

`menu_bar.hpp`의 `PipeServer pipe_;` 옆에 `BuiltinWidgets widgets_;`를 둡니다. `menu_bar.cpp`의 `Create()`에서 `status_.Register(&pipe_);` 바로 뒤에 `status_.Register(&widgets_);`를 넣습니다. `StartAll()` 호출 위치는 그대로입니다.

### 5-2. 전원 알림

`WM_POWERBROADCAST`의 `PBT_APMPOWERSTATUSCHANGE`는 최상위 창에 자동으로 전달되므로 따로 등록할 필요가 없습니다. `HandleMessage`에 다음을 추가하고 `TRUE`를 반환합니다.

| `wparam` | 처리 |
|---|---|
| `PBT_APMPOWERSTATUSCHANGE` | `widgets_.NotePowerEvent(false)`로 배터리를 즉시 다시 읽습니다. |
| `PBT_APMRESUMEAUTOMATIC`, `PBT_APMRESUMESUSPEND` | `widgets_.NotePowerEvent(true)`로 기준점을 버립니다. |

### 5-3. 잠금과 화면 꺼짐

계획 문서 5절 4번은 전체화면 가림뿐 아니라 세션 잠금과 화면 꺼짐에서도 모든 공급자가 `SetActive(false)`를 받아야 한다고 정했습니다. 지금은 전체화면만 연결되어 있습니다. 표본을 추출하는 공급자가 처음 생기는 이 단계에서 나머지를 마저 연결합니다.

- `WTSRegisterSessionNotification(hwnd_, NOTIFY_FOR_THIS_SESSION)`을 `Create()`에서 부르고, `WM_DESTROY`와 `WM_ENDSESSION`에서 `WTSUnRegisterSessionNotification`을 부릅니다. `WM_WTSSESSION_CHANGE`의 `WTS_SESSION_LOCK`과 `WTS_SESSION_UNLOCK`을 처리합니다.
- `RegisterPowerSettingNotification(hwnd_, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE)`로 화면 상태를 받습니다. `WM_POWERBROADCAST`의 `PBT_POWERSETTINGCHANGE`에서 `POWERBROADCAST_SETTING`의 `Data`가 0이면 꺼짐입니다. 해제는 `UnregisterPowerSettingNotification`입니다.

세 조건을 각각 `status_.SetActive`에 직접 연결하지 마십시오. 나중에 바뀐 조건이 앞의 조건을 덮어씁니다. 상태를 모아서 판정하는 함수를 하나 둡니다.

```cpp
void MenuBar::UpdateProviderActive() {
  const bool active = !fullscreen_occluded_ && !session_locked_ && display_on_;
  if (active == providers_active_) {
    return;
  }
  providers_active_ = active;
  status_.SetActive(active);
}
```

`SetFullscreenOccluded`의 `status_.SetActive(!occluded);`를 이 함수 호출로 바꿉니다. 초기값은 `session_locked_`가 `false`, `display_on_`이 `true`, `providers_active_`가 `true`입니다.

### 5-4. 컨텍스트 메뉴

`ShowContextMenu`에는 지금 `종료` 하나만 있습니다. 그 위에 네 항목과 구분선을 넣습니다.

```
배터리          (MF_CHECKED)
CPU
네트워크
위젯 보드 단추
──────────
종료
```

명령 식별자는 `kExitCommand`(1)과 겹치지 않게 10부터 13까지 씁니다. `WM_COMMAND`에서 해당 항목의 불리언을 뒤집어 `widgets_.SetSettings(next)`를 부릅니다. 체크 표시는 `widgets_.settings()`의 현재 값으로 붙입니다.

`PopupSurface`로 새로 만들지 말고 지금의 `HMENU`를 그대로 확장합니다. 계획 문서 7단계에서 항목 표시 설정 전체를 `PopupSurface`로 다시 만들 예정이며, 그때 이 네 항목도 함께 옮겨 갑니다. **이 메뉴가 임시라는 사실을 코드 주석에 한 줄로 남겨 주십시오.**

---

## 6. 내부 식별자 보호

지금 `StatusRegistry::Upsert`는 `id`가 같으면 어느 공급자가 올린 것이든 덮어씁니다. 그래서 파이프 클라이언트가 `bamti.widget/cpu`를 올리면 내장 위젯 항목을 그대로 가로챌 수 있습니다. 내부 식별자가 처음 생기는 이 단계에서 막습니다.

`PipeServer`가 `upsert`와 `patch`와 `remove`를 처리할 때 `id`가 `bamti.`으로 시작하면 그 줄을 버리고, 클라이언트당 한 번만 로그를 남깁니다. 연결을 끊지는 마십시오.

`docs/STATUS-PROTOCOL.md`의 식별자 규칙에 다음 문장을 더합니다.

> `bamti.`으로 시작하는 식별자는 bamti 내부 공급자가 예약합니다. 파이프 클라이언트가 이 접두를 쓰면 그 줄은 무시됩니다.

같은 문서에 내장 항목 표를 짧게 추가합니다. 식별자 네 개가 각각 무엇인지, 그리고 기본값이 전부 꺼짐이라는 사실을 적습니다.

---

## 7. 빌드 설정

`CMakeLists.txt`에 다음을 반영합니다.

- 원본 목록에 `src/settings.cpp`와 `src/widgets/builtin.cpp`를 추가합니다.
- `target_link_libraries`에 `iphlpapi`(`GetIfTable2`)와 `wtsapi32`(세션 알림)를 추가합니다.
- `bamti.vcxproj`에도 같은 원본 두 개를 반영합니다.

`/W4`에서 다음이 걸리기 쉽습니다. 미리 확인하십시오.

- `FILETIME`을 `ULARGE_INTEGER`로 옮길 때의 형 변환 경고
- `MIB_IF_ROW2`의 `ULONG64`를 `double`로 나눌 때의 축소 변환
- 쓰지 않는 콜백 매개변수. 다른 파일과 같이 `(void)param;`으로 처리합니다.

---

## 8. 하지 말아야 할 것

1. 위젯마다 스레드를 만들지 않습니다. 하나입니다.
2. `NtQuerySystemInformation`을 비롯한 비공개 API를 쓰지 않습니다.
3. UI 스레드에서 `ShellExecuteW`, `SendInput`, 파일 쓰기, 레지스트리 조회를 하지 않습니다.
4. 새 JSON 파서나 새 설정 저장소를 만들지 않습니다. `json_line.hpp`와 `settings.json` 하나입니다.
5. 새 팝업 인프라를 만들지 않습니다. 패널은 이미 있는 `PopupSurface` 경로를 그대로 씁니다.
6. 값이 바뀌지 않았는데 `Upsert`를 부르지 않습니다.
7. 위젯 기본값을 켜짐으로 두지 않습니다.
8. `status_item.hpp`의 상한 상수를 늘리거나 새로 만들지 않습니다.
9. 5단계 트레이 미러에 해당하는 코드를 미리 넣지 않습니다.

---

## 9. 검증

**빌드**
- [ ] `/W4` 경고 없이 Debug와 Release가 빌드됩니다.

**동작**
- [ ] 처음 실행하면 위젯이 하나도 보이지 않습니다.
- [ ] 우클릭 메뉴에서 세 위젯을 켜면 1초 안에 세 항목이 바에 나타나고, 체크 표시가 상태와 일치합니다.
- [ ] bamti를 껐다 켜도 그 선택이 유지됩니다.
- [ ] `settings.json`에 손으로 다른 키를 넣어 둔 뒤 메뉴로 위젯을 토글해도 그 키가 그대로 남아 있습니다.
- [ ] 각 항목을 클릭하면 패널이 열리고, 값이 실제 시스템 상태와 맞습니다. CPU는 작업 관리자와, 네트워크는 리소스 모니터와 비교합니다.
- [ ] 패널의 세 버튼이 각각 전원 설정, 네트워크 설정, 작업 관리자를 열고, 누르는 순간 바가 멈추지 않습니다.
- [ ] 보드 단추를 누르면 위젯 보드가 열리고, bamti 시작 메뉴는 열리지 않습니다.
- [ ] 배터리가 없는 컴퓨터에서는 배터리를 켜도 항목이 나타나지 않고 로그에 한 줄만 남습니다.

**절전**
- [ ] 세 위젯을 모두 켠 채 10분 유휴 상태에서 CPU 사용률이 0.2% 미만입니다. 측정은 다음과 같이 합니다.

  ```powershell
  $p = Get-Process bamti; $a = $p.TotalProcessorTime
  Start-Sleep -Seconds 600
  $p.Refresh(); $b = $p.TotalProcessorTime
  ($b - $a).TotalSeconds / 600 / [Environment]::ProcessorCount * 100
  ```

- [ ] 네 항목을 모두 끈 상태의 스레드 수가, 세 위젯을 켠 상태보다 정확히 하나 적습니다. `(Get-Process bamti).Threads.Count`로 확인합니다.
- [ ] 전체화면 게임에서 표본 추출이 멈추고, 나오면 다시 시작합니다. 로그로 확인합니다.
- [ ] `Win+L`로 잠그고 30분 뒤에 풀었을 때, CPU 사용률과 전송 속도가 튀지 않고 정상 범위에서 다시 시작합니다.
- [ ] 절전에서 복귀했을 때도 같습니다.

**회귀**
- [ ] `examples/ticker.py`와 `examples/status_push.py`가 그대로 동작합니다.
- [ ] 파이프 클라이언트가 `bamti.widget/cpu`로 `upsert`를 보내면 무시되고, 연결은 유지되며, 내장 항목이 그대로 남습니다.
- [ ] 항목이 많아지면 위젯도 오버플로 팝업에 정상적으로 접힙니다.
- [ ] 다크와 라이트, 100%와 150%와 200% DPI에서 정렬이 깨지지 않습니다.
- [ ] explorer를 강제 종료하고 다시 시작해도 위젯이 그대로 남아 있습니다.
- [ ] 위젯을 100번 켜고 끈 뒤 스레드 수와 핸들 수와 GDI 객체 수가 늘지 않습니다.

---

## 10. 커밋

| 순서 | 작업 | 커밋 메시지 |
|---|---|---|
| 1 | `settings.hpp` / `.cpp`와 빌드 설정 | `feat: 상단바 설정 파일에서 위젯 표시 여부를 읽는다` |
| 2 | `BuiltinWidgets`와 세 위젯, 보드 단추, 레지스트리 배선, 전원 알림 | `feat: 배터리, CPU, 네트워크 위젯을 추가한다` |
| 3 | 세션 잠금과 화면 꺼짐 연결 | `feat: 세션 잠금과 화면 꺼짐에서 공급자를 멈춘다` |
| 4 | 컨텍스트 메뉴 토글 | `feat: 상단바 메뉴에서 위젯 표시를 켜고 끈다` |
| 5 | 식별자 예약과 문서 | `docs: 내장 공급자 식별자 예약을 명세에 적는다` |

2번 커밋을 마친 시점에 빌드하고 실제로 띄워서 세 위젯이 나오는지 눈으로 확인한 뒤 3번으로 넘어가십시오. 이때는 `settings.json`을 손으로 고쳐 위젯을 켜면 됩니다. 메뉴는 4번에서 생깁니다. 이 확인 없이 진행하면, 나중에 값이 보이지 않을 때 원인이 표본 추출인지 메뉴 배선인지 가릴 수 없습니다.
