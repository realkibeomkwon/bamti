# 작업 지시서 17: 버전이 박힌 실행 파일 경로를 핀으로 저장해 업데이트 후 빈 칸이 남는다

원인을 **확정했습니다.** 코드를 읽은 추측이 아니라, 실제 핀 파일과 실행 중에 남긴 로그에서 측정한 결과입니다.

---

## 1. 측정 결과

### 1.1 저장된 핀 하나가 존재하지 않는 파일을 가리킨다

`%USERPROFILE%\.bamti\dock-pins.txt`의 각 경로에 대해 존재 여부를 확인했습니다.

```
MISS  C:\Program Files\WindowsApps\Microsoft.ScreenSketch_11.2605.37.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe
OK    C:\Windows\explorer.exe
OK    C:\Program Files\WindowsApps\Microsoft.OutlookForWindows_1.2026.818.100_x64__8wekyb3d8bbwe\olk.exe
OK    C:\Program Files\WindowsApps\MSTeams_26198.304.4946.9672_x64__8wekyb3d8bbwe\ms-teams.exe
   ... (나머지 14개는 모두 OK)
```

18개의 핀 가운데 캡처 도구 하나만 실패합니다. 스크린샷에서 검색 아이콘과 설정 아이콘 사이가 비어 있는 바로 그 자리입니다.

### 1.2 실제로 설치된 버전은 다른 폴더에 있다

`bamti.log`가 두 경로를 나란히 기록했습니다.

```
9443: [dock] pin cmp branch=path
  a=[...Microsoft.ScreenSketch_11.2605.37.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe]
  b=[...Microsoft.ScreenSketch_11.2605.38.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe]

9446: [dock] pin miss key=...Microsoft.ScreenSketch_11.2605.38.0_...\SnippingTool.exe
```

저장된 핀은 **37.0**이고 실행 중인 프로세스는 **38.0**입니다. 캡처 도구가 Store 업데이트를 받으면서 `WindowsApps` 아래의 버전 폴더 이름이 바뀌었고, 그 순간 저장해 둔 절대 경로가 무효가 되었습니다.

### 1.3 무효가 된 핀이 그대로 항목으로 만들어진다

```
9449: [dock] order after-rebuild 검색>SnippingTool>설정>explorer.exe>olk.exe>...
```

`SnippingTool`이라는 이름의 항목이 실제로 독에 남아 있습니다. 이 이름은 죽은 경로의 파일 이름에서 나왔습니다.

---

## 2. 왜 빈 칸이 되고, 왜 문서 폴더가 열리는가

### 2.1 항목이 만들어지는 경로

`src/task_list.cpp`의 `CollectDockApps` 마지막 분기(1180행 부근)가 실행됩니다.

```cpp
DockApp app;
app.exe_path = pin;                          // 존재하지 않는 37.0 경로
app.aumid = PathAumid(pin);                  // 파일이 없으므로 빈 값
app.key = L"path:" + CanonicalPath(pin);
app.display_name = DisplayNameFor(pin, {});  // SHGetFileInfoW 실패 → 파일 이름 "SnippingTool"
app.pinned = true;
```

`PathAumid`는 `SHGetPropertyStoreFromParsingName`으로 AUMID를 읽는데, 대상 파일이 없으므로 실패합니다. 그래서 이 항목에는 AUMID도, 창 핸들도, 아이콘 리소스도 없이 오직 죽은 경로만 남습니다.

### 2.2 아이콘을 얻을 방법이 하나도 남지 않는다

`src/dock.cpp`의 `Dock::LoadIconBitmap`(1819행)은 다음 순서로 시도합니다.

