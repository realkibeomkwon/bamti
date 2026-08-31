# 작업 지시서: Spotlight의 검색 결과를 Windows 검색과 같게 맞추고 진입점을 늘린다

`TASK-BAR-REORDER.md` 다음에 하십시오.

---

## 0. 먼저 분명히 해 둘 것: 내장 검색 UI는 감쌀 수 없다

요청은 "Windows 내장 검색 위젯을 Spotlight 컨트롤로 감싸고, UI는 지금 것을 쓰되 검색 내용은 내장 검색을 쓰자"였습니다.

**앞부분은 불가능합니다.** Windows 11의 검색 창은 `SearchHost.exe`가 그리는 UWP 화면이고, 다른 프로세스가 그 화면을 자기 창 안에 넣을 수 있게 하는 공개 API가 없습니다. `SetParent`로 남의 UWP 창을 끌어오는 방법은 입력과 DPI와 수명이 모두 깨지고, 업데이트 한 번에 무너집니다. **시도하지 마십시오.**

**뒷부분은 가능하고, 사실 절반은 이미 되어 있습니다.** 요청의 알맹이는 "결과가 Windows 검색만큼 잘 나오게 하자"이므로, 이 지시서는 **결과를 만드는 원천을 Windows가 쓰는 것과 같게 맞추는 일**을 합니다.

지금 상태입니다.

| 종류 | 지금 원천 | Windows 검색의 원천 | 할 일 |
|---|---|---|---|
| 파일과 폴더 | Windows Search 인덱스(`SystemIndex`) | 같음 | 범위만 넓힌다 |
| 앱 | 시작 메뉴 폴더의 `.lnk` 훑기 | `AppsFolder` | **바꾼다** |
| 설정 | 자체 목록 | 설정 앱 색인 | 그대로 둔다 |

`src/spotlight.cpp`의 `QueryIndexedHits`가 이미 `IQueryParserManager`로 `SystemIndex`를 파싱하고 `ISearchFolderItemFactory`로 결과를 받아 옵니다. 파일 검색은 이미 Windows 검색과 같은 색인을 씁니다. **여기를 갈아엎지 마십시오.**

문제는 앱입니다. `ReloadApps`가 이렇게 모읍니다.

```cpp
CollectFolder(KnownFolder(FOLDERID_StartMenu), raw, seen);
CollectFolder(KnownFolder(FOLDERID_CommonStartMenu), raw, seen);
```

시작 메뉴 폴더의 바로 가기만 봅니다. 그래서 **스토어 앱과 UWP 앱이 통째로 빠집니다.** 계산기, 설정, 사진, 터미널, Xbox 같은 것들이 검색되지 않습니다. Windows 검색에서는 나옵니다. 사용자가 "검색 내용이 내장 검색만 못하다"고 느끼는 가장 큰 이유입니다.

---

## 1. 완료 조건

1. 스토어 앱과 UWP 앱이 검색되고 실행됩니다.
2. 앱 아이콘이 시작 메뉴에서 보이는 것과 같습니다.
3. 상단바에 검색 아이콘이 생기고 누르면 Spotlight가 열립니다.
4. 독에 검색 아이콘이 생기고 누르면 Spotlight가 열립니다.
5. 두 아이콘 모두 Windows가 쓰는 검색 아이콘 모양입니다.

---

## 2. 앱 목록을 `AppsFolder`에서 얻는다

### 2-1. 무엇인가

`shell:AppsFolder`는 셸이 관리하는 가상 폴더로, **데스크톱 앱과 패키지 앱을 한데 모아 보여 줍니다.** 시작 메뉴의 "모든 앱"과 Windows 검색이 보는 목록이 바로 이것입니다. `FOLDERID_AppsFolder`로 접근합니다.

### 2-2. 열거

```cpp
Microsoft::WRL::ComPtr<IShellItem> apps;
HRESULT hr = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, nullptr, IID_PPV_ARGS(&apps));
if (FAILED(hr)) { /* 되돌림 */ }

Microsoft::WRL::ComPtr<IEnumShellItems> e;
hr = apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&e));
```

각 항목에서 두 가지를 뽑습니다.

| 값 | 얻는 법 | 쓰임 |
|---|---|---|
| 표시 이름 | `GetDisplayName(SIGDN_NORMALDISPLAY)` | 검색 대상 문자열, 목록에 보이는 글자 |
| 실행 식별자 | `GetDisplayName(SIGDN_PARENTRELATIVEFORADDRESSBAR)` | 실행할 때 쓰는 AppUserModelID 또는 경로 |

