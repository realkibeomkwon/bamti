# 작업 지시서 18: 패키지 앱 핀 복구가 파일 시스템 경로에 의존해 한 건도 되살리지 못한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

지시서 17의 구현을 검수한 결과입니다. **3-1, 3-2, 3-4는 정상 동작합니다. 3-3만 실패합니다.** 실패 원인은 구현이 아니라 지시서 17의 3-3 설계에 있었습니다.

---

## 1. 측정 결과

### 1.1 버전 제거는 정확히 동작한다

실행 후 로그입니다.

```
[dock] pin cmp branch=path
  a=[C:\Program Files\WindowsApps\Microsoft.ScreenSketch_11.2605.37.0_x64__8wekyb3d8bbwe\SnippingTool\SnippingTool.exe](113)
  ca=[c:\program files\windowsapps\microsoft.screensketch__8wekyb3d8bbwe\snippingtool\snippingtool.exe](96)
```

`PathMatchForm`이 `_11.2605.37.0_x64`를 제거해 113자를 96자로 줄였습니다. 3-2는 의도대로 작동합니다.

### 1.2 복구는 한 건도 일어나지 않았다

```
[pins] loaded 18 from C:\Users\KIBEOMKWON\.bamti\dock-pins.txt
[dock] create pins=18
```

`[pins] repaired ...` 줄이 없습니다. 실행 전후로 `dock-pins.txt`를 `cmp`로 비교했고 **바이트 단위로 동일**했습니다. 깨진 핀은 그대로 남아 있습니다.

```
[dock] order after-rebuild 검색>SnippingTool>설정>explorer.exe>olk.exe>...
```

### 1.3 원인: AppsFolder의 패키지 앱 항목에는 파일 시스템 경로가 없다

셸에 직접 질의했습니다.

```
NAME=[캡처 도구] PATH=[Microsoft.ScreenSketch_8wekyb3d8bbwe!App]
```

MSIX 패키지 앱의 AppsFolder 항목은 **AUMID만 가지며 파일 시스템 항목이 아닙니다.** 그런데 `CollectAppsFolderExecutables`는 이렇게 되어 있습니다.

```cpp
const std::wstring path = FilePathFromShellItem(item.Get());
if (path.empty()) {
  continue;                    // ← 패키지 앱이 전부 여기서 탈락한다
}
```

`FilePathFromShellItem`은 `SIGDN_FILESYSPATH`를 사용하는데, 패키지 앱 항목에서는 실패합니다. 그래서 **후보 목록에 패키지 앱이 하나도 들어가지 못하고**, 뒤이은 `PathMatchForm` 경로 비교는 비교할 대상 자체가 없습니다.

지시서 17의 3-3이 "실행 파일 경로를 얻어 경로끼리 비교하라"고 지시한 것이 잘못이었습니다. 되살려야 할 대상이 바로 경로가 없는 앱인데, 경로로 찾으라고 했기 때문입니다.

---

## 2. 올바른 접근

경로를 거치지 말고 **패키지 패밀리 이름**으로 바로 대조하면 됩니다. 양쪽 모두에서 추출할 수 있습니다.

| 출처 | 값 | 패키지 패밀리 이름 |
| --- | --- | --- |
| 깨진 핀의 폴더 이름 | `Microsoft.ScreenSketch_11.2605.37.0_x64__8wekyb3d8bbwe` | `microsoft.screensketch_8wekyb3d8bbwe` |
| AppsFolder 항목의 AUMID | `Microsoft.ScreenSketch_8wekyb3d8bbwe!App` | `microsoft.screensketch_8wekyb3d8bbwe` |

AUMID는 `<PackageFamilyName>!<AppId>` 형식이므로 `!` 앞을 자르면 그대로 패밀리 이름입니다. 패키지 폴더 이름은 `Name_Version_Arch[_ResourceId]__PublisherId` 형식이므로 `Name`과 `PublisherId`를 언더스코어 **하나**로 이어 붙이면 패밀리 이름이 됩니다.

