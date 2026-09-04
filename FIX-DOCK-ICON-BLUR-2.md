# 수정 지시서: 독의 Claude 아이콘이 여전히 흐리다 (2차)

건드리는 파일은 `src/dock.cpp` 하나입니다.

`FIX-DOCK-ICON-BLUR.md`의 수정은 이미 반영되어 있습니다. 그런데도 증상이 남아 있어서, 이번에는 셸이 실제로 무엇을 돌려주는지를 직접 측정했고 원인을 다르게 확정했습니다. **1차 지시서의 가설(88px 자산을 256px로 확대한다)은 틀렸습니다.** 아래 3절의 원인으로 대체하십시오.

---

## 1. 증상

독에 올라온 아이콘 가운데 Claude 아이콘만 윤곽이 뭉개져 보입니다. 확대나 축소 애니메이션이 없는 정지 상태에서도 흐립니다.

---

## 2. 측정으로 확정된 사실

아래 값은 사용자 환경에서 `IShellItemImageFactory::GetImage`를 직접 호출해서 얻은 것입니다. 다시 조사하지 마십시오.

### 2-1. 현재 코드가 타는 경로

`~/.bamti/bamti.log`에 남은 `icon source=` 줄로 확인했습니다.

| 앱 | source | px |
|---|---|---|
| Claude | `aumid` | 60 |
| Firefox | `aumid` | 60 |
| Zed | `exe_extract` | 60 |

`kIconDip`이 40이고 화면 배율이 150퍼센트이므로 `px`는 60이고, `ShellIconRequestPx(60)`은 96을 돌려줍니다.

### 2-2. Claude 패키지가 담고 있는 자산

패키지 경로는 `C:\Program Files\WindowsApps\Claude_1.40609.1.0_x64__pzs8sxrjxfjjc`이고, `Assets` 폴더에 다음이 들어 있습니다.

| 자산 | 픽셀 크기 | 모양 |
|---|---|---|
| `Square44x44Logo.png` | 88 × 88 | 주황색 둥근 사각형 판 위에 흰색 별 모양 마크 |
| `Square44x44Logo.scale-200.png` | 88 × 88 | 위와 같은 파일 |
| `Square44x44Logo.targetsize-24_altform-unplated.png` | **24 × 24** | 판이 없는 주황색 마크 |
| `Square150x150Logo.png` | 300 × 300 | 판이 있는 형태 |

**판이 없는(unplated) 자산은 24픽셀짜리 하나뿐입니다.**

### 2-3. 셸이 돌려주는 비트맵

96픽셀을 요청했을 때의 결과입니다. `opaqueRatio`는 알파가 12를 넘는 화소 가운데 완전 불투명한 화소의 비율이며, 값이 낮을수록 경계가 뭉개졌다는 뜻입니다.

| 앱 | 플래그 | 돌려받은 크기 | opaqueRatio |
|---|---|---|---|
| Claude | `ICONONLY \| BIGGERSIZEOK` (현재 코드) | 96 × 96 | **10.2%** |
| Claude | `BIGGERSIZEOK` (ICONONLY 제외) | **88 × 88** | 98.0% |
| 캡처 도구 | `ICONONLY \| BIGGERSIZEOK` | 96 × 96 | 95.9% |
| 캡처 도구 | `BIGGERSIZEOK` | 44 × 44 | 93.5% |
| 설정 | `ICONONLY \| BIGGERSIZEOK` | 96 × 96 | 94.0% |
| 설정 | `BIGGERSIZEOK` | 44 × 44 | 86.0% |

Claude에 대해 48, 60, 88, 96, 128, 256을 모두 요청해 보았는데, `ICONONLY`가 붙어 있으면 셸은 어느 크기든 요청한 그대로 만들어 줍니다. 256픽셀로 요청한 결과를 눈으로 보면 계단 모양 덩어리가 뚜렷하게 드러나며, 덩어리 하나의 크기로 역산하면 원본이 24픽셀 남짓입니다.

### 2-4. 패키지별 unplated 자산의 최대 크기

| 패키지 | unplated 자산 개수 | 최대 `targetsize` |
|---|---|---|
| `Microsoft.ScreenSketch` | 28 | 256 |
| `windows.immersivecontrolpanel` | 42 | 256 |
| `Microsoft.WindowsTerminal` | 42 | 256 |
| `Claude` | **1** | **24** |

### 2-5. 접근 권한

