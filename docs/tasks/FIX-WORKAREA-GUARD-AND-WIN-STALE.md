# 작업 지시서: 비정상 종료가 남기는 작업 영역과 굳은 Win 상태를 되돌린다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

2026-09-07 의 상단바 검수에서 발견했지만 그날 손대지 않고 미뤄 둔 위험 두 가지를 다룹니다. 서로 독립적이므로 **1부와 2부를 순서대로 하나씩 끝내십시오.** 둘을 섞어서 고치면 검증에서 어느 쪽이 회귀를 냈는지 가릴 수 없습니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, `src/paths.cpp`, `src/paths.hpp`, `src/host.cpp` 다섯 개입니다.

---

# 1부. 강제한 작업 영역을 비정상 종료 뒤에 되돌린다

## 1-1. 지금 코드가 하는 일

`MenuBar::ReserveWorkArea`(`src/menu_bar.cpp` 1289행)는 AppBar 규약으로 예약이 잡히지 않으면 작업 영역을 직접 강제합니다.

```cpp
    const LONG want = info.rcMonitor.top + BarHeightPx();
    if (info.rcWork.top < want) {
      RECT work = info.rcWork;
      work.top = want;  // ABM_SETPOS is often ignored; keep left/right/bottom.
      if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, SPIF_SENDCHANGE) != FALSE) {
        work_area_forced_ = true;
        GetMonitorInfoW(monitor, &info);
      }
    }
```

되돌리는 곳은 `MenuBar::UnregisterAppBar`(1237행) 하나뿐입니다.

```cpp
void MenuBar::UnregisterAppBar() {
  if (work_area_forced_) {
    work_area_forced_ = false;
    SystemParametersInfoW(SPI_SETWORKAREA, 0, nullptr, SPIF_SENDCHANGE);
  }
```

`UnregisterAppBar` 는 `WM_DESTROY` 와 `WM_ENDSESSION` 에서만 불립니다. 곧 **정상 종료 경로에만 복구가 걸려 있습니다.**

## 1-2. 무엇이 위험한가

프로세스가 예외로 죽거나, 작업 관리자에서 강제 종료되거나, `Stop-Process -Force` 로 끊기면 `work_area_forced_` 라는 기억이 프로세스와 함께 사라집니다. 그러면 **상단바가 사라진 뒤에도 화면 위쪽 32 픽셀이 예약된 채 남습니다.** 최대화한 창이 그 아래에서 시작하므로 사용자는 이유를 알 수 없는 빈 띠를 보게 됩니다.

`SPIF_UPDATEINIFILE` 을 쓰지 않으므로 레지스트리에는 기록되지 않습니다. explorer 를 다시 시작하거나 다시 로그온하면 복구됩니다. 그러나 **사용자가 그 사실을 알아야만 복구된다는 점이 결함입니다.**

`FIX-WORKAREA-ON-RESTART.md` 의 마지막 절은 "작업 영역을 `SPI_SETWORKAREA` 로 직접 설정하지 마십시오. 앱바 규약 밖에서 작업 영역을 건드리면 되돌리지 못합니다"라고 적었습니다. 그 뒤 커밋 `db33286` 이 AppBar 예약이 비는 실제 사례를 만나 이 호출을 넣었습니다. **호출을 되돌리지 마십시오.** 예약이 비는 문제가 다시 살아납니다. 이번 작업은 그때 남겨 둔 복구 장치를 채우는 것입니다.

## 1-3. 어떻게 고치는가

작업 표시줄 자동 숨김이 이미 같은 문제를 같은 방법으로 풀고 있습니다. `TaskbarController` 는 숨길 때 `taskbar.guard` 파일을 쓰고, 다음 실행의 `Restore()` 가 남아 있는 파일을 보고 되돌립니다(`src/taskbar_controller.cpp` 241행과 250행). **같은 규약을 그대로 따르십시오.** 새 방식을 만들지 마십시오.

