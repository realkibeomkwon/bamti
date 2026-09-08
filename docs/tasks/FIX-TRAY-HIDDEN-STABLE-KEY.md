# 작업 지시서: 트레이 아이콘 숨김 설정이 재실행 후에도 유지되게 한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/tray_intercept.cpp`, `src/tray_mirror.cpp`, `src/tray_mirror.hpp`, `src/settings.hpp`, `src/settings.cpp`, `src/menu_bar.cpp` 여섯 개입니다.

---

## 1. 무엇이 잘못되었는가

사용자가 상단바 트레이 메뉴에서 오피스키퍼를 체크 해제해도, bamti 를 다시 실행하면 그 아이콘이 되살아납니다.

**설정 저장 자체는 정상입니다.** `~/.bamti/settings.json` 의 `tray_hidden_keys` 에 값이 남아 있고 로드도 됩니다. 문제는 그 목록에 담기는 값이 프로세스 수명을 넘기지 못한다는 것입니다.

가로채기 백엔드의 키는 `src/tray_intercept.cpp:201` 의 `ItemKey` 가 만듭니다.

```cpp
uint64_t ItemKey(const TrayNotifyIconData& data) {
  if (!GuidEmpty(data.guid_item)) {
    return Fnv1a64(... &data.guid_item ...);
  }
  uint64_t hash = Fnv1a64(... &data.window_handle ...);   // HWND
  return Fnv1a64(... &data.uid ..., hash);
}
```

오피스키퍼는 GUID 없이 아이콘을 등록하므로 아래 경로를 타고, 결국 **창 핸들**에 의존합니다. 창 핸들은 그 프로세스가 다시 뜰 때마다 달라집니다. `~/.bamti/bamti.log` 에 그대로 남아 있습니다.

| 아이콘 | 이전 | 이후 |
|---|---|---|
| 오피스키퍼 | `hwnd=0x20066` | `hwnd=0x10382` |
| Everything | `hwnd=0x3026C` | `hwnd=0x402CE` |
| KakaoTalk | `hwnd=0x202E0` | `hwnd=0x60818` |

그래서 숨김 목록에 죽은 키가 계속 쌓였습니다. 저장된 열한 개 가운데 실제로 살아 있는 것은 두 개뿐입니다.

```
hidden key=0xa5835428ce458fa8 live=1 remembered=1 label=배터리
hidden key=0xa3055b3fc9d345bc live=1 remembered=1 label=오피스키퍼
hidden key=0x3539e4318d4a9ab2 live=0 remembered=0 label=(숨김) 3539e4    ← 나머지 아홉 개
```

**GUID 경로도 대안이 되지 못합니다.** Everything 은 GUID 를 등록하지만 그 값이 실행할 때마다 달라집니다.

```
{00000000-0000-0000-D5EB-7A972F3F0000}
{00000000-0000-0000-DE7B-C8CE483F0000}
{00000001-0000-0000-BE7F-C8CE483F0000}
```

**UIA 백엔드로 바꿔도 해결되지 않습니다.** 그쪽 키는 UI Automation RuntimeId 기반이고, 그마저 없으면 `automation_id + class_name + order` 로 만듭니다(`src/tray_backend_uia.cpp:60`). RuntimeId 는 세션마다 바뀌고, 순서 기반 폴백은 아이콘 배열이 달라지면 곧바로 깨집니다.

## 2. 어떤 식별자를 쓸 것인가

**실행 파일 이름과 `uid` 의 조합**을 씁니다. 로그를 보면 두 값 모두 재실행을 넘어 유지됩니다(오피스키퍼 `uid=151`, KakaoTalk `uid=222`, Everything `uid=0`).

문제는 지금 오피스키퍼의 실행 파일 이름을 얻지 못한다는 점입니다. 로그에 `exe=?` 로 남습니다. `src/tray_intercept.cpp:127` 의 `OwnerExeName` 이 `OpenProcess` 로 시작하는데, 오피스키퍼 프로세스가 그것을 거부하기 때문입니다.

