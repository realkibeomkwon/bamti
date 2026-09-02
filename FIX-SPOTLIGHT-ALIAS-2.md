# 작업 지시서 26: 검수에서 나온 세 가지를 손본다

`FIX-SPOTLIGHT-ALIAS.md`, `FIX-TRAY-MENU-POS.md`, `FIX-AUTOSTART-DELAY.md`의 구현(`43e3db0`, `dcd7615`, `82f4339`)을 검수하면서 찾은 것들입니다. 1절이 실제 동작에 영향을 주는 결함이고, 2절과 3절은 진단과 예외 상황을 다듬는 항목입니다.

---

## 1. 별칭 분기가 패키지 앱의 절반을 놓친다

### 1-1. 측정 결과

이 컴퓨터의 `FOLDERID_AppsFolder` 항목 200개 가운데 **89개는 식별자에 `!`도 `\`도 없습니다.** PowerShell로 셸을 직접 열거해 센 값입니다.

```
원격 데스크톱 연결
  id     = Microsoft.Windows.RemoteDesktop
  target = C:\WINDOWS\system32\mstsc.exe
터미널
  id     = Microsoft.WindowsTerminal_8wekyb3d8bbwe!App
  target =
```

`Microsoft.Windows.RemoteDesktop`처럼 진입점(`!App`)이 붙지 않는 형태가 흔합니다. `Anysphere.Cursor`, `com.squirrel.gitkraken.GitKraken`, `Chrome._crx_ggjoclfcnjemagj.UserData.Profile1` 같은 것들이 모두 여기에 들어갑니다.

### 1-2. 무엇이 어긋나는가

`src/spotlight.cpp`의 `CollectSearchKeys`(279행)는 패키지 앱을 이렇게 가립니다.

```cpp
  if (id.find(L'\\') == std::wstring::npos && id.find(L'!') != std::wstring::npos) {
```

`!`를 요구하므로 위 89개가 전부 **데스크톱 앱 분기**로 흘러갑니다. 그 분기는 파일 이름에서 확장자를 떼는 것이 목적이라 마지막 점의 **앞쪽**을 취합니다.

```cpp
    const size_t dot = leaf.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      AppendSearchKey(keys, leaf.substr(0, dot));
    }
```

그래서 `Microsoft.Windows.RemoteDesktop`에서 뽑히는 두 번째 키가 `microsoft.windows`가 됩니다. 앱을 가리키는 `remotedesktop`이 아닙니다. 결과가 둘로 나타납니다.

1. `remotedesktop`으로 검색해도 나오지 않습니다.
2. `windows`로 검색하면 `microsoft.windows` 키를 가진 앱이 한꺼번에 걸립니다. `MatchScore`가 점 뒤의 위치를 단어 경계로 인정하므로 점수 2로 잘 걸립니다.

사용자가 든 세 예시(`calculator`, `mstsc`, `terminal`)는 우연히 모두 통과합니다. `mstsc`는 `target`에서 나오고, 나머지 둘은 `!`가 있는 패키지 앱입니다. 그래서 겉으로는 문제가 보이지 않습니다.

### 1-3. 수정 내용

분기로 갈라 하나만 처리하지 말고, **가능한 원본을 모두 태우십시오.** `Microsoft.Windows.RemoteDesktop`은 식별자가 패키지 형태이면서 대상 실행 파일도 함께 갖고 있는 항목이므로, 어느 한쪽만 보면 반드시 무언가를 놓칩니다.

```cpp
// 식별자에서 별칭을 뽑는다. 패키지 앱의 AUMID(Publisher.Name_해시!진입점)와
// 그 변형(진입점이나 해시가 없는 형태)을 함께 다룬다.
void AppendFamilyKeys(std::vector<std::wstring>& keys, const std::wstring& id) {
  std::wstring family = id;
  const size_t bang = family.find(L'!');
  if (bang != std::wstring::npos) {
    family.resize(bang);
  }
  const size_t underscore = family.rfind(L'_');
  if (underscore != std::wstring::npos) {
    family.resize(underscore);
  }
  AppendSearchKey(keys, family);
  // 마지막 마디가 앱 이름이다. "Microsoft.Windows.RemoteDesktop"의 "RemoteDesktop",
  // "Microsoft.WindowsCalculator"의 "WindowsCalculator"가 여기서 나온다.
  const size_t dot = family.rfind(L'.');
  if (dot != std::wstring::npos) {
    AppendSearchKey(keys, family.substr(dot + 1));
  }
}

