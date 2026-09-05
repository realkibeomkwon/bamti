# 작업 지시서 15: 로그인 시 열기의 명령 생성과 상태 판정을 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`로그인 시 열기`는 `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`에 값을 만들고 지웁니다. 그 부분은 동작합니다. 결함은 **무엇을 쓰느냐**와 **무엇을 보고 체크를 켜느냐**입니다.

---

## 1. 확인된 사실

사용자 환경의 실제 레지스트리입니다.

```
KakaoTalk                   : "C:\Program Files\Kakao\KakaoTalk\KakaoTalk.exe" -bystartup
bamti-dock-SnippingTool.exe : "C:\Program Files\WindowsApps\Microsoft.ScreenSketch_11.2605.36.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe"
```

위치와 형식은 동작하는 항목(`KakaoTalk`)과 같습니다. `StartupApproved\Run`에 우리 값의 비활성 기록도 없습니다.

**그러므로 "값이 안 만들어진다"는 문제가 아닙니다.** 아래 두 가지가 문제입니다.

---

## 2. 결함 1: 명령을 만드는 근거가 실행 경로와 다르다

### 2.1 현재 코드

`LoginRunCommand`(dock.cpp:736)입니다.

```cpp
std::wstring LoginRunCommand(const DockApp& app) {
  if (!app.exe_path.empty()) {
    if (app.exe_path.find(L'"') != std::wstring::npos) return {};
    return L"\"" + app.exe_path + L"\"";
  }
  if (!app.aumid.empty() && app.aumid.find(L'"') == std::wstring::npos) {
    return L"explorer.exe shell:AppsFolder\\" + app.aumid;
  }
  return {};
}
```

`exe_path`가 있으면 **무조건** 그것을 씁니다.

### 2.2 무엇이 빠졌는가

`LaunchDockApp`(task_list.cpp:1110)은 앱을 열 때 이 순서로 시도합니다.

```
1. 호스팅된 웹앱이면 AUMID 우선, 실패 시 relaunch_command
2. AUMID 가 있고 exe_path 가 비었거나 호스트 exe 면 AUMID
3. relaunch_command
4. 호스트 exe 가 아닌 exe_path
5. AUMID
```

`LoginRunCommand`는 이 중 **`relaunch_command`를 한 번도 쓰지 않습니다.** `relaunch_command`는 앱이 `PKEY_AppUserModel_RelaunchCommand`로 스스로 선언한 재실행 명령이며, 창에서 읽어 `DockApp`에 이미 담겨 있습니다(task_list.cpp:396). 앱이 "나를 다시 열려면 이렇게 하라"고 알려 준 값을 무시하고 원시 경로를 쓰는 것이 결함입니다.

### 2.3 먼저 측정하십시오 (필수, 선행)

`WindowsApps` 아래의 패키지 앱을 실행 파일 경로로 직접 여는 것이 **실제로 되는지 안 되는지 확정되지 않았습니다.** 추측하지 말고 재십시오. 재부팅은 필요 없습니다.

레지스트리에 들어간 명령 문자열을 **그대로** 실행해 보면 로그인 시의 동작을 확인할 수 있습니다.

```powershell
# 값에 들어 있는 문자열을 그대로 실행한다
& "C:\Program Files\WindowsApps\Microsoft.ScreenSketch_...\SnippingTool\SnippingTool.exe"
```

다음 세 가지를 각각 시험하고 결과를 보고하십시오.

1. 일반 앱(예: 탐색기, 터미널)의 `exe_path`
2. 패키지 앱(캡처 도구)의 `WindowsApps` 경로
3. 같은 패키지 앱의 `explorer.exe shell:AppsFolder\<AUMID>` 형태

**각각 창이 실제로 떴는지, 오류가 났다면 그 문구를 그대로 적으십시오.** 이 결과가 2-4의 설계를 정합니다.

### 2.4 명령 생성을 실행 경로와 같은 기준으로 맞춘다 (필수)

`LoginRunCommand`가 `LaunchDockApp`과 **같은 우선순위**로 명령 문자열을 만들게 하십시오.

