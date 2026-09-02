# 작업 지시서 24: Spotlight에서 영어로 앱을 찾지 못한다

`calculator`로 계산기가, `mstsc`로 원격 데스크톱 연결이, `terminal`로 터미널이 나오지 않습니다. 표시 이름 하나만 검색 대상으로 삼고 있기 때문입니다.

---

## 1. 원인

`src/spotlight.cpp`의 `RebuildMatches`(1972행)는 앱을 고를 때 `name` 한 필드만 봅니다.

```cpp
  for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
    const int score = MatchQuery(apps_[static_cast<size_t>(i)].name, needle);
    if (score >= 0) {
      apps.push_back(Match{Kind::App, i, score});
    }
  }
```

`name`은 `CollectAppsFolder`(650행)가 `SIGDN_NORMALDISPLAY`로 얻은 **표시 이름**입니다. 한국어 Windows에서 이 값은 "계산기", "원격 데스크톱 연결", "터미널"이므로 영어 질의와 겹치는 글자가 없습니다.

바로 아래에 있는 설정 항목 검색은 이 문제를 이미 해결해 두었습니다. `kSettings` 표에 `aliases` 열을 두고 제목과 별칭을 이어 붙여 검색합니다.

```cpp
    std::wstring hay = kSettings[i].title;
    hay.push_back(L' ');
    hay += kSettings[i].aliases;
```

앱에도 같은 구조가 필요합니다. 다만 앱 목록은 표로 적어 둘 수 없으므로 별칭을 **셸에서 얻어야** 합니다.

### 별칭으로 쓸 수 있는 값

`CollectAppsFolder`는 이미 `SIGDN_PARENTRELATIVEFORADDRESSBAR`로 실행에 쓸 식별자(`id`)를 받아 두고 있습니다. 이 값의 형태는 두 가지입니다.

| 앱 종류 | `id`의 예 | 여기서 얻을 별칭 |
|---|---|---|
| 패키지 앱 | `Microsoft.WindowsCalculator_8wekyb3d8bbwe!App` | `microsoft.windowscalculator`, `windowscalculator` |
| 데스크톱 앱 | 시작 메뉴 바로 가기 경로 또는 실행 파일 경로 | `mstsc`, `mstsc.exe` |

`calculator`와 `terminal`은 패키지 앱의 AUMID 안에 그대로 들어 있습니다. `mstsc`는 데스크톱 앱이므로 **바로 가기가 가리키는 실행 파일 경로**까지 풀어야 얻을 수 있습니다.

---

## 2. 먼저 실제 값을 확인한다

`id`가 데스크톱 앱에서 어떤 형태로 오는지는 추측하지 말고 측정하십시오. 3절의 구현을 시작하기 전에, 앱 목록을 처음 적재할 때 **한 번만** 전체를 로그로 덤프하는 코드를 넣고 실행해서 확인합니다.

`Spotlight::AcceptApps`에서 `apps ready` 로그 옆에, 정적 플래그로 첫 회차만 도는 블록을 두십시오.

```cpp
  static bool dumped = false;
  if (!dumped) {
    dumped = true;
    for (const AppEntry& app : apps_) {
      Log(L"spotlight", L"app name=\"%s\" id=\"%s\" target=\"%s\"", app.name.c_str(), app.path.c_str(),
          app.target.c_str());
    }
  }
```

확인할 것은 다음 셋입니다.

1. "계산기"의 `id`가 정말 `Microsoft.WindowsCalculator_...!App` 형태인가.
2. "원격 데스크톱 연결"의 `id`와 `target`에 `mstsc`가 들어 있는가. 들어 있지 않다면 3-2절의 어느 경로가 값을 가져오는 데 실패한 것이므로 그 자리를 고쳐야 합니다.
3. "터미널"이 목록에 있는가. 이름이 "터미널"인지 "Windows 터미널"인지도 함께 봅니다.

이 덤프는 **구현이 끝난 뒤에도 그대로 두십시오.** 첫 회차 한 번뿐이라 비용이 없고, 앞으로 같은 종류의 문제를 진단할 때 바로 쓸 수 있습니다.

---

## 3. 수정 내용

### 3-1. 자료 구조를 넓힌다

`src/spotlight.hpp`의 `AppEntry`에 두 필드를 더합니다.

```cpp
  struct AppEntry {
    std::wstring name;
    std::wstring path;
    std::wstring target;              // 바로 가기가 가리키는 실행 파일의 전체 경로. 없으면 빈 값.
    std::vector<std::wstring> keys;   // 검색 보조 키. 모두 소문자로 넣는다.
    HICON icon = nullptr;
    bool filesystem = false;
  };
```