`SIGDN_PARENTRELATIVEFORADDRESSBAR`가 패키지 앱에서는 AUMID를, 데스크톱 앱에서는 `.lnk`나 실행 파일 경로에 해당하는 문자열을 돌려줍니다. 둘 다 다음 절의 방법으로 실행할 수 있습니다.

`BHID_EnumItems`는 `shobjidl_core.h`에 있습니다. 이미 포함되어 있는지 확인하고 없으면 더하십시오.

### 2-3. 실행

`ShellExecuteExW`에 `shell:AppsFolder\` 접두사를 붙여 넘깁니다. 데스크톱 앱과 패키지 앱을 가리지 않습니다.

```cpp
const std::wstring target = L"shell:AppsFolder\\" + entry.app_id;
SHELLEXECUTEINFOW info{};
info.cbSize = sizeof(info);
info.fMask = SEE_MASK_NOASYNC;
info.lpVerb = L"open";
info.lpFile = target.c_str();
info.nShow = SW_SHOWNORMAL;
ShellExecuteExW(&info);
```

`IApplicationActivationManager`를 쓰지 마십시오. 데스크톱 앱에는 쓸 수 없어서 갈래가 둘로 늘어납니다.

지금 `LaunchPath`와 `RevealPath`가 경로를 전제로 짜여 있습니다. **패키지 앱에는 파일 경로가 없습니다.** "파일 위치 열기"를 패키지 앱에서 부르면 안 되므로, 앱 항목에 경로가 있는지 없는지를 구분하는 필드를 두고 없으면 그 동작을 막으십시오.

### 2-4. 아이콘

`IShellItemImageFactory::GetImage`로 얻습니다. `HICON`이 아니라 `HBITMAP`을 돌려주므로, 지금 `AppEntry`가 `HICON`을 들고 있는 것과 어긋납니다.

두 갈래 중 하나를 고르십시오.

- `AppEntry`가 `HBITMAP`도 들 수 있게 넓히고, 그리기에서 종류를 나눈다.
- `GetImage`가 준 `HBITMAP`을 `CreateIconIndirect`로 `HICON`으로 바꿔 기존 경로를 그대로 쓴다.

**뒤쪽을 권합니다.** 그리기 코드를 건드리지 않아도 되고, 아이콘 파괴 경로(`DestroyAppIcons`)가 그대로 맞습니다. 다만 `ICONINFO`에 넣을 마스크 비트맵을 함께 만들어야 하고, 만들고 나면 원본 `HBITMAP`을 `DeleteObject`해야 합니다. 새는지 확인하십시오.

아이콘을 열거 도중에 전부 만들지 마십시오. 앱이 수백 개면 시작이 느려집니다. 지금 `RequestIcon`/`AcceptIcon`이 하고 있는 **필요할 때 비동기로 얻는 방식**을 그대로 쓰십시오.

### 2-5. 열거 비용

`AppsFolder` 열거는 `.lnk` 훑기보다 느릴 수 있습니다. 패키지 정보를 읽기 때문입니다. **반드시 재어 보고 로그로 남기십시오.**

```cpp
Log(L"spotlight", L"AppsFolder enumerate %d apps in %.1f ms", count, ms);
```

`ReloadApps`는 이미 `apps_loaded_at_`으로 다시 읽는 주기를 두고 있습니다. 그 구조를 유지하십시오. 열거가 **300ms를 넘으면** 첫 검색이 느껴집니다. 그때는 `Warmup` 시점에 미리 한 번 읽어 두는 쪽으로 옮기십시오.

### 2-6. 실패하면 되돌아간다

`SHGetKnownFolderItem`이나 열거가 실패하면 **지금의 시작 메뉴 훑기로 되돌아가십시오.** 그 코드를 지우지 말고 남겨 두십시오. 되돌아간 사실은 로그에 한 번 남깁니다.

---

## 3. 파일 검색의 범위

`QueryIndexedHits`를 호출하는 쪽이 지금 사용자 프로필 아래로 범위를 좁히고 있습니다.

```cpp
SHGetKnownFolderItem(FOLDERID_Profile, KF_FLAG_DEFAULT, nullptr, IID_PPV_ARGS(&profile))
```

Windows 검색은 색인된 범위 전체를 봅니다. 다만 범위를 넓히면 결과 수가 늘고 순위 다툼이 생깁니다.

**이번에는 건드리지 마십시오.** 앱 목록만 바꾸고, 파일 범위는 지금대로 둔 채 결과를 보십시오. 앱이 제대로 나오기 시작하면 사용자가 느끼는 부족함의 대부분이 사라집니다. 그래도 부족하면 그때 별도로 다룹니다.

---

## 4. 상단바의 검색 아이콘

### 4-1. 어디에 두는가

시작 단추 바로 오른쪽입니다. `bar_layout.cpp`에서 시작 세그먼트를 만드는 자리 옆에 세그먼트를 하나 더 만듭니다.

```cpp
enum class SegmentKind { kStart, kSpotlight, kWarning, kStatus, kOverflow, kClock };
```

너비는 시작 단추와 같은 `kStartHitWidthDip`(34dip)로 두고, 시작 단추 오른쪽에 붙이십시오. 경고 문구와 상태 항목의 왼쪽 한계(`left_limit`)를 이 세그먼트의 오른쪽 끝으로 옮겨야 합니다. 그러지 않으면 항목이 밀려들어와 겹칩니다.

### 4-2. 그리기

`TASK-BAR-ICON-ART.md`에서 만든 Segoe Fluent Icons 텍스트 포맷을 그대로 씁니다. 검색 아이콘의 코드포인트는 **`U+E721`**입니다. Windows가 검색에 쓰는 바로 그 글리프입니다.

시작 단추와 같은 방식으로 마우스가 올라가면 둥근 배경(`MenuItemHoverFill`)을 깔아 주십시오. 두 단추가 나란히 있는데 하나만 반응하면 어색합니다.

### 4-3. 누르면

`MenuBar::ToggleSpotlight()`가 이미 있습니다. `WM_LBUTTONUP`에서 이 세그먼트를 맞히면 부르십시오.

`HitSegment`가 지금 `kStatus`와 `kOverflow`만 돌려줍니다. `kSpotlight`도 돌려주게 하거나, 시작 단추처럼 `HitStart`에 해당하는 전용 판정 함수를 두십시오. **`HitTest`(상태 항목 히트)와 섞지 마십시오.** 상태 항목이 아니므로 툴팁과 이벤트 경로가 다릅니다.

### 4-4. 마우스 상태

시작 단추는 `start_hot_`과 `start_pressed_`를 들고 `UpdateStartChrome`에서 갱신합니다. 검색 단추도 같은 짝이 필요합니다. `UpdateStartChrome`을 두 단추를 함께 다루도록 넓히고 이름을 바꾸십시오. 함수 두 개로 늘리면 `WM_MOUSELEAVE` 처리가 어긋나기 쉽습니다.

---

## 5. 독의 검색 아이콘

### 5-1. 독의 항목은 전부 앱이다

`Dock`은 `std::vector<DockApp> items_`를 들고, `CollectDockApps(pins_)`가 고정된 경로와 실행 중인 창을 모아 만듭니다. `DockApp`은 실행 파일이나 AUMID를 가리킵니다.

Spotlight는 앱이 아니라 bamti 자신의 기능입니다. 이것을 `pins_`에 문자열로 끼워 넣지 마십시오. `CollectDockApps`가 실행 파일을 찾지 못해 지워 버리거나(`SanitizePins`), 사용자가 실수로 고정을 풀면 되살릴 방법이 없습니다.

### 5-2. 항상 첫 자리에 놓는 특수 항목으로 만든다

`DockApp`에 갈래를 나타내는 필드를 하나 더하십시오.

```cpp
enum class DockItemKind { kApp, kSpotlight };