- `LaunchDockApp`의 판단(`LooksLikeHostedWebApp`, `IsHostExe`, `relaunch_command` 우선순위)을 그대로 따르십시오.
- 두 곳에 같은 규칙을 복사해 두지 마십시오. **판단을 한 함수로 뽑아 양쪽이 쓰게 하십시오.** `task_list.cpp`에 `std::wstring DockLaunchCommandLine(const DockApp&)` 같은 함수를 만들고, `LaunchDockApp`과 `LoginRunCommand`가 모두 그것을 근거로 삼는 형태가 적절합니다.
- **`LaunchDockApp`의 동작을 바꾸지 마십시오.** 지금 클릭으로 앱을 여는 것은 잘 되고 있습니다. 판단을 밖으로 꺼내되 결과는 같아야 합니다. 리팩토링 전후로 클릭 실행이 동일하게 동작하는지 확인하십시오.
- 2-3의 측정에서 `WindowsApps` 직접 실행이 실패한 앱이 있으면, 그런 앱에는 AUMID 형태를 쓰십시오.
- 명령을 만들 수 없으면 `로그인 시 열기` 행을 **아예 넣지 마십시오.** 지금처럼 빈 문자열을 돌려주고 행도 빠지는 동작을 유지합니다.

### 2.5 값 이름의 충돌 (필수)

`LoginRunValueName`(dock.cpp:717)은 실행 파일 **이름만** 씁니다. 서로 다른 폴더에 같은 이름의 실행 파일이 있으면 값 이름이 겹칩니다.

`bamti-dock-` 뒤에 붙이는 부분을 앱을 구분할 수 있는 값으로 바꾸십시오. AUMID가 있으면 AUMID를, 없으면 전체 경로를 근거로 만드십시오. 레지스트리 값 이름에 쓸 수 없는 문자는 지금처럼 치환하고, 너무 길면 앞부분과 짧은 해시를 조합하십시오.

**기존 값 이름과 달라지면 이미 만들어 둔 항목이 고아가 됩니다.** 다음을 지키십시오.

- 상태를 읽을 때는 **새 이름과 옛 이름을 모두** 확인합니다.
- 해제할 때는 **둘 다** 지웁니다.
- 새로 만들 때는 새 이름만 씁니다.

---

## 3. 결함 2: 체크 표시가 실제 상태와 다를 수 있다

### 3.1 현재 코드

`LoginValueExists`(dock.cpp:748)는 `Run` 키에 값이 있는지만 봅니다.

### 3.2 무엇이 빠졌는가

사용자가 작업 관리자나 설정의 시작 프로그램에서 그 항목을 **사용 안 함**으로 바꾸면, `Run`의 값은 그대로 남고 다음 위치에 비활성 기록이 추가됩니다.

```
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run
```

사용자 환경에서 이 키에 `KakaoTalk`, `MicrosoftEdgeAutoLaunch_...`, `CS Dispatch`가 들어 있는 것을 확인했습니다.

이때 메뉴에는 체크가 붙지만 **실제로는 로그인 시 실행되지 않습니다.** 체크가 거짓말을 합니다.

### 3.3 먼저 측정하십시오 (필수, 선행)

`StartupApproved\Run`의 값은 이진 데이터이고, 첫 바이트로 사용 여부를 나타냅니다. **정확한 바이트 값을 문서에서 옮겨 적지 말고 직접 확인하십시오.**

1. 아무 항목이나 하나를 작업 관리자에서 **사용 안 함**으로 바꾸고, 그 값의 바이트를 덤프해 적으십시오.
2. 다시 **사용**으로 바꾸고 같은 값을 덤프해 적으십시오.
3. 두 경우의 차이를 보고하십시오.

```powershell
$k='HKCU:\Software\Microsoft\Windows\CurrentVersion\Explorer\StartupApproved\Run'
(Get-ItemProperty $k).PSObject.Properties | Where-Object Name -notlike 'PS*' |
  ForEach-Object { "{0} = {1}" -f $_.Name, (($_.Value | ForEach-Object { '{0:X2}' -f $_ }) -join ' ') }
```

**사용자의 다른 시작 프로그램을 건드렸다면 반드시 원래 상태로 되돌리십시오.** 확인이 끝나면 처음 상태와 같아야 합니다.

### 3.4 판정을 고친다 (필수)

`LoginValueExists`가 두 곳을 함께 보게 하십시오.

- `Run`에 값이 없으면 **거짓**입니다.
- `Run`에 값이 있고 `StartupApproved\Run`에 우리 값 이름의 기록이 없으면 **참**입니다. 기록이 없는 것은 기본값인 사용으로 취급됩니다.
- `Run`에 값이 있고 기록이 있으면, 3-3에서 확인한 바이트 규칙으로 판정하십시오.

### 3.5 켤 때 비활성 기록을 지운다 (필수)