// 파일 경로에서 별칭을 뽑는다. 실행 파일 이름을 확장자와 함께, 그리고 확장자 없이 넣는다.
void AppendLeafKeys(std::vector<std::wstring>& keys, const std::wstring& path) {
  const std::wstring leaf = FileLeaf(path);
  if (leaf.empty()) {
    return;
  }
  AppendSearchKey(keys, leaf);
  const size_t dot = leaf.rfind(L'.');
  if (dot != std::wstring::npos && dot > 0) {
    AppendSearchKey(keys, leaf.substr(0, dot));
  }
}

void CollectSearchKeys(const std::wstring& id, const std::wstring& target, std::vector<std::wstring>& keys) {
  // 식별자가 경로가 아니면 패키지 계열이다. 진입점이 붙지 않은 형태도 여기에 들어온다.
  if (id.find_first_of(L"\\/:") == std::wstring::npos) {
    AppendFamilyKeys(keys, id);
  } else {
    AppendLeafKeys(keys, id);
  }
  // 대상 실행 파일은 종류를 가리지 않고 언제나 태운다. 표시 이름이 번역된 시스템
  // 도구는 이 경로로만 영어 이름을 얻는다(예: 원격 데스크톱 연결 → mstsc).
  if (!target.empty()) {
    AppendLeafKeys(keys, target);
  }
}
```

`AppendSearchKey`는 그대로 두십시오. 중복과 두 글자 미만을 이미 걸러 냅니다.

### 1-4. 검증

빌드한 뒤 앱을 다시 띄우고 Spotlight에서 확인합니다.

1. `remotedesktop`으로 원격 데스크톱 연결이 나와야 합니다. 지금은 나오지 않습니다.
2. `mstsc`, `calculator`, `terminal`, `notepad`, `paint`가 그대로 나와야 합니다.
3. `windows`를 입력했을 때 결과가 예전보다 **줄어야** 합니다. `microsoft.windows` 잡음 키가 사라지기 때문입니다.
4. `cursor`, `gitkraken`처럼 `!`가 없는 형태의 앱도 그대로 나와야 합니다. 1-3절의 수정으로 이들의 마지막 마디(`cursor`, `gitkraken`)가 별칭에 추가되므로 오히려 잘 걸립니다.
5. 첫 적재 때 남는 `[spotlight] app name=... id=... target=...` 덤프에서 원격 데스크톱 연결 줄을 찾아, `target`이 `C:\WINDOWS\system32\mstsc.exe`로 채워졌는지 확인하십시오. 비어 있다면 `PKEY_Link_TargetParsingPath` 조회가 실패한 것이므로 그쪽을 먼저 보아야 합니다.

---

## 2. 팝업 가드의 진단 로그가 쓸모를 잃는다

`src/tray_popup_guard.cpp`의 `GuardProc`에서 두 가지가 어긋납니다.

### 2-1. 채워지지 않은 사각형을 찍는다

```cpp
  RECT rc{};
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);

  if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
    LogSkip(hwnd, pid, L"not-toplevel", rc);      // rc는 아직 0,0,0,0이다