관리자 권한이 없는 프로세스에서도 `C:\Program Files\WindowsApps\<패키지>\Assets` 폴더를 열거하고 파일을 읽을 수 있음을 확인했습니다. `WindowsApps` 최상위 폴더를 열거하는 것만 막혀 있습니다.

---

## 3. 근본 원인

`SIIGBF_ICONONLY`를 붙여서 `GetImage`를 호출하면, 셸은 패키지 앱에 대해 **판이 없는 자산(`altform-unplated`)만** 고릅니다. Claude는 그 자산을 24픽셀 하나만 담고 있으므로, 셸은 24픽셀 그림을 60픽셀이든 256픽셀이든 요청받은 크기까지 확대해서 돌려줍니다. bamti는 이미 뭉개진 그 비트맵을 받아 다시 60픽셀로 줄이므로, 확대 과정에서 잃어버린 윤곽이 그대로 남습니다.

캡처 도구, 설정, 터미널이 선명한 이유는 이 앱들이 256픽셀짜리 unplated 자산을 갖고 있어서 확대가 일어나지 않기 때문입니다. 즉 이것은 화면 배율이나 요청 크기의 문제가 아니라, **Claude 패키지에 큰 unplated 자산이 없다는 사실을 코드가 감지하지 못한다는 문제**입니다.

1차 지시서에서 도입한 `ShellIconRequestPx`는 유지해도 무해하지만, 이번 증상과는 무관합니다. 그대로 두십시오.

---

## 4. 수정 방향

`SIIGBF_ICONONLY`를 빼고 호출하면 셸은 판이 포함된 88픽셀 원본 자산을 그대로 돌려줍니다. 이것을 60픽셀로 줄이면 선명합니다.

다만 **이 플래그를 전역으로 빼면 안 됩니다.** 2-3의 표에서 보듯이 캡처 도구와 설정은 `ICONONLY`를 빼는 순간 44픽셀짜리 비트맵을 받게 되어 지금보다 나빠집니다. 그러므로 판단은 앱마다 따로 내려야 합니다.

판단 기준은 다음과 같습니다.

> 패키지가 담고 있는 unplated 자산의 최대 크기가 **확실히 확인되었고**, 그 값이 그리려는 `px`보다 작을 때에만 `SIIGBF_ICONONLY`를 뺀다.

크기를 확인하지 못했을 때(패키지를 찾지 못했거나 자산 폴더를 읽지 못했을 때)는 지금 동작을 그대로 유지해야 합니다. 그래야 이번 변경이 다른 앱을 나쁘게 만들 여지가 없습니다.

---

## 5. 구현

### 5-1. 헤더를 추가한다

`src/dock.cpp` 위쪽의 헤더 묶음에 다음을 넣으십시오. `FindPackagesByPackageFamilyName`과 `GetPackagePathByFullName`은 `kernel32`에 들어 있으므로 링크 설정을 바꿀 필요가 없습니다.

```cpp
#include <appmodel.h>
```

### 5-2. 패키지 설치 경로를 구하는 헬퍼를 만든다

익명 네임스페이스 안, `BitmapFromShellItem`보다 위에 넣으십시오.

```cpp
// AUMID는 "<패키지 패밀리 이름>!<앱 아이디>" 꼴이다.
std::wstring PackageFamilyFromAumid(const std::wstring& aumid) {
  const size_t bang = aumid.find(L'!');
  return bang == std::wstring::npos ? aumid : aumid.substr(0, bang);
}

std::wstring PackageInstallPath(const std::wstring& family) {
  if (family.empty()) {
    return {};
  }
  const UINT32 filters = PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT;
  UINT32 count = 0;
  UINT32 chars = 0;
  LONG rc = FindPackagesByPackageFamilyName(family.c_str(), filters, &count, nullptr, &chars, nullptr, nullptr);
  if (rc != ERROR_INSUFFICIENT_BUFFER || count == 0 || chars == 0) {
    return {};
  }
  std::vector<PWSTR> names(count, nullptr);
  std::vector<wchar_t> buffer(chars, L'\0');
  std::vector<UINT32> props(count, 0);
  rc = FindPackagesByPackageFamilyName(family.c_str(), filters, &count, names.data(), &chars, buffer.data(),
                                       props.data());
  if (rc != ERROR_SUCCESS || count == 0 || names[0] == nullptr) {
    return {};
  }
  UINT32 path_chars = 0;
  rc = GetPackagePathByFullName(names[0], &path_chars, nullptr);
  if (rc != ERROR_INSUFFICIENT_BUFFER || path_chars == 0) {
    return {};
  }
  std::wstring path(path_chars, L'\0');
  rc = GetPackagePathByFullName(names[0], &path_chars, path.data());
  if (rc != ERROR_SUCCESS) {
    return {};
  }
  // 돌려받은 길이에는 끝의 널 문자가 포함되어 있다.
  path.resize(path_chars > 0 ? path_chars - 1 : 0);
  return path;
}
```