**측정 결과입니다.** `PROCESS_QUERY_LIMITED_INFORMATION` 으로 여는 것조차 `ERROR_ACCESS_DENIED(5)` 로 실패합니다.

```
jscpmon            pid=12772  OPEN_FAILED err=5
jsmamon            pid=11756  OPEN_FAILED err=5
jsmpimon           pid=12652  OPEN_FAILED err=5
```

그런데 **Toolhelp 스냅숏은 같은 프로세스의 이름을 그대로 돌려줍니다.** 프로세스 핸들을 열지 않기 때문입니다.

```
jscpmon            pid=12772  toolhelp=jscpmon.exe
jsmamon            pid=11756  toolhelp=jsmamon.exe
jsmpimon           pid=12652  toolhelp=jsmpimon.exe
```

따라서 `OwnerExeName` 에 Toolhelp 폴백을 붙이면 보호된 프로세스의 이름까지 얻을 수 있고, 그 위에 안정적인 식별자를 세울 수 있습니다.

## 3. 고칠 것

### 3.1 `OwnerExeName` 에 Toolhelp 폴백을 붙인다

`src/tray_intercept.cpp` 의 `OwnerExeName`(127행) 을 고칩니다. `#include <tlhelp32.h>` 를 파일 상단에 더하십시오.

`OpenProcess` 가 실패하거나 `QueryFullProcessImageNameW` 가 실패한 경우, `CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)` 으로 프로세스 목록을 훑어 `th32ProcessID` 가 일치하는 항목의 `szExeFile` 을 돌려줍니다. 두 경로가 모두 실패할 때만 지금처럼 `L"?"` 를 돌려주십시오.

기존의 `OpenProcess` 경로를 지우지 마십시오. 스냅숏은 프로세스 수만큼 순회하므로 더 비쌉니다. 성공하는 쪽을 먼저 시도하는 순서를 유지합니다.

스냅숏 호출은 아이콘이 새로 등록될 때(`created_now` 이거나 소유 창이 바뀔 때)만 일어납니다. `src/tray_intercept.cpp:1157` 을 보면 이미 그 조건 안에 있으므로, 매 라운드마다 도는 일은 없습니다.

### 3.2 안정 식별자를 만드는 함수를 더한다

`src/tray_mirror.hpp` 의 공개부에 정적 함수를 선언하고 `src/tray_mirror.cpp` 에 정의합니다.

```cpp
  // 프로세스가 다시 떠도 유지되는 식별자다. 창 핸들과 RuntimeId 는 재실행마다
  // 바뀌므로 숨김 목록에는 이 값을 저장한다.
  static std::string StableKey(const TrayIconInfo& icon);
```

결정 순서는 이렇습니다.

1. `icon.owner_exe` 가 비어 있지 않고 `L"?"` 도 아니면 → `"exe:" + 소문자로 바꾼 실행 파일 이름 + "#" + std::to_string(icon.uid)`
2. 아니면 `icon.tip` 이 비어 있지 않으면 → `"tip:" + UTF-8 로 바꾼 툴팁의 앞 64자`
3. 둘 다 없으면 → 빈 문자열

**GUID 는 재료로 쓰지 마십시오.** 위에서 보인 대로 Everything 의 GUID 가 실행마다 달라집니다.

2번 폴백은 UIA 백엔드를 위해 필요합니다. `src/tray_backend.hpp:29` 의 주석대로 UIA 백엔드는 `owner_exe` 를 채우지 않습니다. 툴팁이 상태에 따라 변하는 아이콘(`스피커: 0%`, `배터리 상태: 완전히 충전됨(100%)`)에서는 이 폴백이 흔들리지만, 1번이 먹는 가로채기 백엔드가 기본값이므로 실제 영향은 작습니다.

판정용 함수도 함께 둡니다.

```cpp
  static bool StableHidden(const TrayIconInfo& icon, const std::vector<std::string>& hidden);
```

`StableKey` 가 빈 문자열이면 무조건 `false` 를 돌려주십시오. 빈 값끼리 맞아떨어져 엉뚱한 아이콘이 사라지면 안 됩니다.

### 3.3 설정에 `tray_hidden` 을 더한다

