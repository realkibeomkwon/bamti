# 작업 지시서: 재시작한 뒤에 상단바가 작업 영역을 예약하지 못하는 문제를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp` 두 개입니다.

---

## 1. 증상과 지금까지 잰 값

재부팅 직후에 자동 시작으로 뜬 bamti 는 정상입니다. 바탕 화면 아이콘과 최대화한 창이 상단바 아래에서 시작합니다. 그런데 **bamti 를 종료했다가 다시 실행하면** 상단바와 바탕 화면 아이콘, 최대화한 창의 제목 표시줄이 겹칩니다.

2026-09-07 10:20 에 재본 값입니다. 그때 실행 중이던 bamti 는 09:26:13 에 시작한 인스턴스입니다.

```
Bounds  : {X=0,Y=0,Width=2560,Height=1080}
WorkArea: {X=0,Y=32,Width=2560,Height=1048}
```

**이 시점에는 정상이었습니다.** 작업 영역의 위쪽이 상단바 높이 32 픽셀만큼 밀려 있습니다. 즉 이 결함은 재시작할 때마다 반드시 나오는 것이 아니라 **조건이 맞을 때만 나옵니다.** 재현하지 못한 채 코드만 고치면 고쳤는지 알 수 없으므로, 2절의 재현을 먼저 수행하십시오.

---

## 2. 무엇이 원인이라고 보는가

`MenuBar::Init`(393행부터)의 시작 순서가 다음과 같습니다.

```cpp
Layout();                        // ABM_QUERYPOS 와 ABM_SETPOS 로 영역을 예약한다
taskbar_.Restore();              // 작업 표시줄의 자동 숨김을 푼다
ShowWindow(hwnd_, SW_SHOWNA);    // 이 줄이 실행되기 전까지 창은 보이지 않는다
ApplyBackdrop();
Present();
taskbar_.Hide();                 // 자동 숨김을 다시 건다
```

의심하는 지점이 두 가지입니다.

**첫째, 창이 보이지 않는 상태에서 영역을 예약합니다.** `Layout()` 이 `ShowWindow` 보다 먼저 실행됩니다. 상단바는 레이어드 창이므로 `Present()` 안의 `UpdateLayeredWindow` 까지 끝나야 실제로 화면에 나타납니다. explorer 가 보이지 않는 앱바의 영역을 작업 영역에서 빼지 않는다면, 이 시점의 `ABM_SETPOS` 는 효과가 없습니다.

**둘째, 그 뒤에 작업 표시줄의 상태를 두 번 바꿉니다.** `taskbar_.Restore()` 와 `taskbar_.Hide()` 가 각각 `ABM_SETSTATE` 로 자동 숨김을 껐다 켭니다. 작업 표시줄의 상태가 바뀌면 explorer 는 작업 영역을 다시 계산합니다. 그 재계산이 상단바의 예약을 덮으면 작업 영역이 모니터 전체로 되돌아갑니다.

**재부팅 직후에만 정상인 이유도 이것으로 설명됩니다.** 부팅 직후에는 explorer 가 셸을 초기화하면서 `WM_SETTINGCHANGE` 와 `ABN_POSCHANGED` 를 여러 번 보냅니다. 그때마다 `Layout()` 이 다시 돌기 때문에(1057행, 1111행) 결과적으로 예약이 복구됩니다. 재시작할 때에는 그런 메시지가 오지 않으므로 한 번 놓친 예약이 그대로 남습니다.

### 재현 절차

bamti 를 종료하고 다시 실행하기를 **다섯 번 반복**하면서, 매번 작업 영역을 재십시오. 종료와 실행은 사용자에게 부탁하지 말고 여러분이 프로세스를 다뤄도 됩니다. 다만 **오래 살아야 하는 프로세스는 `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽으므로 WMI 로 띄우십시오.**

```powershell
Add-Type -AssemblyName System.Windows.Forms
$s = [System.Windows.Forms.Screen]::PrimaryScreen
"{0} / {1}" -f $s.Bounds, $s.WorkingArea
```

`WorkingArea` 의 `Y` 가 0 으로 나오는 회차가 재현입니다. 다섯 번 안에 한 번도 재현되지 않으면 조건을 더 좁혀야 하므로, 다음을 함께 바꿔 가며 재시도하십시오.

- 최대화한 창을 하나 띄워 둔 채로 재시작한다.
- 종료한 뒤 곧바로(1초 안에) 다시 실행한다.
- 종료한 뒤 30초를 기다렸다가 실행한다.

---

## 3. 시작 순서를 바꾸고 예약을 확인한다

### 3-1. 작업 영역을 확인하는 함수를 더한다

`src/menu_bar.hpp` 의 비공개 구역에 다음을 선언하십시오.

```cpp
  void VerifyWorkArea(const wchar_t* phase);