| 시도 | 조건 | 이 항목의 상태 |
| --- | --- | --- |
| explorer 전용 셸 아이콘 | 파일 이름이 `explorer.exe` | 해당 없음 |
| `BitmapFromAumid` | `aumid`가 있음 | 빈 값이라 건너뜀 |
| `BitmapFromIconResource` | `icon_resource`가 있음 | 빈 값이라 건너뜀 |
| 창 아이콘 | `hwnd`가 있음 | 실행 중이 아니라 `nullptr` |
| `PrivateExtractIconsW` 외 | `PathImpliesGenericIcon`이 거짓 | `\WindowsApps\` 경로라 **조건 자체가 거짓**이어서 블록을 통째로 건너뜀 |
| 마지막 `BitmapFromShellItem` | `exe_path`가 있음 | 파일이 없어 실패 |

결국 `nullptr`을 반환합니다.

### 2.3 아이콘이 없어도 고정 항목은 지워지지 않는다

`Dock::Rebuild`(1685행 부근)의 필터가 이렇습니다.

```cpp
if (items_[i].kind == DockItemKind::kSpotlight || items_[i].pinned ||
    (i < icons_.size() && icons_[i] != nullptr)) {
```

아이콘이 없는 항목을 걸러내는 장치가 있기는 하지만 `pinned`가 먼저 통과시킵니다. **아이콘이 없는 고정 항목은 그대로 슬롯 하나를 차지합니다.** 이것이 화면에 보이는 빈 칸입니다.

### 2.4 "파일 위치 열기"가 문서 폴더를 여는 이유

`src/dock.cpp`의 `ShowInFolder`(718행)는 경로가 비어 있는지만 확인합니다.

```cpp
if (app.exe_path.empty() || app.exe_path.find(L'"') != std::wstring::npos) { return; }
const std::wstring args = L"/select,\"" + app.exe_path + L"\"";
ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
```

경로는 비어 있지 않으므로 이 검사를 통과합니다. 그런데 `explorer.exe /select,"존재하지 않는 경로"`는 오류를 돌려주지 않고 **기본 폴더인 문서를 엽니다.** `ShellExecuteW`의 반환값도 성공이어서 로그에도 아무 흔적이 남지 않습니다. 사용자가 관찰한 `C:\Users\KIBEOMKWON\Documents`는 여기에서 나온 결과입니다.

### 2.5 덤으로 항목이 둘로 갈라진다

`SameDockPin`은 경로를 `CanonicalPath`로 소문자로 바꾼 뒤 문자열 비교만 합니다. 37.0과 38.0은 서로 다른 문자열이므로 매칭에 실패하고, 캡처 도구를 실행하면 죽은 핀 항목과 실행 중인 항목이 **각각 따로** 독에 표시됩니다. 로그 9446행의 `pin miss`가 그 증거입니다.

### 2.6 이것은 캡처 도구 하나만의 문제가 아니다

현재 핀 목록에는 버전 문자열이 포함된 경로가 다섯 개 더 있습니다.

```
Microsoft.OutlookForWindows_1.2026.818.100_x64__8wekyb3d8bbwe\olk.exe
MSTeams_26198.304.4946.9672_x64__8wekyb3d8bbwe\ms-teams.exe
Microsoft.WindowsTerminal_1.24.11911.0_x64__8wekyb3d8bbwe\WindowsTerminal.exe
Microsoft.ScreenSketch_11.2605.37.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe
gitkraken\app-12.4.0\gitkraken.exe
```

지금은 다섯 개가 유효하지만, 각 앱이 업데이트를 받는 순간 똑같이 깨집니다. **MSIX 패키지 앱과 Electron 계열 자동 업데이트 앱의 실행 파일 경로를 영속 식별자로 사용한 것이 근본 원인입니다.**

---

## 3. 수정 방침

네 가지를 함께 적용합니다. 3-1과 3-2는 재발을 막고, 3-3은 이미 깨진 데이터를 되살리며, 3-4는 그래도 남는 경우에 사용자가 혼란을 겪지 않도록 합니다.

### 3-1. 패키지 앱 핀은 AUMID로 저장한다 (`src/task_list.cpp`)

AUMID는 버전과 무관하게 유지되고, 코드에는 이미 `aumid:` 접두사 형식과 그 해석 경로(`ShellItemFromAumid`, `BitmapFromAumid`, `AppsFolderLaunchLine`)가 모두 갖추어져 있습니다.

익명 네임스페이스에 판정 함수를 추가합니다.

```cpp
// MSIX 패키지 앱의 실행 파일은 업데이트마다 버전 폴더 이름이 바뀌므로 경로를 핀으로 쓸 수 없다.
bool IsPackagedPath(const std::wstring& path) {
  const std::wstring lower = Lower(path);
  return lower.find(L"\\windowsapps\\") != std::wstring::npos ||
         lower.find(L"\\systemapps\\") != std::wstring::npos;
}
```

`DockPinId`(842행)의 첫 조건에 이 판정을 더합니다.

```cpp
  if (!app.aumid.empty() &&
      (LooksLikeHostedWebApp(app.aumid) || app.exe_path.empty() || IsHostExe(app.exe_path) ||
       IsPackagedPath(app.exe_path))) {
    return std::wstring(kAumidPinPrefix) + app.aumid;
  }
```

`dock.cpp`의 `PathImpliesGenericIcon`(303행)이 같은 판정을 이미 수행하지만 그쪽은 아이콘 선택 용도라 의미가 다릅니다. 두 함수를 하나로 합치지 말고 각자 두시기 바랍니다.

### 3-2. 핀 매칭을 버전과 무관하게 만든다 (`src/task_list.cpp`)

3-1을 적용해도 이미 저장되어 있는 경로 핀은 여전히 경로로 비교됩니다. 비교하는 단계에서 버전 세그먼트를 지웁니다.

익명 네임스페이스에 다음을 추가합니다.

```cpp
// "Name_Version_Arch[_ResourceId]__PublisherId" 폴더 이름에서 버전과 아키텍처를 지우고
// "name__publisherid"만 남긴다. 패키지 폴더 형식이 아니면 빈 값을 돌려준다.
std::wstring StripPackageVersion(const std::wstring& segment) {
  const size_t pub = segment.rfind(L"__");
  if (pub == std::wstring::npos || pub == 0 || pub + 2 >= segment.size()) {
    return {};
  }
  const size_t name_end = segment.find(L'_');
  if (name_end == std::wstring::npos || name_end >= pub) {
    return {};
  }
  return segment.substr(0, name_end) + L"__" + segment.substr(pub + 2);
}

// 핀을 비교할 때에만 쓰는 형태. 패키지 경로면 버전을 지우고, 아니면 CanonicalPath 그대로다.
std::wstring PathMatchForm(const std::wstring& path) {
  std::wstring canon = CanonicalPath(path);
  const size_t root = canon.find(L"\\windowsapps\\");
  if (root == std::wstring::npos) {
    return canon;
  }
  const size_t begin = root + wcslen(L"\\windowsapps\\");
  size_t end = canon.find(L'\\', begin);
  if (end == std::wstring::npos) {
    end = canon.size();
  }
  const std::wstring stripped = StripPackageVersion(canon.substr(begin, end - begin));
  if (stripped.empty()) {
    return canon;
  }
  return canon.substr(0, begin) + stripped + canon.substr(end);
}
```

변환 결과는 다음과 같아야 합니다.

```
c:\program files\windowsapps\microsoft.screensketch_11.2605.37.0_x64__8wekyb3d8bbwe\snippingtool\snippingtool.exe
→ c:\program files\windowsapps\microsoft.screensketch__8wekyb3d8bbwe\snippingtool\snippingtool.exe
```

`SameDockPin`(855행)의 경로 분기에서 `CanonicalPath`를 `PathMatchForm`으로 바꿉니다.

```cpp
  const std::wstring ca = PathMatchForm(a);
  const std::wstring cb = PathMatchForm(b);
```

`DockPinCompareForm`(898행)의 마지막 `return CanonicalPath(pin);`도 `PathMatchForm(pin)`으로 바꾸어 로그가 새 기준을 보여 주도록 합니다.

**`app.key`를 만드는 `CanonicalPath` 호출은 절대 바꾸지 마십시오.** `key`는 아이콘 캐시의 키이므로, 버전이 바뀌면 캐시를 새로 만드는 지금 동작이 옳습니다. `PathMatchForm`은 매칭에만 사용합니다.

이 변경만으로도 캡처 도구가 실행 중일 때에는 죽은 핀이 실행 항목과 결합되어 아이콘이 살아납니다. 다만 실행 중이 아닐 때에는 여전히 빈 칸이므로 3-3이 필요합니다.

### 3-3. 시작할 때 깨진 경로 핀을 되살린다 (`src/task_list.cpp`, `src/task_list.hpp`, `src/dock.cpp`)

`task_list.hpp`에 선언을 추가합니다.

```cpp
size_t RepairDockPins(std::vector<std::wstring>& pins);
```

구현 규칙입니다.

1. 핀을 훑어 `IsSpotlightPin`이거나 `IsAumidPin`인 항목은 건너뜁니다.
2. `PinPrimary(pin)`에 `GetFileAttributesW`를 걸어 `INVALID_FILE_ATTRIBUTES`가 아니면 건너뜁니다.
3. **깨진 항목이 하나도 없으면 즉시 0을 돌려줍니다.** 아래의 열거 비용을 피하기 위한 조건이므로 반드시 지켜 주십시오.
4. 깨진 항목이 있을 때에만 AppsFolder를 한 번 열거해 `(AUMID, 실행 파일 경로)` 목록을 만듭니다. `src/spotlight.cpp`의 `CollectAppsFolder`(651행)와 같은 방식을 쓰되, 표시 이름 대신 다음 두 가지를 얻습니다.
   - AUMID: `item->GetDisplayName(SIGDN_PARENTRELATIVEFORADDRESSBAR, &id)`
   - 실행 파일 경로: 기존 `FilePathFromShellItem(item.Get())`
   - 경로가 빈 항목은 후보에서 제외합니다.
5. 깨진 핀의 `PathMatchForm`과 후보 경로의 `PathMatchForm`이 같으면 그 후보로 핀을 교체합니다. 후보의 AUMID가 있으면 `aumid:` 형식으로, 없으면 후보의 실제 경로로 바꿉니다. `PinExtra(pin)`이 비어 있지 않으면 탭과 함께 그대로 붙여서 보존합니다.
6. 하나라도 바꾸었으면 `SaveDockPins(pins)`를 호출하고 `Log(L"pins", L"repaired %zu stale=%s new=%s", ...)`를 남깁니다.

`src/dock.cpp`의 `Dock::Create`(1189행)에서 호출합니다.

```cpp
  pins_ = LoadDockPins();
  RepairDockPins(pins_);
  SanitizePins();
```

`CoInitializeEx`는 `src/host.cpp:234`에서 `Dock::Create`보다 먼저 호출되므로, 이 지점에서 셸 API를 사용해도 안전합니다.

**되살리지 못한 핀은 지우지 마십시오.** 외장 드라이브나 네트워크 경로가 일시적으로 보이지 않을 수 있고, 현재 핀에는 `D:\repos\...`와 `D:\PRAi Vision\...`처럼 다른 볼륨에 있는 항목이 포함되어 있습니다. 사용자가 직접 고정한 항목을 임의로 없애면 되돌릴 수 없습니다.

### 3-4. 그래도 남는 경우에 빈 칸과 엉뚱한 폴더를 없앤다 (`src/dock.cpp`)

**아이콘 폴백.** `Dock::LoadIconBitmap`(1819행)의 마지막 `return nullptr;` 앞에 넣습니다.

```cpp
  // 어떤 방법으로도 아이콘을 얻지 못한 고정 항목은 빈 칸으로 남으므로 일반 앱 아이콘을 대신 그린다.
  SHSTOCKICONINFO stock{};
  stock.cbSize = sizeof(stock);
  if (SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_LARGEICON, &stock)) &&
      stock.hIcon != nullptr) {
    HBITMAP ready = BitmapFromIcon(stock.hIcon, px);
    DestroyIcon(stock.hIcon);
    if (ready != nullptr) {
      return ready;
    }
  }