`src/settings.hpp` 의 `WidgetSettings` 에 넣습니다. `tray_hidden_keys` 는 **지우지 말고 그대로 두십시오.** 지금 숨겨 둔 배터리와 오피스키퍼가 이번 실행에서도 계속 숨겨져 있어야 합니다.

```cpp
  std::vector<std::string> tray_hidden;  // 안정 식별자. tray_hidden_keys 를 대신한다.
```

`src/settings.cpp` 의 `FormatSettings` 에서 `tray_hidden_keys` 배열을 쓰는 자리 바로 뒤에 같은 방식으로 `"tray_hidden"` 배열을 씁니다. `ParseSettings` 에서는 `json::GetStringArray(*widgets, "tray_hidden")` 으로 읽고, `tray_hidden_keys` 와 똑같이 `kTrayHiddenKeysMax` 로 앞에서부터 잘라 냅니다.

### 3.4 숨김 판정을 두 목록 모두 보게 한다

`src/tray_mirror.cpp:844` 의 `Include` 를 고칩니다.

```cpp
  if (KeyHidden(icon.key, settings.tray_hidden_keys) || StableHidden(icon, settings.tray_hidden)) {
    return false;
  }
```

`src/tray_mirror.cpp:499` 의 `ApplySettings` 안에도 같은 판정이 있습니다. 그 루프는 `items_` 를 돌면서 키만 가지고 판정하므로 안정 식별자를 알 수 없습니다. **`ItemState` 에 `std::string stable;` 을 더하고**, `DoRound` 에서 `ItemState` 를 채울 때(`src/tray_mirror.cpp:1194` 근처와 `Publish` 안) 함께 채우십시오. 그러면 이렇게 쓸 수 있습니다.

```cpp
        const ItemState& st = it->second;
        bool hidden = KeyHidden(it->first, next.tray_hidden_keys);
        if (!hidden && !st.stable.empty()) {
          for (const std::string& one : next.tray_hidden) {
            if (one == st.stable) {
              hidden = true;
              break;
            }
          }
        }
        if (hidden) {
```

### 3.5 메뉴가 안정 식별자를 다루게 한다

`src/tray_mirror.hpp` 의 `MenuItem` 에 `std::string stable;` 을 더합니다.

`MenuItems()`(`src/tray_mirror.cpp:623`) 를 고칩니다.

- 보이는 항목을 만들 때 `row.stable = st.stable;` 을 채웁니다.
- 숨긴 항목을 열거하는 부분에서, 지금 `settings_.tray_hidden_keys` 를 도는 루프를 **그대로 두고**, 그 뒤에 `settings_.tray_hidden` 을 도는 루프를 하나 더 넣습니다. 안정 식별자로 툴팁을 찾으려면 새 맵이 필요합니다.

`src/tray_mirror.hpp` 의 비공개부에 넣습니다.

```cpp
  std::unordered_map<std::string, LastTip> last_stable_tips_;
```

`src/tray_mirror.cpp:1216` 의 `last_tips_` 를 채우는 자리에서 같은 값을 안정 식별자로도 기록하십시오. 그 자리는 `Include` 로 걸러지기 **전**이므로 숨겨 둔 아이콘의 이름도 기억됩니다. `Publish` 안의 `last_tips_`(886행) 갱신 자리에도 같은 줄을 넣습니다.

이미 나온 항목을 두 번 넣지 않도록, 안정 식별자로 본 `seen` 집합을 따로 두십시오. 지금 코드의 `seen`(`uint64_t` 집합) 은 그대로 쓰고, 문자열용 집합을 하나 더 만드는 편이 간단합니다. 보이는 항목의 `stable` 도 그 집합에 미리 넣어야 합니다. 같은 아이콘이 옛 키 목록과 새 목록 양쪽에 남아 있을 수 있기 때문입니다.

라벨을 만들지 못하는 항목을 빼는 기존 규칙(`if (LabelForGuid(guid) == nullptr && tip.empty()) continue;`) 은 새 루프에도 똑같이 적용하십시오. 죽은 항목이 메뉴에 쌓이지 않게 하는 장치입니다.