```

`not-toplevel`과 `hidden` 두 경로는 `GetWindowRect`를 부르기 전이라 사각형이 전부 0으로 남습니다. 로그를 봐도 그 창이 어디에 있었는지 알 수 없습니다. 두 경로에서 `LogSkip`을 부르기 전에 `GetWindowRect(hwnd, &rc)`를 한 번 부르고, 실패하면 0인 채로 두십시오.

### 2-2. 흔한 사유가 다섯 줄 한도를 다 먹는다

무장한 2초 동안 `EVENT_OBJECT_SHOW`는 수십 건이 오고, 그 대부분이 `not-toplevel`이나 `hidden`으로 걸립니다. `kMaxSkipLogs = 5`를 이들이 먼저 소진하면, 정작 판정을 다투는 `too-big`, `pid`, `not-bottom` 줄이 한 개도 남지 않습니다. 이 로그를 넣은 목적 자체가 그 셋을 보려던 것이었습니다.

한도를 사유별로 나누십시오. 사유마다 두 줄씩이면 충분합니다.

```cpp
enum class SkipReason { kNotToplevel, kHidden, kPid, kTooBig, kNotBottom, kCount };
int skip_logs[static_cast<size_t>(SkipReason::kCount)] = {};
constexpr int kMaxSkipLogsPerReason = 2;
```

`TrayPopupGuardArm`에서 배열 전체를 0으로 되돌리는 것을 잊지 마십시오. 지금 `skip_logs = 0`으로 되돌리는 자리와 같은 곳입니다.

---

## 3. 마이그레이션이 남은 Run 값을 정리하지 않는다

`src/autostart.cpp`의 `AutostartMigrate`는 작업이 이미 있으면 곧바로 반환합니다.

```cpp
  Microsoft::WRL::ComPtr<IRegisteredTask> task = GetBamtiTask(folder.Get());
  if (task) {
    ...
    Log(L"host", L"autostart migrate task=1 run_removed=0");
    return;
  }
```

작업과 `Run` 값이 **둘 다 살아 있는 상태**가 이 경로로는 영원히 정리되지 않습니다. 그런 상태는 두 가지 경로로 생깁니다.

1. 첫 마이그레이션에서 작업 등록은 성공했는데 `DeleteRunValue()`가 실패한 경우.
2. 사용자가 다른 도구나 이전 버전 bamti로 `Run` 값을 다시 만든 경우.

단일 인스턴스 뮤텍스가 중복 실행을 막아 주므로 눈에 띄는 고장은 나지 않지만, 매 로그온마다 프로세스가 하나 더 떠서 곧바로 죽는 낭비가 생깁니다. `FIX-AUTOSTART-DELAY.md` 4절이 "둘 다 켜 두지 마십시오"라고 적은 상태가 바로 이것입니다.

작업이 있는 경로에서도 `RunValueExists()`를 확인하고, 있으면 지우십시오.

```cpp
  if (task) {
    const std::wstring have = TaskExecPath(task.Get());
    const std::wstring want = ModulePath();
    if (!have.empty() && !want.empty() && lstrcmpiW(have.c_str(), want.c_str()) != 0) {
      RegisterBamtiTask(service.Get(), folder.Get());
    }
    // 작업이 이미 있는데 Run 값도 남아 있으면 로그온마다 프로세스가 하나 더 뜬다.
    const bool removed = RunValueExists() && DeleteRunValue();
    Log(L"host", L"autostart migrate task=1 run_removed=%d", removed ? 1 : 0);
    return;
  }
```

### 검증

`AutostartMigrate`가 도는 것은 앱을 띄울 때뿐이므로, 확인은 로그로 합니다. 앱을 다시 띄운 뒤 `[host] autostart migrate` 줄을 보고, 그다음 **조회 명령만으로** 상태를 확인하십시오.

```powershell
schtasks /query /tn bamti /fo list
reg query "HKCU\Software\Microsoft\Windows\CurrentVersion\Run" /v bamti
```

작업이 있고 `Run` 값은 없어야 합니다. **레지스트리에 값을 쓰거나 지우는 명령을 검증 절차로 실행하지 마십시오.**

---

## 4. 주의 사항

- 1절과 2절, 3절은 서로 다른 파일을 건드리므로 순서를 지킬 필요가 없습니다. 커밋은 절마다 나누어 주십시오.
- `MatchScore`, `MatchQuery`, `MatchApp`, `kAliasPenalty`는 그대로 두십시오. 이번에 고칠 것은 별칭을 뽑는 단계뿐입니다.
- 앱 목록 덤프(`[spotlight] app name=...`)를 지우지 마십시오. 1절의 검증이 그 줄에 기대고 있습니다.
- 2절의 사유별 한도를 늘려서 해결하려 하지 마십시오. 한도를 키우면 흔한 사유가 더 많이 쌓일 뿐이고, 사유마다 나누는 것이 목적에 맞습니다.
