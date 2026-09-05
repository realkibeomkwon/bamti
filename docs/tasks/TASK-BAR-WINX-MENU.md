# 작업 지시서: 시작 단추 우클릭에 Windows 빠른 링크 메뉴를 붙인다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-MENU-STYLE.md`와 `TASK-BAR-MENU-ITEMS.md` 다음에 하십시오. **순서를 지켜야 합니다.** 이 메뉴는 그 두 지시서가 만든 공용 그리기와 여러 서브메뉴 지원 위에서 돕니다.

건드리는 파일은 새로 만드는 `src/winx_menu.hpp`와 `src/winx_menu.cpp`, 그리고 `src/menu_bar.cpp`, `src/menu_bar.hpp`, `CMakeLists.txt`, `bamti.vcxproj`입니다.

---

## 1. 무엇을 만드는가

Windows 작업 표시줄의 시작 단추를 우클릭하면 뜨는 빠른 링크 메뉴(Win+X 메뉴)를 bamti 상단바의 시작 단추에도 붙입니다. **항목과 동작은 Windows와 같게, 생김새는 bamti 메뉴 규격으로** 만듭니다.

지금은 상단바 어디를 우클릭해도 `MenuBar::ShowContextMenu`가 뜹니다. `WM_RBUTTONUP` 처리부에 `HitStart(pt)` 검사가 없어서 시작 단추도 예외가 아닙니다.

---

## 2. 항목은 어디에서 오는가 (실측)

Windows는 이 메뉴를 `%LOCALAPPDATA%\Microsoft\Windows\WinX`의 바로 가기에서 만듭니다. 이 컴퓨터를 확인한 결과입니다.

```
Group1/  1 - Desktop.lnk                      desktop.ini
Group2/  1 - Run.lnk  2 - Search.lnk  3 - Windows Explorer.lnk
         4 - Control Panel.lnk  5 - Task Manager.lnk        desktop.ini
Group3/  01a - Windows PowerShell.lnk  02a - Windows PowerShell.lnk
         03 - Computer Management.lnk  04 - Disk Management.lnk
         04-1 - NetworkStatus.lnk      05 - Device Manager.lnk
         06 - SystemAbout.lnk          07 - Event Viewer.lnk
         08 - PowerAndSleep.lnk        09 - Mobility Center.lnk
         10 - AppsAndFeatures.lnk      desktop.ini
```

파일 이름은 영어인데 화면에는 한국어로 나옵니다. **번역은 각 그룹의 `desktop.ini`에 들어 있습니다.**

```ini
[LocalizedFileNames]
10 - AppsAndFeatures.lnk=@%SystemRoot%\system32\shell32.dll,-33248
09 - Mobility Center.lnk=@%SystemRoot%\system32\mblctr.exe,-1002
08 - PowerAndSleep.lnk=@%SystemRoot%\system32\powercpl.dll,-1
...
```

`@경로,-번호` 형식은 `SHLoadIndirectString`(`shlwapi.h`, `Shlwapi.lib`)이 그대로 받습니다. 환경 변수도 이 함수가 펼쳐 줍니다. 별도로 `ExpandEnvironmentStrings`를 부르지 마십시오.

**표시 순서는 Group3 → Group2 → Group1이고, 그룹 안에서는 파일 이름의 내림차순입니다.** 그룹과 그룹 사이에 구분선이 들어갑니다.

참고 화면은 `C:\Users\KIBEOMKWON\Pictures\Screenshots\Screenshot-2026-09-05_23-11-26.png`(Windows 작업 표시줄의 빠른 링크 메뉴)입니다. 이 화면의 순서가 위 규칙과 정확히 맞습니다.

---

## 3. Windows가 손대는 두 자리

파일을 그대로 읽으면 화면과 두 곳이 다릅니다.

1. **`01a`와 `02a`의 이름.** `desktop.ini`는 둘 다 `powershell.exe,-109`, 곧 `Windows PowerShell`을 가리키는데 화면에는 `터미널`과 `터미널(관리자)`로 나옵니다. Windows 11이 기본 콘솔 호스트가 Windows Terminal일 때 이름을 바꿔 보여 주기 때문입니다.

   `SearchPathW`로 `wt.exe`를 찾을 수 있으면 두 항목의 이름만 `터미널`과 `터미널(관리자)`로 바꾸십시오. **여는 대상은 바꾸지 마십시오.** 바로 가기를 그대로 실행하면 Windows가 알아서 Terminal 안에서 엽니다.

   둘 중 어느 쪽이 관리자인지 **파일 이름으로 짐작하지 말고 측정하십시오.** `IShellLink`를 `IShellLinkDataList`로 질의해 `GetFlags`의 `SLDF_RUNAS_USER` 비트를 봅니다. 결과를 `Log(L"winx", ...)`로 남기십시오.