### 3.6 토글이 새 목록에 쓰게 한다

`src/menu_bar.cpp` 를 고칩니다.

**먼저 함정 하나를 짚습니다.** 이 파일에는 설정을 바꿀 때마다 트레이 쪽 설정을 손으로 복사하는 블록이 일곱 군데 있습니다.

```cpp
        next.tray_hidden_keys = tray.tray_hidden_keys;
```

1105, 1130, 1147, 1167, 1195, 1231, 1959 행입니다. **모든 자리에 `next.tray_hidden = tray.tray_hidden;` 을 함께 넣으십시오.** 한 군데라도 빠뜨리면 그 경로를 지날 때 숨김 설정이 통째로 날아갑니다.

`tray_menu_keys_` 옆에 `std::vector<std::string> tray_menu_stable_;` 을 두고, 메뉴를 만들 때(`src/menu_bar.cpp:2540` 근처) `MenuItem::stable` 을 같은 순서로 담습니다.

`kTrayItemCmdBase` 처리(1185행부터) 를 이렇게 바꿉니다.

1. 지금 숨김 상태인지 판정합니다. 옛 키가 `next.tray_hidden_keys` 에 있거나, 안정 식별자가 `next.tray_hidden` 에 있으면 숨김입니다.
2. 숨김이면 **양쪽에서 모두 지웁니다.** 옛 키 제거는 지금의 `kept` 를 만드는 코드를 그대로 쓰면 됩니다.
3. 숨김이 아니면 `next.tray_hidden` 에만 안정 식별자를 더합니다. `tray_hidden_keys` 에는 더 이상 쓰지 마십시오. 상한을 넘으면 지금처럼 앞에서 하나 지웁니다.

안정 식별자가 빈 문자열이면 3번을 건너뛰고 예전처럼 `tray_hidden_keys` 에 옛 키를 넣으십시오. 식별할 재료가 아무것도 없는 아이콘을 위한 마지막 수단입니다.

`kTrayHideIconCmd` 처리(1158행부터) 도 같은 방식으로 바꿉니다. 이 경로는 `tray_menu_id_` 에서 키만 얻으므로 안정 식별자를 알 수 없습니다. `TrayMirror` 에 조회 함수를 더하십시오.

```cpp
  std::string StableForKey(uint64_t key) const;  // items_ 에서 찾는다. 없으면 빈 문자열이다.
```

## 4. 검증

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

**bamti 는 새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. WMI 가 돌려주는 새 pid 는 재시작의 증거가 되지 못합니다.

상단바 창(클래스 `bamti.MenuBar`)에 `WM_COMMAND` 로 명령 1번을 보내면 `DestroyWindow` 를 거쳐 작업 표시줄과 작업 영역까지 되돌린 뒤 끝납니다.

```powershell
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Bar {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find() {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h, p) => { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
      if (sb.ToString() == "bamti.MenuBar") { f = h; return false; } return true; }, IntPtr.Zero);
    return f;
  }
}
'@ -Language CSharp
$h = [Bar]::Find()
if ($h -ne [IntPtr]::Zero) { [void][Bar]::PostMessageW($h, 0x0111, [IntPtr]1, [IntPtr]0) }
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Process bamti -EA SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
"종료 후 남은 프로세스: " + $(if (Get-Process bamti -EA SilentlyContinue) { (Get-Process bamti).Id -join ',' } else { '없음' })
```

프로세스가 사라진 것을 확인한 뒤에 새 바이너리를 WMI 로 띄웁니다. 세션이 끝나도 살아 있어야 하므로 `Start-Process` 를 쓰지 마십시오.

```powershell
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '"D:\repos\bamti\build\Release\bamti.exe"'
}
"rc=$($r.ReturnValue) pid=$($r.ProcessId)"
```

재시작이 실제로 일어났는지는 로그로 판정하십시오. `[host] start` 줄이 새로 늘어야 합니다.

```powershell
Select-String -Path "$env:USERPROFILE\.bamti\bamti.log" -Pattern '\[host\] start ' | Select-Object -Last 2
```