### 5-3. unplated 자산의 최대 크기를 조사하는 헬퍼를 만든다

```cpp
int MaxUnplatedTargetSize(const std::wstring& dir) {
  int best = 0;
  WIN32_FIND_DATAW fd{};
  HANDLE find = FindFirstFileW((dir + L"\\*unplated*.png").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return 0;
  }
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      continue;
    }
    std::wstring name = fd.cFileName;
    CharLowerBuffW(name.data(), static_cast<DWORD>(name.size()));
    const size_t at = name.find(L"targetsize-");
    if (at == std::wstring::npos) {
      continue;
    }
    const int value = _wtoi(name.c_str() + at + 11);
    if (value > best) {
      best = value;
    }
  } while (FindNextFileW(find, &fd) != FALSE);
  FindClose(find);
  return best;
}

// 패키지가 담고 있는 판 없는 아이콘 자산의 최대 크기를 돌려준다. 확인하지 못하면 0을 돌려준다.
// EnsureIcons는 독 스레드에서만 돌기 때문에 잠금 없는 정적 캐시로 충분하다.
int LargestUnplatedAssetPx(const std::wstring& aumid) {
  static std::map<std::wstring, int> cache;
  const auto found = cache.find(aumid);
  if (found != cache.end()) {
    return found->second;
  }
  int best = 0;
  const std::wstring root = PackageInstallPath(PackageFamilyFromAumid(aumid));
  if (!root.empty()) {
    best = MaxUnplatedTargetSize(root);
    // 자산은 보통 Assets 같은 하위 폴더에 들어 있으므로 한 단계만 더 내려가 본다.
    WIN32_FIND_DATAW fd{};
    HANDLE find = FindFirstFileW((root + L"\\*").c_str(), &fd);
    if (find != INVALID_HANDLE_VALUE) {
      do {
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
          continue;
        }
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) {
          continue;
        }
        best = (std::max)(best, MaxUnplatedTargetSize(root + L"\\" + fd.cFileName));
      } while (FindNextFileW(find, &fd) != FALSE);
      FindClose(find);
    }
  }
  cache.emplace(aumid, best);
  return best;
}
```

### 5-4. 플래그를 인자로 받도록 고친다

`BitmapFromShellItemObject`와 `BitmapFromAumid`가 지금은 플래그를 함수 안에 박아 두고 있습니다. 인자로 빼되 기본값을 지금 값으로 두어서, 다른 호출부는 손대지 않아도 되게 하십시오.

```cpp
constexpr int kShellIconFlags = SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK;

HBITMAP BitmapFromShellItemObject(IShellItem* item, int request_px, int flags = kShellIconFlags) {
  ...
  if (FAILED(factory->GetImage(size, flags, &bmp))) {
    return nullptr;
  }
  return bmp;
}

HBITMAP BitmapFromAumid(const std::wstring& aumid, int request_px, int flags = kShellIconFlags) {
  Microsoft::WRL::ComPtr<IShellItem> item = ShellItemFromAumid(aumid);
  if (!item) {
    return nullptr;
  }
  return BitmapFromShellItemObject(item.Get(), request_px, flags);
}
```

`BitmapFromShellItem`(경로를 받는 쪽)은 이번 변경과 무관하므로 그대로 두십시오.

### 5-5. `LoadIconBitmap`의 AUMID 분기를 고친다

`src/dock.cpp:1931` 부근의 AUMID 분기를 다음으로 바꾸십시오.