struct DockApp {
  DockItemKind kind = DockItemKind::kApp;
  ...
};
```

`Rebuild`에서 `CollectDockApps`가 돌려준 목록 **맨 앞에** 검색 항목을 끼워 넣습니다. 다음을 지키십시오.

- 고정 해제 메뉴가 뜨지 않게 합니다(`can_pin = false`, `pinned` 무관).
- 끌어서 자리를 바꿀 수 없게 합니다. `BeginDragIfNeeded`와 `DropIndexAt`이 이 항목을 건너뛰어야 합니다. 첫 자리에 고정된 항목이 끌기 계산에 섞이면 슬롯 번호가 어긋납니다.
- 실행 중 표시(점)를 그리지 않습니다.
- 우클릭 메뉴는 열지 않거나, 열더라도 "Spotlight 열기" 하나만 둡니다.

`DisplayOrder`, `SlotIconX`, `HitTest`, `PinnedCount`가 모두 인덱스를 다룹니다. 첫 자리에 항목이 하나 늘어나면 **이 함수들이 전부 영향을 받습니다.** 하나씩 확인하며 고치십시오. 특히 끌기 애니메이션(`anim_x_`)의 원소 수가 항목 수와 맞아야 합니다.

### 5-3. 아이콘

독의 아이콘은 `HBITMAP`입니다(`LoadIconBitmap`, `icons_`). 글리프 폰트를 그대로 쓸 수 없습니다.

`U+E721`을 `Segoe Fluent Icons`로 렌더링한 결과를 32비트 DIB에 그려 `HBITMAP`을 만드십시오. 크기는 다른 아이콘과 같은 픽셀 크기입니다. 만든 결과를 `icon_cache_`에 특수한 키(예: `L"\x01spotlight"`)로 넣어 다시 만들지 않게 하십시오. 파일 경로와 부딪히지 않는 키여야 합니다.

DPI가 바뀌면 아이콘을 다시 만들어야 합니다. `ResetIconCache`가 이미 그 일을 하므로 같은 경로를 타게 두십시오.

### 5-4. 누르면

`Spotlight`를 여는 길이 있어야 합니다. 지금 `Dock`은 `Spotlight`를 모릅니다. `MenuBar`가 들고 있습니다.

`Dock`에 `Spotlight*`를 넘기지 마십시오. 두 창의 수명이 다릅니다. `MenuBar`가 이미 `kToggleSpotlightMsg`(`WM_APP + 8`)를 자기 창에서 받고 있으므로, **독은 상단바 창에 그 메시지를 보내기만 하면 됩니다.**

`Dock`이 상단바의 `HWND`를 알아야 합니다. `FindWindowW(kMenuBarClass, nullptr)`로 찾거나, 생성 시점에 넘겨받으십시오. 넘겨받는 쪽이 낫습니다.

---

## 6. 하지 말아야 할 것

- `SearchHost.exe`나 `SearchApp.exe`의 창을 찾아 `SetParent`하지 마십시오.
- Spotlight의 그리기, 입력, IME 처리를 건드리지 마십시오. 이번 작업은 원천과 진입점만 바꿉니다.
- `QueryIndexedHits`의 AQS 파싱을 다시 쓰지 마십시오. 이미 Windows 검색과 같은 색인을 봅니다.
- 시작 메뉴 훑기 코드를 지우지 마십시오. 되돌림 경로입니다.
- 독의 고정 목록(`pins_`)에 Spotlight를 넣지 마십시오.
- 웹 검색 폴백은 이번 범위가 아닙니다. 넣지 마십시오.
- 레지스트리에 쓰지 마십시오.

---

## 7. 검증

1. Release 빌드가 경고 없이 통과합니다.
2. Spotlight를 열고 `계산기`를 칩니다. 계산기가 나오고 실행됩니다. **수정 전에는 나오지 않던 것입니다.**
3. `설정`, `사진`, `터미널`을 각각 칩니다. 모두 나오고 실행됩니다.
4. 앱 아이콘이 시작 메뉴에서 보이는 것과 같습니다. 빈 사각형이나 기본 아이콘이 아닙니다.
5. 데스크톱 앱(예: 메모장, 브라우저)도 그대로 나오고 실행됩니다.
6. 패키지 앱을 고른 상태에서 "파일 위치 열기"에 해당하는 동작이 오류를 내지 않습니다.
7. 로그에서 `AppsFolder enumerate` 줄을 찾아 열거 시간을 확인합니다. 300ms를 넘으면 2-5절대로 처리했는지 확인합니다.
8. 상단바 시작 단추 오른쪽에 검색 아이콘이 있고, 누르면 Spotlight가 열립니다. 마우스를 올리면 배경이 생깁니다.
9. 상태 항목이 검색 단추와 겹치지 않습니다. 항목을 많이 켜서 확인합니다.
10. 독 맨 앞에 검색 아이콘이 있고, 누르면 Spotlight가 열립니다.
11. 독의 다른 아이콘을 끌어 순서를 바꿉니다. 검색 아이콘은 제자리에 있고, 다른 아이콘의 자리 계산이 어긋나지 않습니다.
12. 독의 검색 아이콘에 실행 중 표시가 그려지지 않고, 우클릭해도 고정 해제 메뉴가 나오지 않습니다.

---

## 8. 커밋

```
feat: Spotlight가 AppsFolder에서 앱을 모은다
feat: 상단바에 검색 단추를 더한다
feat: 독에 검색 단추를 더한다
```