**화면에 클릭이나 키 입력을 합성하지 마십시오.** 아래의 2번과 4번은 사용자에게 부탁하고 결과를 받으십시오. 로그와 설정 파일을 읽는 것, 화면을 캡처해서 모양을 재는 것은 괜찮습니다.

1. 로그에서 실행 파일 이름이 잡히는지 확인합니다. 이것이 3.1 이 먹었다는 증거입니다.

   ```
   Select-String -Path "$env:USERPROFILE\.bamti\bamti.log" -Pattern 'intercept item tip="오피스키퍼"' | Select-Object -Last 1
   ```

   `exe=?` 가 아니라 실제 파일 이름(예: `exe=jscpmon.exe`)이 나와야 합니다.

2. 사용자에게 상단바 트레이 메뉴에서 오피스키퍼를 체크 해제해 달라고 부탁합니다. 아이콘이 즉시 사라져야 합니다.

3. 설정 파일에 새 형식으로 기록되었는지 확인합니다.

   ```
   Get-Content "$env:USERPROFILE\.bamti\settings.json"
   ```

   `tray_hidden` 배열에 `exe:jscpmon.exe#151` 같은 항목이 들어 있어야 합니다. 16진수 값이면 3.6 이 덜 된 것입니다.

4. **여기가 이 작업의 핵심 판정입니다.** 사용자에게 오피스키퍼를 완전히 종료했다가 다시 실행해 달라고 부탁한 뒤(그래야 창 핸들이 바뀝니다), bamti 를 WMI 로 다시 띄웁니다. 오피스키퍼 아이콘이 여전히 보이지 않아야 합니다. 예전 코드였다면 이 지점에서 되살아났습니다.

5. 사용자에게 같은 항목을 다시 체크해 달라고 부탁합니다. 아이콘이 되살아나고, `tray_hidden` 에서 그 항목이 빠져야 합니다.

6. Everything 과 KakaoTalk 으로도 2번부터 5번까지 되풀이합니다. Everything 은 GUID 를 등록하는 아이콘이고 `uid=0` 이므로, GUID 가 아니라 실행 파일 이름으로 식별되는지 확인하는 자리입니다.

7. 숨기지 않은 다른 아이콘들이 그대로 보이는지 확인합니다. `StableKey` 가 빈 문자열을 돌려주는 아이콘끼리 맞아떨어져 사라지는 일이 없어야 합니다.

8. 상단바의 트레이 아이콘 순서가 예전과 같은지 확인합니다. 이번 작업은 런타임 키를 건드리지 않으므로 순서는 달라지지 않아야 합니다.

## 5. 하지 말 것

- **`ItemKey`(`src/tray_intercept.cpp:201`) 와 `FallbackKey`(`src/tray_backend_uia.cpp:60`) 를 고치지 마십시오.** 런타임 키는 좌표 조회와 클릭 전달에 쓰입니다. 여기에 툴팁처럼 변하는 값을 섞으면 아이콘이 깜박이거나 클릭이 엉뚱한 곳으로 갑니다. 이번 작업은 **저장하는 식별자만** 바꿉니다.
- `tray_hidden_keys` 를 설정 파일에서 지우거나, 로드할 때 버리지 마십시오. 지금 숨겨 둔 항목이 이번 실행에서 되살아납니다.
- `bar_order` 의 `bamti.tray/...` 항목은 이번에 건드리지 마십시오. 같은 원인으로 트레이 아이콘 순서도 재실행 후 초기화되지만, 그것은 별도 작업입니다. 다만 `StableKey` 를 나중에 그대로 재사용할 수 있도록 공개 정적 함수로 두십시오.
- 트레이 백엔드를 `uia` 로 바꾸거나 기본값을 손대지 마십시오.
- 숨김 목록에서 죽은 항목을 자동으로 청소하는 코드를 넣지 마십시오. 무엇이 죽었는지는 그 프로그램이 실행 중일 때만 알 수 있어서, 자동 청소는 잠시 꺼 둔 프로그램의 설정을 지웁니다.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