2. **`4 - Control Panel.lnk`.** 파일 크기가 1.5KB로 나머지 바로 가기(1.0KB 안팎)보다 크고, 화면에는 `제어판`이 아니라 `설정`으로 나옵니다. 이 자리에 무엇이 나오는지 확정하지 못했습니다. **`SHLoadIndirectString`이 돌려준 문자열을 로그로 찍고, 나온 값을 그대로 쓰십시오.** 화면과 다르면 그때 다시 판단합니다. 임의로 `설정`이라고 적어 넣지 마십시오.

이름을 못 얻은 항목은 파일 이름에서 확장자와 앞의 번호를 떼어 낸 부분(`10 - AppsAndFeatures.lnk` → `AppsAndFeatures`)을 씁니다. 그 경우에도 로그를 남기십시오.

---

## 4. Windows가 덧붙이는 두 자리

`종료 또는 로그아웃`과 그 아래 `데스크톱`은 화면에 있지만 `WinX` 폴더에는 `데스크톱`밖에 없습니다. `종료 또는 로그아웃`은 Explorer가 만들어 끼워 넣는 서브메뉴입니다.

**Group1 앞에 이 서브메뉴를 직접 넣으십시오.** 결과는 이렇게 됩니다.

```
(Group3 열한 줄)
──────────────
(Group2 다섯 줄)
──────────────
종료 또는 로그아웃    ▸
데스크톱
```

서브메뉴 내용과 동작입니다.

| 항목 | 구현 |
| --- | --- |
| 로그아웃 | `ExitWindowsEx(EWX_LOGOFF, SHTDN_REASON_MAJOR_OTHER \| SHTDN_REASON_FLAG_PLANNED)` |
| 절전 | `SetSuspendState(FALSE, FALSE, FALSE)` (`powrprof.dll`, `PowrProf.lib`) |
| 최대 절전 모드 | `SetSuspendState(TRUE, FALSE, FALSE)` |
| 시스템 종료 | `ExitWindowsEx(EWX_SHUTDOWN \| EWX_POWEROFF, ...)` |
| 다시 시작 | `ExitWindowsEx(EWX_REBOOT, ...)` |

`시스템 종료`와 `다시 시작`과 `로그아웃`은 `SE_SHUTDOWN_NAME` 권한이 필요합니다. `OpenProcessToken` → `LookupPrivilegeValue` → `AdjustTokenPrivileges`로 켠 뒤에 부르고, 각 단계의 실패를 로그로 남기십시오.

`최대 절전 모드`는 켜져 있지 않은 컴퓨터가 많습니다. `GetPwrCapabilities`(`SYSTEM_POWER_CAPABILITIES`)의 `SystemS4`와 `HiberFilePresent`가 모두 참일 때만 행을 넣으십시오. 둘 중 하나라도 거짓이면 행을 아예 만들지 않습니다.

### 이 절의 동작은 실제로 실행해서 검증하지 마십시오

전원 명령을 확인하겠다고 컴퓨터를 끄거나 다시 시작하거나 로그아웃하면 작업 중인 것이 전부 날아갑니다. **다음 안전장치를 넣고 그것으로만 확인하십시오.**

환경 변수 `BAMTI_POWER_DRYRUN`이 `1`이면, 권한 조정까지는 실제로 하되 마지막 API 호출 직전에 다음을 남기고 돌아옵니다.

```cpp
Log(L"winx", L"power dryrun action=%s privilege=%d", name, privilege_ok);
```

검증은 이 변수를 켠 채로 합니다. 안전장치를 뺀 실제 동작 확인은 사용자가 직접 합니다.

---

## 5. 새 모듈

`src/winx_menu.hpp`에 다음을 둡니다.

```cpp
#pragma once

#include <string>
#include <vector>

namespace bamti {

struct WinXEntry {
  std::wstring label;
  std::wstring lnk_path;    // 비어 있으면 아래 action이 쓰인다
  int group = 0;            // 3, 2, 1
  bool admin = false;
};

// Group3 → Group2 → Group1 순으로, 그룹 안에서는 파일 이름 내림차순.
// 실패하면 빈 벡터를 돌려준다.
std::vector<WinXEntry> LoadWinXEntries();

enum class PowerAction { kLogoff, kSleep, kHibernate, kShutdown, kRestart };

bool HibernateAvailable();
void InvokePowerAction(PowerAction action);

// 바로 가기를 그대로 실행한다.
void LaunchWinXEntry(const WinXEntry& entry);

}  // namespace bamti
```

`LaunchWinXEntry`는 `ShellExecuteW(nullptr, nullptr, entry.lnk_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL)` 하나로 끝냅니다. 바로 가기가 대상과 승격 여부를 이미 들고 있으므로 대상을 풀어서 다시 만들지 마십시오. `SLDF_RUNAS_USER`가 켜진 바로 가기는 이 호출만으로 승격 창이 뜹니다. **뜨는지 확인하십시오.** 안 뜨면 그때 `runas` 동사를 붙입니다.