```

`src/menu_bar.cpp` 의 `Layout()` 바로 아래에 정의하십시오.

```cpp
void MenuBar::VerifyWorkArea(const wchar_t* phase) {
  if (hwnd_ == nullptr || fullscreen_occluded_ || !appbar_registered_) {
    return;
  }
  const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (!GetMonitorInfoW(monitor, &info)) {
    return;
  }
  const LONG want = info.rcMonitor.top + BarHeightPx();
  const bool ok = info.rcWork.top >= want;
  Log(L"bar", L"workarea %s top=%ld want=%ld mon_top=%ld ok=%d", phase, info.rcWork.top, want, info.rcMonitor.top,
      ok ? 1 : 0);
  if (!ok) {
    Layout();
  }
}
```

**`SystemParametersInfoW(SPI_GETWORKAREA)` 를 쓰지 마십시오.** 그 호출은 주 모니터의 값만 돌려주므로 모니터가 여럿인 환경에서 틀린 판정을 내립니다. `GetMonitorInfoW` 의 `rcWork` 가 상단바가 붙어 있는 모니터의 값입니다.

### 3-2. 시작 순서를 바꾼다

`MenuBar::Init` 의 해당 부분을 다음과 같이 바꾸십시오.

```cpp
  taskbar_.Restore();
  ShowWindow(hwnd_, SW_SHOWNA);
  ApplyBackdrop();
  Present();
  taskbar_.Hide();
  Layout();
  VerifyWorkArea(L"init");
```

`Layout()` 을 `taskbar_.Restore()` 앞에서 **지우고**, 창이 실제로 그려진 뒤이자 작업 표시줄의 상태 변경이 모두 끝난 뒤로 옮기는 것입니다. `taskbar_.Restore()` 와 `taskbar_.Hide()` 사이의 다른 줄은 순서를 그대로 두십시오.

### 3-3. 뒤늦게 덮이는 경우에 대비한다

explorer 가 작업 영역을 다시 계산하는 시점이 우리 `Layout()` 보다 늦을 수 있습니다. 시작한 뒤 세 번 더 확인하십시오.

`src/menu_bar.cpp` 의 타이머 식별자들이 모여 있는 곳에 다음을 더하십시오.

```cpp
constexpr UINT kWorkAreaTimerId = <겹치지 않는 값>;
constexpr UINT kWorkAreaCheckMs = 700;
constexpr int kWorkAreaCheckMax = 4;
```

`Init` 끝부분의 `SetTimer` 들 옆에서 타이머를 걸고, `WM_TIMER` 처리에 다음 분기를 더하십시오.

```cpp
      if (wparam == kWorkAreaTimerId) {
        VerifyWorkArea(L"tick");
        if (++work_area_checks_ >= kWorkAreaCheckMax) {
          KillTimer(hwnd_, kWorkAreaTimerId);
        }
        return 0;
      }
```

`work_area_checks_` 는 `MenuBar` 의 `int` 멤버로 두고 0 으로 초기화하십시오. **네 번을 세면 타이머를 반드시 끄십시오.** 상시로 도는 타이머를 하나 더 늘리면 안 됩니다.

`WM_DESTROY` 와 `WM_ENDSESSION` 의 `KillTimer` 목록에도 `kWorkAreaTimerId` 를 더하십시오.

---

## 4. 검증

### 4-1. 빌드

Release 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

### 4-2. 작업 영역 (이것이 성공 판정입니다)

2절과 같은 방식으로 **종료와 재시작을 열 번 반복**하고 매번 작업 영역을 재십시오.

**판정 기준은 로그의 `ok=1` 이 아니라 실제로 잰 `WorkingArea` 입니다.** `Y` 가 32 여야 하며, 열 번 모두 32 여야 합니다. 한 번이라도 0 이 나오면 실패입니다.

로그의 `[bar] workarea` 줄도 함께 확인하십시오. `init` 에서 `ok=0` 이었다가 `tick` 에서 `ok=1` 로 바뀌었다면 3-2 의 순서 변경만으로는 부족하고 3-3 의 확인 타이머가 실제로 일하고 있다는 뜻이므로, 그 사실을 보고에 적으십시오.

### 4-3. 사람 눈으로 봐야 하는 것

**직접 스크린샷을 찍거나 입력을 합성하지 말고 사용자에게 부탁하십시오.**

- 바탕 화면 아이콘의 맨 윗줄이 상단바에 가리지 않는가.
- 창을 최대화했을 때 제목 표시줄이 상단바 아래에서 시작하는가.
- 재시작을 몇 번 반복해도 같은가.

### 4-4. 되돌아오는 것이 없는지 확인한다

- 상단바 자체의 위치와 크기가 그대로인가. `Layout()` 을 옮겼으므로 창이 잠깐 잘못된 자리에 나타났다가 옮겨질 수 있습니다. 시작할 때 상단바가 깜빡이거나 자리를 옮기는 것이 눈에 보이면 보고하십시오.
- 작업 표시줄이 자동 숨김으로 잘 들어갔는가. `[tray] hidden autohide=1` 로그를 확인하십시오.
- 전체 화면 앱을 띄웠다가 나왔을 때 상단바가 정상으로 돌아오는가.

---

## 5. 건드리지 말 것

- `TaskbarController` 의 `Restore`, `Hide`, `EnsureHidden` 을 바꾸지 마십시오. 이 지시서는 호출 순서만 다룹니다.
- `RegisterAppBar` 와 `UnregisterAppBar` 를 바꾸지 마십시오.
- `ABN_FULLSCREENAPP` 처리(1058행 부근)를 건드리지 마십시오. 그 주석에 적힌 대로, 검사 없이 앱바를 내리면 상단바가 깜빡입니다.
- `Layout()` 안의 `ABM_QUERYPOS` 와 `ABM_SETPOS` 순서를 바꾸지 마십시오.
- 레지스트리에 쓰지 마십시오. 작업 영역을 `SPI_SETWORKAREA` 로 직접 설정하지도 마십시오. 앱바 규약 밖에서 작업 영역을 건드리면 되돌리지 못합니다.