수집 경로가 `std::pair<std::wstring, std::wstring>`로 이름과 식별자만 나르고 있으므로, 이것을 구조체로 바꿔야 합니다. `spotlight.cpp` 익명 네임스페이스에 다음을 두고, `CollectAppsFolder`(650행)와 `CollectFolder`(553행), `AppsPayload`(967행), `Spotlight::StartAppsReload`, `Spotlight::AcceptApps`의 타입을 모두 바꾸십시오.

```cpp
struct RawApp {
  std::wstring name;
  std::wstring id;
  std::wstring target;
};
```

정렬 비교식(`StartAppsReload`의 `std::sort`)이 `a.first`를 쓰고 있으므로 `a.name`으로 함께 고쳐야 합니다.

### 3-2. 별칭 원본을 모은다

`CollectAppsFolder`의 항목 순회 안에서, `id`를 얻은 뒤에 대상 경로를 한 번 더 시도합니다. 순서대로 시도하고 처음 성공한 값을 씁니다.

```cpp
    // 데스크톱 앱은 표시 이름이 번역되어 있어 영어 질의와 겹치지 않는다. 실행 파일
    // 이름을 별칭으로 쓰기 위해 바로 가기가 가리키는 대상을 풀어 둔다.
    std::wstring target;
    Microsoft::WRL::ComPtr<IShellItem2> item2;
    if (SUCCEEDED(item.As(&item2)) && item2) {
      PWSTR value = nullptr;
      if (SUCCEEDED(item2->GetString(PKEY_Link_TargetParsingPath, &value)) && value != nullptr) {
        target = value;
        CoTaskMemFree(value);
      }
    }
    if (target.empty()) {
      PWSTR parsing = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_DESKTOPABSOLUTEPARSING, &parsing)) && parsing != nullptr) {
        target = parsing;
        CoTaskMemFree(parsing);
      }
    }
```

`PKEY_Link_TargetParsingPath`는 `propkey.h`에 있습니다. 이 헤더가 아직 포함되어 있지 않으면 추가하십시오.

`SIGDN_DESKTOPABSOLUTEPARSING`이 패키지 앱에서는 `shell:AppsFolder\<AUMID>` 형태를 돌려주는데, 그 경우 `id`와 내용이 겹치므로 3-3절의 정제 단계가 중복 키를 걸러 냅니다. 따로 분기하지 마십시오.

폴백 경로인 `CollectFolder`는 `.lnk` 파일을 직접 훑으므로, 거기서는 `full`(바로 가기 파일의 전체 경로)을 `target`에 넣으면 됩니다. 바로 가기가 가리키는 대상까지 풀 필요는 없습니다. 이 경로는 `FOLDERID_AppsFolder` 열거가 실패했을 때만 도는 예비 경로입니다.

### 3-3. 별칭을 정제한다

`AcceptApps`에서 `AppEntry`를 만들 때 `keys`를 채웁니다. 정제 함수를 익명 네임스페이스에 두십시오.

```cpp
// 검색 보조 키를 뽑는다. 중복과 빈 값은 넣지 않고, 모두 소문자로 저장한다.
void AppendSearchKey(std::vector<std::wstring>& keys, std::wstring value) {
  value = LowerCopy(TrimCopy(std::move(value)));
  if (value.size() < 2) {
    return;
  }
  for (const std::wstring& one : keys) {
    if (one == value) {
      return;
    }
  }
  keys.push_back(std::move(value));
}

void CollectSearchKeys(const std::wstring& id, const std::wstring& target, std::vector<std::wstring>& keys) {
  // 패키지 앱: Publisher.Name_해시!진입점 → "publisher.name"과 "name"을 쓴다.
  // 해시와 진입점은 검색어와 겹칠 일이 없고, "!App"은 거의 모든 앱에 붙어 있어
  // 그대로 두면 "app" 질의가 전부 걸린다.
  if (id.find(L'\\') == std::wstring::npos && id.find(L'!') != std::wstring::npos) {
    std::wstring family = id.substr(0, id.find(L'!'));
    const size_t underscore = family.rfind(L'_');
    if (underscore != std::wstring::npos) {
      family.resize(underscore);
    }
    AppendSearchKey(keys, family);
    const size_t dot = family.rfind(L'.');
    if (dot != std::wstring::npos) {
      AppendSearchKey(keys, family.substr(dot + 1));
    }
    return;
  }
  // 데스크톱 앱: 실행 파일 이름을 확장자와 함께, 그리고 확장자 없이 넣는다.
  for (const std::wstring* source : {&id, &target}) {
    const std::wstring leaf = FileLeaf(*source);
    if (leaf.empty()) {
      continue;
    }
    AppendSearchKey(keys, leaf);
    const size_t dot = leaf.rfind(L'.');
    if (dot != std::wstring::npos && dot > 0) {
      AppendSearchKey(keys, leaf.substr(0, dot));
    }
  }
}
```