### 1-3-1. 표식 파일의 경로를 더한다

`src/paths.hpp` 의 선언 목록에서 `TaskbarGuardPath()` 바로 아래에 더하십시오.

```cpp
std::wstring WorkAreaGuardPath();
```

`src/paths.cpp` 의 `TaskbarGuardPath()` 정의 바로 아래에 같은 모양으로 정의하십시오.

```cpp
std::wstring WorkAreaGuardPath() {
  const std::wstring dir = DataDir();
  if (dir.empty()) {
    return {};
  }
  return JoinPath(dir, L"workarea.guard");
}
```

`DataDir()` 안의 `MigrateFile` 목록에는 손대지 마십시오. 이 파일은 예전 경로에 존재한 적이 없으므로 옮길 것이 없습니다.

### 1-3-2. 표식을 쓰고 지우는 함수를 더한다

`src/menu_bar.cpp` 의 포함 목록에 `#include "paths.hpp"` 를 더하십시오. 지금은 포함되어 있지 않습니다. 알파벳 순서를 지켜 `#include "log.hpp"` 와 `#include "settings.hpp"` 사이에 넣으십시오.

익명 이름 공간 안, `InjectWinKey`(206행) 위쪽의 자유 함수들이 모인 자리에 다음을 더하십시오.

```cpp
// 작업 영역을 강제했다는 사실을 파일로 남긴다.
// 프로세스가 비정상 종료해도 이 파일은 남으므로, 다음 실행이 보고 되돌린다.
void WriteWorkAreaGuard() {
  const std::wstring path = WorkAreaGuardPath();
  if (path.empty()) {
    return;
  }
  const HANDLE file =
      CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return;
  }
  const char body[] = "v=1\n";
  DWORD written = 0;
  WriteFile(file, body, static_cast<DWORD>(sizeof(body) - 1), &written, nullptr);
  CloseHandle(file);
}

void DeleteWorkAreaGuard() {
  const std::wstring path = WorkAreaGuardPath();
  if (!path.empty()) {
    DeleteFileW(path.c_str());
  }
}

bool WorkAreaGuardExists() {
  const std::wstring path = WorkAreaGuardPath();
  return !path.empty() && GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}
```

파일 내용은 판정에 쓰지 않습니다. **존재 여부만 봅니다.** `v=1` 은 나중에 형식을 늘릴 때를 위한 자리이며, 읽는 코드를 지금 만들지 마십시오.

### 1-3-3. 강제할 때 표식을 쓴다

`ReserveWorkArea`(1289행)의 해당 부분을 다음과 같이 바꾸십시오.

```cpp
      if (SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, SPIF_SENDCHANGE) != FALSE) {
        if (!work_area_forced_) {
          work_area_forced_ = true;
          WriteWorkAreaGuard();
        }
        GetMonitorInfoW(monitor, &info);
      }
```

`if (!work_area_forced_)` 로 감싸는 이유가 있습니다. `ReserveWorkArea` 는 시작할 때뿐 아니라 앱바 알림과 설정 변경에서도 다시 불립니다(494행, 678행, 2739행). 조건 없이 쓰면 같은 파일을 되풀이해 만듭니다.

### 1-3-4. 되돌릴 때 표식을 지운다

`UnregisterAppBar`(1237행)의 앞부분을 다음과 같이 바꾸십시오.

```cpp
void MenuBar::UnregisterAppBar() {
  if (work_area_forced_) {
    work_area_forced_ = false;
    SystemParametersInfoW(SPI_SETWORKAREA, 0, nullptr, SPIF_SENDCHANGE);
  }
  DeleteWorkAreaGuard();
```

`DeleteWorkAreaGuard()` 를 `if` 밖에 두십시오. **앞선 실행이 남긴 표식도 정상 종료할 때 함께 정리되어야 합니다.**

### 1-3-5. 시작할 때 남아 있는 표식을 처리한다