COM이 필요하므로 호출 스레드의 초기화 상태를 확인하고, 필요하면 `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`를 겁니다. 상단바 창 절차가 도는 스레드가 이미 초기화되어 있는지 먼저 코드로 확인하십시오.

`LoadWinXEntries`가 빈 벡터를 돌려주면(폴더가 없거나 읽지 못하면) **빠른 링크 메뉴를 띄우지 말고 지금의 `ShowContextMenu`를 그대로 띄우십시오.** 빈 메뉴가 뜨는 것보다 낫습니다. 이 갈래로 빠졌다는 사실을 로그에 남기십시오.

---

## 6. 상단바에 잇기

`src/menu_bar.hpp`에 `void ShowStartContextMenu(POINT screen);`를 더합니다.

`WM_RBUTTONUP` 처리부의 **맨 앞**에 다음을 넣습니다. `HitClock`보다 앞이어야 합니다.

```cpp
if (HitStart(pt)) {
  ClientToScreen(hwnd_, &pt);
  ShowStartContextMenu(pt);
  return 0;
}
```

`ShowStartContextMenu`는 `ShowContextMenu`와 같은 뼈대를 씁니다.

1. `fullscreen_occluded_`면 그대로 돌아갑니다.
2. 시작 메뉴가 떠 있으면(`start_menu_.visible()`) 닫습니다.
3. `bar_menu_->Reset(hwnd_, dark_)` 뒤에 5절의 항목을 채웁니다.
4. 명령 번호는 기존 값과 겹치지 않게 `constexpr UINT kWinXCmdBase = 5000;`부터 씁니다. `kTrayItemCmdBase = 4000`과 `kTrayHiddenKeysMax = 64`가 4063까지 쓰므로 5000이면 안전합니다. 전원 항목은 `kPowerCmdBase = 5200`부터 씁니다.
5. 항목 목록은 `std::vector<WinXEntry> winx_entries_`에 들고 있다가 `WM_COMMAND`에서 첨자로 찾습니다. 트레이 목록이 `tray_menu_keys_`를 쓰는 방식과 같습니다.
6. 여는 위치는 시작 단추 아래입니다. `StartRect()`를 화면 좌표로 옮겨 왼쪽 아래 모서리를 `PopupSurface::Anchor::BelowAt`에 넘기십시오. 커서 위치가 아니라 **단추에 붙여야** Windows와 같아집니다.
7. `cc_open_`, `clock_open_`, `open_panel_id_`를 정리하는 세 줄은 `ShowContextMenu`와 똑같이 넣습니다. 빠뜨리면 제어 센터가 열린 것으로 남습니다.
8. `SetAfterTick`에는 `TASK-BAR-MENU-ITEMS.md`가 만든 `AfterBarPopupTick`을 그대로 넘깁니다. `종료 또는 로그아웃` 서브메뉴가 이 경로로 열립니다.

---

## 7. 검증

1. **빌드.** Release 클린 빌드가 경고 없이 통과해야 합니다.
2. **항목 대조.** 상단바 시작 단추를 우클릭한 화면과 Windows 작업 표시줄에서 Win+X를 누른 화면을 나란히 찍으십시오. 항목의 **이름과 순서와 구분선 위치가 모두 같아야 합니다.** 기대값은 위에서 아래로 다음과 같습니다.

   설치된 앱 · 모바일 센터 · 전원 옵션 · 이벤트 뷰어 · 시스템 · 장치 관리자 · 네트워크 연결 · 디스크 관리 · 컴퓨터 관리 · 터미널 · 터미널(관리자) · ─── · 작업 관리자 · 설정 · 파일 탐색기 · 검색 · 실행 · ─── · 종료 또는 로그아웃 ▸ · 데스크톱

3. **생김새.** 강조 칠, 체크, 꺾쇠, 모서리가 독 우클릭 메뉴와 같아야 합니다. Windows 메뉴의 생김새를 따라가면 안 됩니다.
4. **여는 동작.** `작업 관리자`, `장치 관리자`, `설정`, `실행`을 각각 눌러 실제로 열리는지 확인하십시오.
5. **승격.** `터미널(관리자)`를 누르면 사용자 계정 컨트롤 창이 떠야 합니다. 승인은 사용자가 합니다.
6. **전원 서브메뉴.** `BAMTI_POWER_DRYRUN=1`을 켜고 다섯 항목을 눌러, 각각 로그에 알맞은 `action`과 `privilege=1`이 찍히는지 보십시오. **변수를 끄고 누르지 마십시오.**
7. **최대 절전.** 이 컴퓨터에서 항목이 나오는지 나오지 않는지와 `GetPwrCapabilities`의 값을 함께 로그에 남기십시오.
8. **회귀.** 상단바의 빈 곳과 위젯 위를 우클릭하면 여전히 기존 메뉴가 떠야 합니다. 시작 단추에서만 새 메뉴가 떠야 합니다.

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