`FileLeaf`는 이 파일에 이미 있는 함수입니다.

### 3-4. 매칭에 별칭을 태운다

`RebuildMatches`(1985행)의 앱 순회를 다음으로 바꿉니다.

```cpp
  for (int i = 0; i < static_cast<int>(apps_.size()); ++i) {
    const int score = MatchApp(apps_[static_cast<size_t>(i)], needle);
    if (score >= 0) {
      apps.push_back(Match{Kind::App, i, score});
    }
  }
```

`MatchApp`은 익명 네임스페이스에 둡니다.

```cpp
// 별칭으로 걸린 앱은 이름으로 걸린 앱보다 항상 뒤에 놓는다. 점수는 작을수록 앞이다.
constexpr int kAliasPenalty = 20;

int MatchApp(const Spotlight::AppEntry& app, const std::wstring& needle) {
  const int direct = MatchQuery(app.name, needle);
  if (direct >= 0) {
    return direct;
  }
  int best = -1;
  for (const std::wstring& key : app.keys) {
    const int one = MatchQuery(key, needle);
    if (one >= 0 && (best < 0 || one < best)) {
      best = one;
    }
  }
  return best < 0 ? -1 : best + kAliasPenalty;
}
```

`AppEntry`가 지금 `Spotlight`의 비공개 중첩 구조체이므로, 익명 네임스페이스의 자유 함수에서 쓰려면 공개 영역으로 옮겨야 합니다. `FileHit`이 이미 공개로 나와 있으니 그 옆에 두면 기존 구조와 어긋나지 않습니다.

`MatchQuery`와 `MatchScore`는 손대지 마십시오. `MatchScore`가 세 글자 미만 질의에서 부분 일치를 거부하는 규칙(161행)이 별칭에도 그대로 적용되어야 잡음이 늘지 않습니다.

---

## 4. 검증

빌드한 뒤 앱을 다시 띄우고 확인합니다.

1. `calculator`를 입력하면 계산기가 앱 항목에 나와야 합니다. Enter로 실제 실행까지 됩니다.
2. `mstsc`를 입력하면 원격 데스크톱 연결이 나와야 합니다.
3. `terminal`을 입력하면 터미널이 나와야 합니다.
4. `notepad`, `paint`, `explorer`도 함께 확인합니다. 이 셋이 나오지 않으면 별칭 원본이 부족한 것이므로 2절의 덤프를 다시 보십시오.
5. **한국어 검색이 그대로인지 확인합니다.** `계산기`, `설정`, `터미널`을 입력했을 때 결과와 순서가 이전과 같아야 합니다.
6. **이름으로 걸린 앱이 별칭으로 걸린 앱보다 위에 오는지** 확인합니다. 예를 들어 `계산기`를 입력했을 때 계산기가 첫 줄이어야 합니다.
7. `app`을 입력해 봅니다. 결과가 수십 개로 폭발하면 AUMID의 `!App` 부분을 제대로 잘라 내지 못한 것입니다.
8. 로그의 `AppsFolder enumerate ... ms` 값을 이전 세션과 비교합니다. 대상 경로를 푸는 작업이 늘었으므로 시간이 조금 늘어나는 것은 정상이지만, **두 배를 넘으면** 안 됩니다. 넘는다면 `SIGDN_DESKTOPABSOLUTEPARSING` 시도를 `PKEY_Link_TargetParsingPath`가 실패했을 때로만 제한했는지 확인하십시오.

---

## 5. 주의 사항

- 앱 열거는 이미 백그라운드 스레드에서 돕니다(`StartAppsReload`). 별칭을 만드는 작업도 그 스레드 안에서 끝내고, UI 스레드에서 다시 계산하지 마십시오.
- 별칭을 파일에 캐시하지 마십시오. 앱 목록은 이미 주기적으로 다시 읽고 있고, 캐시를 두면 앱 설치와 제거를 따라가지 못합니다.
- 별칭 표를 소스에 손으로 적어 넣는 방식은 쓰지 마십시오. 사용자가 든 세 예시만 통과하고 나머지는 그대로 남습니다.
- `kSettings` 표는 이번 범위가 아닙니다. 설정 항목은 이미 영어 별칭을 갖고 있습니다.
- `LooksLikeFilesystemPath`로 정하는 `filesystem` 플래그와 실행 경로(`LaunchApp`)는 건드리지 마십시오. 별칭은 검색에만 쓰고 실행에는 쓰지 않습니다.