익명 이름 공간 밖, `namespace bamti` 안의 `MenuBar` 멤버 정의보다 앞자리에 자유 함수를 정의하십시오.

```cpp
void ReleaseLeftoverWorkArea() {
  if (!WorkAreaGuardExists()) {
    return;
  }
  SystemParametersInfoW(SPI_SETWORKAREA, 0, nullptr, SPIF_SENDCHANGE);
  DeleteWorkAreaGuard();
  Log(L"bar", L"workarea guard released");
}
```

`src/menu_bar.hpp` 의 `namespace bamti {` 바로 아래, `class MenuBar` 선언보다 앞에 선언을 더하십시오.

```cpp
// 앞선 실행이 비정상 종료해 강제된 작업 영역이 남아 있으면 되돌린다.
void ReleaseLeftoverWorkArea();
```

`MenuBar::Create` 에서 `if (!RegisterAppBar()) {`(451행) **바로 앞**에 호출을 넣으십시오.

```cpp
  ReleaseLeftoverWorkArea();
  if (!RegisterAppBar()) {
```

이 자리인 이유는 두 가지입니다. 첫째, 우리 앱바가 아직 등록되기 전이므로 `SPI_SETWORKAREA(nullptr)` 의 재계산 결과가 explorer 자신의 상태만 반영합니다. 둘째, 그 뒤에 오는 `taskbar_.Restore()` 와 `taskbar_.Hide()` 와 `ReserveWorkArea()`(487행부터 494행)가 순서대로 다시 계산하므로, 정상 경로의 최종 상태는 달라지지 않습니다.

### 1-3-6. 복구 명령에도 연결한다

`src/host.cpp` 에는 이미 `--restore-taskbar` 분기가 있습니다(292행 부근). 비정상 종료 뒤에 사용자가 손으로 복구하는 통로이므로 작업 영역도 여기서 함께 풀어야 합니다.

```cpp
    Log(L"host", L"restore-taskbar");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    TaskbarController::ForceRestore();
    ReleaseLeftoverWorkArea();
```

`src/host.cpp` 는 이미 `menu_bar.hpp` 를 포함하고 있으므로 포함 목록은 바꾸지 않습니다.

## 1-4. 1부에서 하지 말 것

- `SPI_SETWORKAREA` 호출 자체를 없애지 마십시오. `db33286` 이 실제 결함을 보고 넣은 것입니다.
- `SPIF_UPDATEINIFILE` 을 붙이지 마십시오. 레지스트리에 남으면 explorer 재시작으로도 복구되지 않습니다.
- `TaskbarController` 의 `WriteGuard`, `ReadGuard`, `DeleteGuard` 를 고치거나 재사용하지 마십시오. 작업 표시줄 표식과 작업 영역 표식은 수명이 다르므로 파일도 따로 둡니다.
- 종료 처리기(`SetUnhandledExceptionFilter`, `SetConsoleCtrlHandler`)를 새로 달지 마십시오. 강제 종료는 어차피 잡지 못하므로 표식 방식이 유일하게 확실한 장치입니다.

---

# 2부. 굳은 Win 눌림 상태를 시간으로 만료시킨다

## 2-1. 어떻게 굳고, 왜 그 뒤가 위험한가

`LowLevelKeyboardProc`(235행)은 Win 누름을 삼키고 기억만 남깁니다.

```cpp
  if (is_win) {
    if (down) {
      g_win_held = true;
      g_win_combo = false;
      g_win_injected = false;
      g_win_vk = vk;
      return 1;
    }
```

`g_win_held` 를 거짓으로 돌리는 곳은 Win 뗌 이벤트와 `RemoveWinHook`(2280행) 두 곳뿐입니다. UAC 프롬프트나 화면 잠금으로 보안 데스크톱이 뜨면 그 사이의 입력이 우리 훅에 오지 않으므로 **뗌을 통째로 놓칩니다.**