사용자가 작업 관리자에서 껐다가 우리 메뉴에서 다시 켜면, 비활성 기록이 남아 있어 여전히 실행되지 않습니다.

`ToggleLoginItem`이 값을 **추가할 때**, `StartupApproved\Run`에서 **같은 이름의 값을 지우십시오.** 그래야 사용 상태로 시작합니다.

- **우리가 만든 이름(`bamti-dock-`으로 시작하는 값)만 지우십시오.**
- 해제할 때도 `StartupApproved\Run`의 우리 이름 기록을 함께 지우십시오. 찌꺼기를 남기지 않습니다.
- 키가 없거나 값이 없으면 오류로 보지 말고 조용히 넘어가십시오.

---

## 4. 하지 말아야 할 것

- **다른 프로그램의 `Run` 값이나 `StartupApproved` 기록을 지우거나 고치지 마십시오.** `bamti-dock-`으로 시작하는 이름만 다룹니다. 3-3의 확인 과정에서 임시로 바꾼 것은 반드시 되돌리십시오.
- `HKEY_LOCAL_MACHINE`을 건드리지 마십시오. 사용자 단위 설정입니다.
- 시작 폴더(`shell:startup`)에 바로가기를 만드는 방식으로 바꾸지 마십시오. 지금 방식을 고칩니다.
- **`LaunchDockApp`의 실행 결과를 바꾸지 마십시오.** 판단을 함수로 뽑되 동작은 같아야 합니다.
- 2-3과 3-3을 건너뛰고 고치지 마십시오. 둘 다 측정이 먼저입니다.
- 지금까지의 안정성 수정과 계측을 되돌리지 마십시오. 감시 스레드, `msg flood`, 트레이 억제, 굶주림 방지, 서브메뉴 닫힘 판정을 모두 유지합니다.
- 작업 관리자에 우리 항목이 어떻게 보이는지를 꾸미려 하지 마십시오. 이번 범위가 아닙니다.

---

## 5. 검증

**명령 생성**
- [ ] 2-3의 세 가지 실행 시험 결과를 그대로 보고한다. 실패했다면 오류 문구를 적는다.
- [ ] 일반 앱과 패키지 앱 각각에 대해 `Run`에 들어간 값을 보고한다.
- [ ] 그 값을 그대로 실행했을 때 앱이 뜬다.
- [ ] `relaunch_command`가 있는 앱에서 그 명령이 반영된다.
- [ ] 클릭으로 앱을 여는 동작이 리팩토링 전과 같다. 일반 앱과 패키지 앱을 각각 확인한다.
- [ ] 명령을 만들 수 없는 앱에는 `로그인 시 열기` 행이 보이지 않는다.

**값 이름**
- [ ] 새 이름 규칙으로 값이 만들어진다.
- [ ] 옛 이름(`bamti-dock-SnippingTool.exe`)으로 만들어진 항목도 메뉴에서 체크로 보이고, 해제하면 지워진다.

**상태 판정**
- [ ] 3-3의 바이트 덤프를 사용/사용 안 함 두 경우 모두 보고한다.
- [ ] 작업 관리자에서 우리 항목을 사용 안 함으로 바꾸면 메뉴의 체크가 사라진다.
- [ ] 그 상태에서 메뉴로 다시 켜면 `StartupApproved`의 기록이 사라지고 체크가 붙는다.
- [ ] 해제하면 `Run`과 `StartupApproved` 양쪽에서 우리 이름이 사라진다.
- [ ] 확인 과정에서 건드린 다른 시작 프로그램이 원래 상태로 돌아와 있다.

**회귀**
- [ ] 서브메뉴가 열리고 닫히며, 바깥 클릭으로 둘 다 닫힌다.
- [ ] `파일 위치 열기`가 그대로 동작한다.
- [ ] 드래그 재정렬과 호버 강조가 그대로 동작한다.
- [ ] 유휴 60초 누적 CPU가 1초 미만이다.
- [ ] `[perf] msg flood` 줄이 남지 않는다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 6. 커밋

셋으로 나누십시오.

```
fix: 로그인 항목 명령을 앱 실행과 같은 기준으로 만든다
```
2-4입니다. 본문에 2-3의 실행 시험 결과를 적으십시오.

```
fix: 로그인 항목 이름이 같은 파일명끼리 겹치지 않게 한다
```
2-5입니다.

```
fix: 시작 프로그램 사용 안 함 상태를 체크 표시에 반영한다
```
3-4와 3-5입니다. 본문에 3-3의 바이트 덤프를 적으십시오.