```cpp
  if (!app.aumid.empty()) {
    // 판 없는 자산을 작게만 담은 패키지 앱은 셸이 그 작은 자산을 확대해서 돌려주므로 아이콘이
    // 뭉개진다. 그때만 SIIGBF_ICONONLY를 빼서 판이 포함된 원본 자산을 그대로 받는다.
    const int unplated = LargestUnplatedAssetPx(app.aumid);
    const bool prefer_plated = unplated > 0 && unplated < px;
    HBITMAP shell = nullptr;
    if (prefer_plated) {
      shell = BitmapFromAumid(app.aumid, ShellIconRequestPx(px), SIIGBF_BIGGERSIZEOK);
      // 판이 있는 자산이 오히려 더 작으면 얻는 것이 없으므로 원래 경로로 되돌린다.
      BITMAP bm{};
      if (shell != nullptr && GetObjectW(shell, sizeof(bm), &bm) != 0 && bm.bmWidth < unplated) {
        DeleteObject(shell);
        shell = nullptr;
      }
    }
    if (shell == nullptr) {
      shell = BitmapFromAumid(app.aumid, ShellIconRequestPx(px));
    }
    if (shell != nullptr) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        Log(L"dock", L"icon source=%s name=%s px=%d unplated=%d plated=%d", L"aumid", app.display_name.c_str(), px,
            unplated, prefer_plated ? 1 : 0);
        note(L"aumid");
        return ready;
      }
    }
  }
```

`FinalizeIconBitmap`은 넘겨받은 `HBITMAP`을 안에서 해제하므로, 위 코드에서 `shell`을 따로 지우지 않도록 주의하십시오. 되돌리는 분기에서만 `DeleteObject`를 부릅니다.

---

## 6. 외형이 달라진다는 점을 사용자에게 알려야 한다

이 수정을 적용하면 독의 Claude 아이콘이 **판이 없는 주황색 마크에서, 주황색 둥근 사각형 안에 흰색 마크가 들어간 모양으로 바뀝니다.** 이것은 Windows 작업 표시줄과 시작 메뉴가 이미 쓰고 있는 모양과 같습니다.

Claude 패키지에는 큰 unplated 자산이 아예 없으므로, 판이 없는 모양을 유지하면서 선명하게 만들 방법은 존재하지 않습니다. 선명함과 기존 모양 가운데 하나를 골라야 하는 상황입니다.

그러므로 빌드를 마친 뒤에 **사용자에게 바뀐 모양을 확인받고, 유지할지 되돌릴지 판단을 받으십시오.** 되돌리기를 요청받으면 5-5의 분기만 지우면 되므로 나머지 헬퍼는 그대로 두어도 됩니다.

---

## 7. 검증

1. `out/cmake-debug` 구성으로 빌드가 경고 없이 통과해야 합니다.
2. 앱을 실행하고 독을 한 번 연 뒤 `~/.bamti/bamti.log`를 읽으십시오. 다음 세 줄이 기대하는 값입니다.
   - `icon source=aumid name=Claude px=60 unplated=24 plated=1`
   - `icon source=aumid name=캡처 도구 px=60 unplated=256 plated=0`
   - `icon source=aumid name=설정 px=60 unplated=256 plated=0`
   - `plated=1`이 Claude 말고 다른 앱에도 붙었다면 판정 기준이 잘못 걸린 것이므로 5-5를 다시 보십시오.
3. 독의 Claude 아이콘이 다른 아이콘과 같은 선명도로 보이는지 확인해야 합니다. **독은 마우스를 화면 아래쪽 가장자리로 옮겨야 나타나므로, 입력을 합성하지 말고 사용자에게 화면 확인을 부탁하십시오.**
4. 같은 화면에서 캡처 도구, 설정, 터미널, Firefox 아이콘이 이전보다 나빠지지 않았는지 함께 확인받으십시오.
5. 화면 배율을 바꾸는 검증은 하지 마십시오. 사용자 설정을 되돌리지 못할 위험이 있습니다.

---

## 8. 하지 말아야 할 것

- `SIIGBF_ICONONLY`를 전역으로 빼지 마십시오. 2-3의 측정대로 캡처 도구와 설정이 44픽셀로 떨어집니다.
- `ShellIconRequestPx`를 지우거나 되돌리지 마십시오. 이번 원인과 무관하며, 그대로 두어도 해가 없습니다.
- `kIconDip`을 키워서 흐림을 가리려 하지 마십시오.
- 흐림을 보정하려고 샤프닝 필터를 넣지 마십시오. 원본을 제대로 고르면 필요 없습니다.
- `LargestUnplatedAssetPx`가 0을 돌려줄 때 판이 있는 자산으로 넘어가지 마십시오. 0은 "확인하지 못했다"는 뜻이지 "작다"는 뜻이 아닙니다.
- 아이콘 캐시 키 구조(`items_[i].key + L"|" + px`)를 바꾸지 마십시오.
- `Square150x150Logo.png`를 파일에서 직접 읽는 우회는 이번 범위가 아닙니다. 88픽셀 자산으로 `px`가 60인 지금 화면은 충분히 덮이며, 파일을 직접 읽으면 패키지 구조 변화에 더 취약해집니다.