굳은 뒤의 경로를 끝까지 따라가면 피해가 한 번으로 끝나지 않습니다.

1. 사용자가 아무 키나 누르면 288행의 `else if (g_win_held && down)` 갈래로 들어갑니다.
2. `g_win_combo` 가 거짓이므로 `InjectWinCombo(g_win_vk, *info)` 가 **Win 누름과 그 키를 함께 주입합니다.** `A` 를 누르면 시스템은 `Win+A` 를 받고 빠른 설정이 열립니다.
3. `InjectWinCombo` 는 Win 뗌을 주입하지 않습니다. 뗌은 진짜 Win 뗌이 왔을 때 274행의 갈래에서만 주입됩니다. **그런데 그 뗌은 이미 놓친 상태입니다.**
4. 곧 **시스템 수준에서 Win 키가 눌린 채로 남습니다.** 그 뒤로는 훅이 무엇을 하든 모든 키 입력이 Win 조합으로 해석됩니다.

3번과 4번이 이 결함의 핵심입니다. 고칠 때 **주입해 둔 Win 누름을 반드시 되돌려야 합니다.** 기억만 지우면 시스템에 남은 눌림은 그대로입니다.

## 2-2. 과거에 실패한 방법을 되살리지 마십시오

`FIX-WIN-HELD-STALE.md`(커밋 `ebc17f1`)는 `GetAsyncKeyState(VK_LWIN)` 로 실제 키 상태를 읽어 검증하는 방법을 지시했습니다. **그 방법은 실패했고 이미 제거되었습니다.** 훅이 Win 누름을 `return 1` 로 삼키기 때문에 운영 체제의 키 상태 표가 갱신되지 않고, 그래서 이 검사는 Win 을 실제로 누르고 있을 때에도 항상 거짓을 돌려줍니다. 결과적으로 정상적인 Win 조합키가 매번 깨졌습니다.

`GetAsyncKeyState` 나 `GetKeyState` 로 Win 상태를 확인하는 코드를 다시 넣지 마십시오.

## 2-3. 어떻게 고치는가

### 2-3-1. 눌린 시각을 기억한다

`g_win_held` 가 선언된 자리(124행 부근)에 더하십시오.

```cpp
ULONGLONG g_win_down_tick = 0;
bool g_win_expired = false;
UINT g_win_stale_count = 0;
UINT g_win_stale_logged = 0;
```

같은 파일의 상수 자리(34행 부근의 타이머 식별자 아래)에 더하십시오.

```cpp
// Win 을 누른 채 조합키를 치기까지 걸리는 시간은 길어야 몇 초다.
// 이 시간을 넘기면 뗌 이벤트를 놓친 것으로 보고 기억을 만료시킨다.
constexpr ULONGLONG kWinHeldMaxMs = 10000;
```

Win down 갈래에서 시각을 남기십시오. **처음 눌릴 때만 기록해야 합니다.**

```cpp
    if (down) {
      if (!g_win_held) {
        g_win_down_tick = GetTickCount64();
      }
      g_win_held = true;
```

키보드 자동 반복이 이 훅에 도달하는지는 아직 재지 않았습니다. `if (!g_win_held)` 로 감싸면 도달하든 하지 않든 만료 시각이 첫 누름에서 고정되므로, **자동 반복의 동작에 의존하지 않는 설계가 됩니다.**

### 2-3-2. 만료 함수를 더한다

`InjectWinCombo`(218행) 정의 바로 아래에 더하십시오.

```cpp
// 훅이 기억하는 Win 눌림을 풀고, 주입해 둔 Win 누름이 있으면 함께 되돌린다.
void ReleaseHeldWin() {
  if (g_win_combo && g_win_injected) {
    InjectWinKey(g_win_vk, true);
  }
  g_win_held = false;
  g_win_combo = false;
  g_win_injected = false;
}

// 뗌을 놓쳐 굳은 상태를 시간으로 판정한다. 만료시켰으면 참을 돌려준다.
bool ExpireStaleWin() {
  if (!g_win_held || GetTickCount64() - g_win_down_tick <= kWinHeldMaxMs) {
    return false;
  }
  ReleaseHeldWin();
  g_win_expired = true;
  ++g_win_stale_count;
  return true;
}
```