기존 `StripPackageVersion`이 만드는 값(`microsoft.screensketch__8wekyb3d8bbwe`, 언더스코어 두 개)과는 다릅니다. 그 함수는 경로 비교용이므로 **고치지 말고 그대로 두시고**, 패밀리 이름용 함수를 따로 만드십시오.

---

## 3. 수정 내용

모두 `src/task_list.cpp`에서 이루어집니다. 다른 파일은 손대지 않습니다.

### 3-1. 패키지 패밀리 이름 추출 함수를 추가한다

익명 네임스페이스에 넣습니다. `StripPackageVersion` 바로 아래가 적당합니다.

```cpp
// 패키지 폴더 이름 "Name_Version_Arch[_ResourceId]__PublisherId"에서
// 패키지 패밀리 이름 "name_publisherid"를 만든다. 형식이 아니면 빈 값이다.
std::wstring PackageFamilyFromFolder(const std::wstring& segment) {
  const size_t pub = segment.rfind(L"__");
  if (pub == std::wstring::npos || pub == 0 || pub + 2 >= segment.size()) {
    return {};
  }
  const size_t name_end = segment.find(L'_');
  if (name_end == std::wstring::npos || name_end >= pub) {
    return {};
  }
  return Lower(segment.substr(0, name_end) + L"_" + segment.substr(pub + 2));
}

// WindowsApps 경로에서 패키지 패밀리 이름을 뽑는다. 패키지 경로가 아니면 빈 값이다.
std::wstring PackageFamilyFromPath(const std::wstring& path) {
  const std::wstring canon = CanonicalPath(path);
  const size_t root = canon.find(L"\\windowsapps\\");
  if (root == std::wstring::npos) {
    return {};
  }
  const size_t begin = root + wcslen(L"\\windowsapps\\");
  size_t end = canon.find(L'\\', begin);
  if (end == std::wstring::npos) {
    end = canon.size();
  }
  return PackageFamilyFromFolder(canon.substr(begin, end - begin));
}

// AUMID "PackageFamilyName!AppId"에서 앞부분을 뽑는다. '!'가 없으면 전체를 쓴다.
std::wstring PackageFamilyFromAumid(const std::wstring& aumid) {
  if (aumid.empty()) {
    return {};
  }
  const size_t bang = aumid.find(L'!');
  return Lower(bang == std::wstring::npos ? aumid : aumid.substr(0, bang));
}
```

### 3-2. AUMID만 있는 항목도 후보로 받는다

`CollectAppsFolderExecutables`의 탈락 조건을 바꿉니다. 경로가 없어도 AUMID가 있으면 유효한 후보입니다.

```cpp
  while (e->Next(1, item.ReleaseAndGetAddressOf(), &fetched) == S_OK && fetched == 1) {
    const std::wstring path = FilePathFromShellItem(item.Get());
    std::wstring aumid;
    PWSTR id = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEFORADDRESSBAR, &id)) && id != nullptr) {
      aumid = id;
      CoTaskMemFree(id);
    }
    if (path.empty() && aumid.empty()) {
      continue;
    }
    out.push_back({std::move(aumid), std::move(path)});
  }
```

경로 후보를 버리지 마십시오. 패키지가 아닌 데스크톱 앱은 AppsFolder 항목이 바로 가기 경로를 돌려주므로, 그쪽 매칭에 계속 필요합니다.

함수 이름이 더 이상 내용과 맞지 않으므로 `CollectAppsFolderEntries`로 바꾸고 호출부도 함께 고칩니다.

### 3-3. 매칭을 두 단계로 나눈다

`RepairDockPins`의 후보 탐색 부분을 다음 순서로 바꿉니다.

1. **패밀리 이름 대조 (패키지 앱).** 깨진 핀의 `PackageFamilyFromPath`가 비어 있지 않으면, 후보의 `PackageFamilyFromAumid(candidate.first)`와 비교합니다. 일치하면 그 후보의 AUMID로 교체합니다.
2. **경로 대조 (그 밖의 앱).** 1단계에서 못 찾았을 때만, 기존처럼 `PathMatchForm`끼리 비교합니다. 후보의 경로가 비어 있으면 건너뜁니다.