```

`SHGetStockIconInfo`는 `<shellapi.h>`에 선언되어 있고 `shell32`를 이미 링크하고 있으므로 빌드 설정을 바꿀 필요가 없습니다. 반환된 `hIcon`은 직접 해제해야 합니다.

**파일 위치 열기 방어.** `ShowInFolder`(718행)의 첫 검사 뒤에 존재 확인을 추가합니다.

```cpp
  if (GetFileAttributesW(app.exe_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log(L"dock", L"show in folder missing path=%s", app.exe_path.c_str());
    return;
  }
```

explorer가 존재하지 않는 경로에 대해 조용히 문서 폴더를 여는 동작을 여기에서 끊습니다.

---

## 4. 검증

빌드한 뒤 다음을 순서대로 확인해 주십시오.

1. **핀 복구가 일어나는지.** 로그에 `[pins] repaired ...` 줄이 남고, `%USERPROFILE%\.bamti\dock-pins.txt`의 캡처 도구 줄이 `aumid:Microsoft.ScreenSketch_8wekyb3d8bbwe!App` 형태로 바뀌었는지 확인합니다.
2. **빈 칸이 사라졌는지.** 독의 검색 아이콘 오른쪽에 캡처 도구 아이콘이 정상으로 보여야 합니다.
3. **항목이 갈라지지 않는지.** 캡처 도구를 실행한 뒤 로그의 `order after-rebuild`에 `SnippingTool`이 한 번만 나와야 합니다. 지금은 핀 항목과 실행 항목이 둘로 나뉩니다.
4. **잘못된 폴더가 열리지 않는지.** `dock-pins.txt`에 존재하지 않는 임시 경로를 한 줄 넣고 실행한 뒤, 그 항목에서 "파일 위치 열기"를 눌러 문서 폴더가 열리지 않고 `show in folder missing` 로그만 남는지 확인합니다. 확인이 끝나면 그 줄은 지웁니다.
5. **회귀 확인.** Chrome 웹앱 핀 두 개(`aumid:Chrome._crx_...`)와 `C:\Windows\explorer.exe` 핀이 그대로 남아 있는지, 드래그로 순서를 바꾼 뒤 재시작해도 순서가 유지되는지 확인합니다.
6. **패키지 앱 재고정.** Teams나 Outlook을 독에서 뺐다가 다시 고정한 뒤, `dock-pins.txt`에 경로가 아니라 `aumid:` 형태로 기록되는지 확인합니다. 3-1이 동작하는지를 보는 항목입니다.

---

## 5. 주의 사항

- `PathMatchForm`을 `CanonicalPath`가 있던 자리에 전부 끼워 넣지 마십시오. 적용 대상은 `SameDockPin`의 경로 분기와 `DockPinCompareForm` 두 곳뿐입니다.
- `RepairDockPins`의 AppsFolder 열거는 수백 밀리초가 걸릴 수 있습니다. 깨진 핀이 있을 때에만 수행한다는 조건(3-3의 3항)을 빠뜨리면 프로그램을 시작할 때마다 그 비용을 치르게 됩니다.
- `gitkraken\app-12.4.0\gitkraken.exe`처럼 패키지 앱이 아닌 자동 업데이트 앱은 이번 수정 범위 밖입니다. 3-3의 AppsFolder 후보에 시작 메뉴 바로 가기가 잡히면 함께 복구될 수도 있지만, 보장되지는 않습니다. 이 앱이 실제로 깨진 뒤에 별도로 다루는 편이 낫습니다.
- 3-4의 아이콘 폴백을 넣으면 `Dock::Rebuild`의 `icons_[i] != nullptr` 필터가 사실상 항상 참이 됩니다. 고정되지 않은 실행 중 항목이 아이콘 실패만으로 사라지던 기존 동작이 바뀌므로, 검증 5항에서 독의 항목 수가 예상과 맞는지 함께 확인해 주십시오.