### 2-3-3. 훅에서 만료를 검사한다

검사 위치가 중요합니다. `LowLevelKeyboardProc` 안에서 **Ctrl 처리가 끝난 다음, `g_menu_bar == nullptr || ... || !g_menu_bar->win_key_enabled()` 로 빠져나가는 줄보다 앞**에 두십시오.

```cpp
  ExpireStaleWin();
  if (g_menu_bar == nullptr || g_menu_bar->hwnd() == nullptr || !g_menu_bar->win_key_enabled()) {
    return CallNextHookEx(g_key_hook, code, wparam, lparam);
  }
```

이 자리여야 하는 이유는 두 가지입니다. 첫째, Win 을 누르고 있는 동안 전체 화면 앱이 뜨면 `win_key_enabled()` 가 거짓이 되어 그 아래 코드가 아예 실행되지 않습니다. 그 경로에서도 굳은 상태가 풀려야 합니다. 둘째, `is_win` 판정과 288행의 조합 갈래보다 앞이므로 굳은 기억이 그 아래의 모든 판단에 섞이지 않습니다.

`ExpireStaleWin` 은 `g_win_held` 가 참일 때만 시각을 비교하므로 평소에는 비교 한 번으로 끝납니다.

### 2-3-4. 만료 뒤에 오는 진짜 뗌을 흘려보낸다

만료시킨 뒤 진짜 Win 뗌이 뒤늦게 도착하면 지금 코드는 `g_win_combo` 가 거짓이라는 이유로 시작 메뉴를 엽니다. 사용자가 누르지 않은 시작 메뉴가 열리므로 막아야 합니다. Win up 갈래(272행)의 맨 앞에 더하십시오.

```cpp
    if (up) {
      if (g_win_expired) {
        g_win_expired = false;
        return 1;
      }
      g_win_held = false;
```

### 2-3-5. 화면 잠금과 잠금 해제에서 즉시 푼다

10초 만료는 어떤 원인이든 막는 마지막 장치이지만, 화면 잠금은 원인을 정확히 알 수 있으므로 기다릴 이유가 없습니다. `WM_WTSSESSION_CHANGE` 처리(1160행)를 다음과 같이 바꾸십시오.

```cpp
    case WM_WTSSESSION_CHANGE:
      if (wparam == WTS_SESSION_LOCK) {
        session_locked_ = true;
        ReleaseHeldWin();
        UpdateProviderActive();
      } else if (wparam == WTS_SESSION_UNLOCK) {
        session_locked_ = false;
        ReleaseHeldWin();
        UpdateProviderActive();
      }
      return 0;
```

`ReleaseHeldWin` 은 익명 이름 공간의 자유 함수이고 창 절차는 같은 파일에 있으므로 그대로 부를 수 있습니다. 저수준 키보드 훅은 훅을 설치한 스레드에서 돌고 창 절차도 같은 스레드이므로 경쟁 상태는 없습니다.

### 2-3-6. 만료 횟수를 훅 밖에서 남긴다

**훅 안에서는 절대 로그를 부르지 마십시오.** `FIX-HOOK-LOG-FLOOD.md` 에서 확인한 대로 `Log` 는 전역 뮤텍스와 파일 쓰기를 하고, 저수준 키보드 훅은 `LowLevelHooksTimeout`(기본 300밀리초) 안에 반환하지 못하면 시스템에 의해 제거됩니다.

대신 이미 1초마다 도는 시계 타이머에서 횟수가 바뀌었을 때만 남기십시오. `WM_TIMER` 의 `kClockTimerId` 갈래(553행) 끝에 더하십시오.