교체 규칙과 `PinExtra` 보존, `SaveDockPins` 호출, 되살리지 못한 핀을 지우지 않는 원칙은 지금 구현 그대로 유지합니다.

로그는 어느 단계에서 맞았는지 알 수 있게 남겨 주십시오.

```cpp
Log(L"pins", L"repaired by=%s stale=%s new=%s", by, stale_pin.c_str(), pins[i].c_str());
```

`by`는 `L"family"` 또는 `L"path"`입니다.

### 3-4. 후보를 못 찾은 핀을 로그로 남긴다

지금은 복구 실패가 조용히 지나갑니다. 이번 결함을 바로 알아채지 못한 이유이기도 합니다. 깨진 핀을 되살리지 못했을 때 한 줄 남기십시오.

```cpp
Log(L"pins", L"repair miss pin=%s family=%s candidates=%zu", pins[i].c_str(), family.c_str(), candidates.size());
```

`candidates=0`이면 AppsFolder 열거 자체가 실패한 것이고, 0이 아닌데 못 찾았으면 대조 규칙 문제입니다. 둘을 구분할 수 있어야 합니다.

---

## 4. 검증

1. 빌드한 뒤 실행하고 로그에서 다음을 확인합니다.
   ```
   [pins] repaired by=family stale=C:\Program Files\WindowsApps\Microsoft.ScreenSketch_11.2605.37.0_... new=aumid:Microsoft.ScreenSketch_8wekyb3d8bbwe!App
   ```
2. `%USERPROFILE%\.bamti\dock-pins.txt`의 캡처 도구 줄이 `aumid:` 형식으로 바뀌었는지 확인합니다. 실행 전에 파일을 복사해 두고 비교하십시오.
3. `[dock] order after-rebuild`에 캡처 도구가 `SnippingTool`이 아니라 **`캡처 도구`**라는 표시 이름으로 나와야 합니다. AUMID 핀은 `AppsFolderDisplayName`으로 이름을 얻기 때문이며, 이 이름이 바뀌는 것이 복구가 실제로 이루어졌다는 증거입니다.
4. 독에서 캡처 도구 자리에 일반 앱 아이콘이 아니라 **실제 캡처 도구 아이콘**이 보여야 합니다.
5. 두 번째 실행에서는 깨진 핀이 없으므로 `[pins] repaired`도 `[pins] repair miss`도 나오지 않아야 합니다. AppsFolder 열거를 건너뛰는지 확인하는 항목입니다.
6. 회귀 확인: 나머지 17개 핀의 순서와 내용이 그대로인지 비교합니다.

---

## 5. 주의 사항

- `StripPackageVersion`과 `PathMatchForm`은 **고치지 마십시오.** 경로 비교용으로 이미 정상 동작하며(1.1 참고), 여기에 패밀리 이름 규칙을 섞으면 `SameDockPin`의 동작이 함께 바뀝니다.
- 패밀리 이름 비교는 반드시 소문자로 정규화한 뒤 수행하십시오. 폴더 이름은 `Microsoft.ScreenSketch`, AUMID도 대문자로 시작하지만 대소문자가 항상 일치한다는 보장은 없습니다.
- `SystemApps` 폴더는 `Name_PublisherId` 형식이라 버전 세그먼트가 없습니다. `PackageFamilyFromPath`는 `\windowsapps\`만 처리하므로 SystemApps 경로에서는 빈 값을 돌려주고, 그러면 2단계 경로 대조로 넘어갑니다. 의도된 동작이니 SystemApps를 따로 처리하려 하지 마십시오.
- 복구된 핀이 목록에 이미 있는 다른 핀과 같아질 수 있습니다. 지금 핀 목록에서는 발생하지 않지만, 교체 직전에 같은 값이 이미 있는지 확인해 중복을 만들지 않도록 해 주십시오. 중복이면 교체하지 말고 `repair miss`로 남기면 됩니다.