```cpp
      if (wparam == kClockTimerId) {
        ...
        RefreshOpenPanel();
        if (g_win_stale_count != g_win_stale_logged) {
          g_win_stale_logged = g_win_stale_count;
          Log(L"bar", L"win stale expired count=%u", g_win_stale_count);
        }
      }
```

**이 줄이 2부의 유일한 측정 수단입니다.** 없으면 만료가 실제로 일어났는지 확인할 방법이 없습니다.

### 2-3-7. 훅을 뗄 때 함께 초기화한다

`RemoveWinHook`(2280행)의 초기화 목록에 더하십시오.

```cpp
  g_win_down_tick = 0;
  g_win_expired = false;
```

`g_win_stale_count` 와 `g_win_stale_logged` 는 **초기화하지 마십시오.** 프로세스가 사는 동안의 누적값이어야 합니다.

## 2-4. 2부에서 하지 말 것

- `kWinHeldMaxMs` 값을 근거 없이 바꾸지 마십시오. 짧게 잡으면 Win 을 오래 누르고 있던 사용자의 조합키가 무시되고, 길게 잡으면 굳은 뒤 피해가 이어지는 구간이 길어집니다. 바꿀 근거가 생기면 값이 아니라 그 근거를 보고하십시오.
- Win 키 처리의 의도를 바꾸지 마십시오. Win 단독 누름으로 시작 메뉴가 열리고, `Win+Space` 로 Spotlight 가 열리며, 그 밖의 Win 조합은 셸로 전달되어야 합니다.
- `Ctrl` 처리(249행부터)를 건드리지 마십시오. `kCornerWatchMsg` 를 보내는 경로입니다.
- `InjectWinCombo` 가 키 뗌을 주입하지 않는 것은 원래 설계입니다. 원본 키 뗌은 훅을 그대로 통과하므로 그대로 두십시오.

---

# 3. 검증

## 3-1. 빌드

Release 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

**빌드하려고 실행 중인 bamti 를 종료했다면, 끝난 뒤 반드시 다시 띄워 놓으십시오.** 오래 살아야 하는 프로세스를 `Start-Process` 로 띄우면 여러분의 세션이 끝날 때 함께 죽습니다. WMI 로 띄우십시오.

```powershell
([wmiclass]'Win32_Process').Create('<빌드 디렉터리>\Release\bamti.exe')
```

## 3-2. 작업 영역을 재는 방법

```powershell
Add-Type -AssemblyName System.Windows.Forms
$s = [System.Windows.Forms.Screen]::PrimaryScreen
"{0} / {1}" -f $s.Bounds, $s.WorkingArea
```

표식 파일은 `$env:USERPROFILE\.bamti\workarea.guard` 입니다.

## 3-3. 1부의 성공 판정

아래를 순서대로 수행하고 각 단계의 측정값을 보고에 적으십시오.

1. bamti 를 실행하고 로그에서 `[bar] workarea ... forced=` 값을 확인합니다. **`forced=0` 이면 이 환경에서는 강제 경로를 타지 않은 것이므로 2번부터는 판정할 수 없습니다.** 그때는 그 사실을 보고하고 3-4 로 넘어가십시오.
2. `forced=1` 이면 표식 파일이 존재해야 합니다.
3. 상단바 메뉴로 **정상 종료**합니다. 표식 파일이 사라지고 `WorkingArea` 의 `Y` 가 0 으로 돌아와야 합니다.
4. 다시 실행한 뒤 `Stop-Process -Force` 로 **강제 종료**합니다. 표식 파일이 남아 있고 `Y` 는 32 인 채로 남습니다. 상단바가 없는데 띠만 남은 상태이며, 이것이 고치려는 증상입니다.
5. `bamti.exe --restore-taskbar` 를 실행합니다. `Y` 가 0 으로 돌아오고 표식 파일이 사라지며 로그에 `[bar] workarea guard released` 가 남아야 합니다.
6. 4번을 다시 만든 뒤 이번에는 bamti 를 **정상 실행**합니다. 로그에 `workarea guard released` 가 먼저 남고 그 뒤에 `workarea ... forced=1` 이 남아야 하며, 최종 `Y` 는 32 여야 합니다.

**판정 기준은 로그가 아니라 `WorkingArea` 의 실측값입니다.**

## 3-4. 2부의 성공 판정

**이 환경에서는 키보드 입력을 합성할 수 없습니다. 사용자 화면에 입력을 밀어 넣으려고 하지 마십시오.** 아래 항목은 코드 경로를 설명한 뒤 사용자 확인으로 넘기십시오.

먼저 회귀가 없는지 확인할 항목입니다.

1. Win 단독 누름으로 시작 메뉴가 열리는가.
2. `Win+Space` 로 Spotlight 가 열리는가.
3. `Win+E` 와 `Win+R` 과 `Win+방향키` 가 그대로 동작하는가.
4. `Ctrl+C` 와 `Ctrl+V` 와 `Alt+V` 가 그대로인가.

다음은 고친 동작을 확인하는 재현 절차이며, **사용자에게 부탁해야 합니다.**

5. Win 키를 누른 채로 `L` 을 눌러 화면을 잠급니다. 이때 Win 뗌은 잠금 화면이 가져가므로 우리 훅에 오지 않습니다.
6. 잠금을 풀고 아무 키나 누릅니다. **빠른 설정이나 다른 Win 조합 기능이 열리면 실패입니다.** 평범하게 그 글자가 입력되어야 합니다.
7. 고치기 전 빌드에서 5번과 6번을 하면 증상이 재현되는지도 함께 확인하면 좋습니다. 재현되지 않으면 원인이 다른 곳일 수 있으므로 그 사실을 보고하십시오.

로그의 `[bar] win stale expired count=` 는 10초 만료 장치가 실제로 일했을 때에만 남습니다. 5번과 6번은 세션 알림 경로(2-3-5)가 먼저 처리하므로 이 줄이 남지 않는 것이 정상입니다. **두 장치를 헷갈리지 말고, 어느 쪽이 일했는지 보고에 구분해 적으십시오.**

## 3-5. 함께 확인할 것

- 훅 안에 `Log` 호출이 없는지 코드로 확인하십시오.
- 전체 화면 앱에 들어갔다 나온 뒤 Win 키 동작이 정상인지 확인하십시오.
- 상단바의 위치와 크기, 작업 표시줄 자동 숨김(`[tray] hidden autohide=1`)이 그대로인지 확인하십시오.

---

# 4. 공통으로 건드리지 말 것

- 레지스트리에 쓰는 확인 절차를 넣지 마십시오. 되돌리지 못하고 사용자 설정을 날린 적이 있습니다.
- `Layout()` 안의 `ABM_QUERYPOS` 와 `ABM_SETPOS` 순서를 바꾸지 마십시오.
- `ABN_FULLSCREENAPP` 처리를 건드리지 마십시오.
- `TaskbarController` 의 `Restore`, `Hide`, `EnsureHidden` 을 바꾸지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.

---

# 5. 고치지 말고 보고만 할 것

작업 중에 확인했지만 이번 범위가 아닌 것이 하나 있습니다.

Win down 갈래는 눌림 이벤트마다 `g_win_combo` 와 `g_win_injected` 를 거짓으로 되돌립니다. 키보드 자동 반복이 이 훅에 도달한다면, `Win+E` 를 친 뒤 Win 을 계속 누르고 있는 동안 반복 이벤트가 이 두 값을 지워서 주입해 둔 Win 누름이 되돌려지지 않을 수 있습니다. **자동 반복이 실제로 도달하는지 아직 재지 않았습니다.** 이번 작업에서 고치지 말고, 2부를 검증하다가 관련 증상을 보면 그 사실만 보고하십시오.
