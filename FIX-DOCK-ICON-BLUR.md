# 수정 지시서: 독에서 Claude 아이콘만 흐리게 보인다

건드리는 파일은 `src/dock.cpp` 하나입니다.

이 문서를 먼저 처리하고, 그다음에 `FIX-BAR-MENU-DARK.md`로 넘어가십시오. 두 작업은 서로 다른 파일을 건드리므로 순서를 바꾸어도 충돌하지는 않습니다.

---

## 1. 증상

독에 올라온 아이콘 가운데 Claude 아이콘 하나만 윤곽이 뭉개져 보입니다. 같은 독에 있는 Firefox, 카카오톡, Zed, 탐색기 아이콘은 선명합니다. 확대나 축소 애니메이션이 없는 정지 상태에서도 흐립니다.

---

## 2. 이미 확인된 사실

아래 값들은 사용자 환경에서 직접 측정한 것이므로 다시 조사하지 마십시오.

| 항목 | 값 |
|---|---|
| Claude 실행 파일 | `C:\Program Files\WindowsApps\Claude_1.40609.1.0_x64__pzs8sxrjxfjjc\app\Claude.exe` |
| 패키지 종류 | MSIX 패키지 앱 (`Get-AppxPackage -Name Claude`로 확인됨) |
| AUMID | `Claude_pzs8sxrjxfjjc!Claude` |
| 화면 물리 해상도 | 5120 × 2160 |
| 화면 논리 해상도 | 3413 × 1440 |
| DPI 배율 | 150% (dpi = 144) |
| 독 아이콘 크기 | `kIconDip` = 36 이므로 `icon_px` = 54 |

Claude 패키지가 실제로 담고 있는 아이콘 자산은 다음 넷뿐입니다. 픽셀 크기는 PNG 헤더에서 읽은 값입니다.

| 자산 | 픽셀 크기 |
|---|---|
| `Assets\Square44x44Logo.png` | 88 × 88 |
| `Assets\Square44x44Logo.scale-200.png` | 88 × 88 (같은 파일) |
| `Assets\Square44x44Logo.targetsize-24_altform-unplated.png` | 24 × 24 |
| `Assets\Square150x150Logo.png` | 300 × 300 |

셸이 앱 아이콘으로 고르는 것은 `Square44x44Logo` 계열이므로, **이 앱에서 얻을 수 있는 아이콘의 실제 상한은 88px입니다.** 256px 자산은 존재하지 않습니다.

---

## 3. 근본 원인 가설

`Dock::LoadIconBitmap`(`src/dock.cpp:1826`)이 아이콘을 고르는 순서는 다음과 같습니다.

```cpp
const bool identity = !app.aumid.empty() || PathImpliesGenericIcon(app.exe_path);
...
if (!app.aumid.empty()) {
    if (HBITMAP shell = BitmapFromAumid(app.aumid, 256)) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        return ready;
      }
    }
}
```

Claude는 경로에 `\WindowsApps\`를 포함하므로 `PathImpliesGenericIcon`이 참을 돌려주고, AUMID가 있으므로 첫 분기인 `BitmapFromAumid(aumid, 256)`을 탑니다. 그 안에서 호출되는 것이 `IShellItemImageFactory::GetImage({256, 256}, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK)`입니다.

원본 자산이 88px뿐인 앱에 대해 256px를 요청하면, 셸은 88px 이미지를 256px 캔버스로 **확대해서** 돌려줄 수 있습니다. bamti는 그렇게 이미 뭉개진 256px 비트맵을 받아 다시 54px로 축소하므로, 확대 과정에서 잃어버린 윤곽이 그대로 남습니다. 256px 자산이나 256px 아이콘 리소스를 가진 다른 앱은 확대가 일어나지 않으므로 선명합니다.

`kIconDip`이 36이고 배율이 150%이므로 실제로 필요한 크기는 54px뿐인데, 코드가 무조건 256px를 요청하고 있다는 점이 문제의 핵심입니다.

**다만 이것은 아직 가설입니다.** 셸이 확대해서 주는 대신 88px 이미지를 256px 캔버스 가운데에 여백과 함께 얹어서 줄 가능성도 있습니다. 그 경우에는 `CropPaddedJumbo`가 88px 영역만 잘라내므로 흐림의 원인이 다른 곳에 있게 됩니다. 그러므로 **고치기 전에 먼저 측정하십시오.**

---

## 4. 1단계: 측정

`Dock::LoadIconBitmap`이 어떤 경로를 탔고 셸이 무엇을 돌려주었는지를 로그로 남깁니다.

### 4-1. 셸 비트맵의 실제 크기를 찍는다

`src/dock.cpp`의 `BitmapFromShellItemObject`와 `BitmapFromShellItem`에서, `GetImage`가 성공한 직후 돌려받은 `HBITMAP`의 크기를 `GetObjectW`로 읽어 한 줄 남기십시오.

```cpp
BITMAP bm{};
if (GetObjectW(bmp, sizeof(bm), &bm) != 0) {
  Log(L"dock", L"shell image req=%d got=%ldx%ld", request_px, bm.bmWidth, std::abs(bm.bmHeight));
}
```

### 4-2. 크롭 전후 크기를 찍는다

`src/icon_cache.cpp`의 `FinalizeIconBitmap`에서 `CropPaddedJumbo` 호출 전후의 `image.width`와 `image.height`, 그리고 목표 `px`를 한 줄로 남기십시오.

```cpp
const int before_w = image.width;
const int before_h = image.height;
CropPaddedJumbo(image);
Log(L"icon", L"finalize before=%dx%d after=%dx%d target=%d", before_w, before_h, image.width, image.height, px);
```

### 4-3. 어느 경로를 탔는지 찍는다

`Dock::LoadIconBitmap`의 각 분기가 성공해서 반환할 때, 그 경로 이름과 앱 이름을 남기십시오. 예를 들어 `aumid`, `icon_resource`, `window_icon`, `exe_extract`, `exe_shell`, `jumbo`, `stock` 같은 짧은 이름이면 충분합니다.

```cpp
Log(L"dock", L"icon source=%s name=%s px=%d", L"aumid", app.display_name.c_str(), px);
```

### 4-4. 로그를 읽고 판정한다

앱을 띄우고 독을 한 번 연 뒤 로그를 읽으십시오. Claude 항목에 대해 다음을 확정해야 합니다.

1. `source`가 `aumid`가 맞는가.
2. `shell image req=256 got=?` 의 `got` 값이 얼마인가.
3. `finalize before=? after=?` 에서 크롭이 실제로 일어났는가.

판정 기준은 다음과 같습니다.

- **`got=256x256`이고 크롭 후에도 200px를 넘는다** → 셸이 88px를 256px로 확대해서 준 것입니다. 5절로 가십시오. 이것이 예상되는 결과입니다.
- **`got=256x256`인데 크롭 후 90px 안팎으로 줄어든다** → 셸이 여백을 넣어 준 것이고 확대는 없었습니다. 이 경우에는 5절을 적용해도 효과가 없으므로, 6절의 대안을 검토하고 사용자에게 측정값을 보고한 뒤 판단을 받으십시오.
- **`got`이 88 근처로 나온다** → 확대가 없었다는 뜻이므로 흐림의 원인은 축소 단계에 있습니다. 마찬가지로 6절로 가십시오.

측정용 로그는 원인을 확정한 뒤 4-3의 `source` 한 줄만 남기고 나머지는 지우십시오. 이 한 줄은 앞으로도 아이콘 문제를 진단할 때 쓸모가 있습니다.

---

## 5. 2단계: 셸에 요청하는 크기를 실제로 필요한 크기에 맞춘다

측정에서 확대가 확인되었을 때 적용합니다.

### 5-1. 요청 크기를 고르는 헬퍼를 만든다

`src/dock.cpp`의 익명 네임스페이스 안, `BitmapFromShellItem` 바로 위에 다음 함수를 추가하십시오.

```cpp
// 셸 아이콘 캐시는 48/96/256 단계로 관리된다. 그리는 데 필요한 크기 이상인 가장 작은
// 단계를 요청하면, 원본 자산이 작은 앱에서 셸이 크게 확대한 비트맵을 돌려주는 일을 막는다.
int ShellIconRequestPx(int px) {
  if (px <= 48) {
    return 48;
  }
  if (px <= 96) {
    return 96;
  }
  return 256;
}
```

여기서 상한을 256으로 두는 이유는 셸이 그보다 큰 단계를 갖고 있지 않기 때문입니다. 화면 배율이 아무리 커도 이 이상은 의미가 없습니다.

### 5-2. `LoadIconBitmap`의 256 고정값을 모두 바꾼다

`Dock::LoadIconBitmap` 안에서 `256`을 직접 넘기는 자리가 다섯 군데 있습니다. 전부 `ShellIconRequestPx(px)`로 바꾸십시오.

| 위치 | 현재 | 바뀐 뒤 |
|---|---|---|
| 탐색기 분기 | `BitmapFromShellItem(app.exe_path, 256)` | `BitmapFromShellItem(app.exe_path, ShellIconRequestPx(px))` |
| AUMID 분기 | `BitmapFromAumid(app.aumid, 256)` | `BitmapFromAumid(app.aumid, ShellIconRequestPx(px))` |
| exe 아이콘 추출 | `PrivateExtractIconsW(app.exe_path.c_str(), 0, 256, 256, ...)` | 같은 자리에 `ShellIconRequestPx(px)`를 폭과 높이로 |
| exe 셸 이미지 | `BitmapFromShellItem(app.exe_path, 256)` | `BitmapFromShellItem(app.exe_path, ShellIconRequestPx(px))` |
| 마지막 셸 이미지 | `BitmapFromShellItem(app.exe_path, 256)` | `BitmapFromShellItem(app.exe_path, ShellIconRequestPx(px))` |

`BitmapFromIconResource`(`src/dock.cpp:1875` 부근) 안에도 두 자리가 있습니다. 이 함수는 이미 목표 크기 `px`를 인자로 받고 있으므로 그 값을 그대로 쓸 수 있습니다.

```cpp
const UINT got = PrivateExtractIconsW(path.c_str(), has_index ? index : 0,
                                      ShellIconRequestPx(px), ShellIconRequestPx(px),
                                      &icon, nullptr, 1, LR_DEFAULTCOLOR);
...
if (HBITMAP shell = BitmapFromShellItem(path, ShellIconRequestPx(px))) {
```

`BitmapFromJumboList`는 `SHIL_JUMBO` 목록에서 가져오므로 크기를 고를 여지가 없습니다. 그대로 두십시오.

### 5-3. 왜 이 변경으로 다른 앱이 나빠지지 않는가

`px`가 54이면 요청 크기는 96이 됩니다.

- Claude처럼 88px 자산만 있는 앱은 88 → 96으로 9%만 늘어나므로 확대 열화가 사실상 없습니다. 그 뒤 96 → 54 축소는 `BgraToBitmap`이 WIC의 Fant 보간으로 처리하므로 선명합니다.
- 256px 자산을 가진 앱은 셸이 256px 원본을 96px로 줄여서 돌려줍니다. 그 뒤 다시 54px로 줄이므로 축소가 두 번 일어나지만, 축소는 확대와 달리 정보를 잃을 뿐 없는 정보를 지어내지 않으므로 눈에 띄는 열화가 생기지 않습니다.

그래도 기존에 선명하던 아이콘이 나빠지지 않았는지는 7절에서 눈으로 확인해야 합니다. 만약 256px 자산을 가진 앱에서 품질이 떨어진 것이 확인되면, `ShellIconRequestPx`를 `px * 2` 이상인 첫 단계를 고르도록 한 칸 올려서(54 → 128이 아니라 다음 단계인 256) 다시 비교하지 말고, **AUMID 분기에만 낮은 요청 크기를 적용하고 나머지는 256으로 되돌리는 쪽**을 택하십시오. 문제가 확인된 것은 패키지 앱 경로뿐입니다.

---

## 6. 측정 결과가 가설과 다를 때의 대안

4-4에서 확대가 없었다고 판정되면 흐림의 원인은 축소 단계에 있습니다. 그때는 다음 순서로 좁히십시오.

1. `src/icon_cache.cpp`의 `BgraToBitmap`이 쓰는 보간 모드를 확인하십시오. `ScaleWicToBitmap`은 축소일 때 `WICBitmapInterpolationModeFant`를 쓰지만, `BgraToBitmap`이 같은 경로를 타는지 확인이 필요합니다.
2. `DefringePremul`이 88px 같은 작은 원본에서 윤곽 화소를 과하게 뭉개고 있지 않은지, 이 함수를 건너뛴 결과와 비교하십시오.
3. `CropPaddedJumbo`가 Claude 아이콘의 유효 영역을 잘못 잡아 필요 이상으로 확대되는 결과를 만들고 있지 않은지, 4-2의 로그로 확인하십시오.

이 경우에는 고치기 전에 측정값과 함께 사용자에게 상황을 보고하십시오.

---

## 7. 검증

1. `out/cmake-debug` 구성으로 빌드가 경고 없이 통과해야 합니다.
2. 앱을 실행하고 독을 한 번 연 뒤, 로그에서 Claude 항목의 `shell image req=96 got=?` 값을 확인하십시오. `got`이 88 또는 96이면 의도대로 동작한 것입니다.
3. 독의 Claude 아이콘이 다른 아이콘과 같은 선명도로 보이는지 확인해야 합니다. **독은 마우스를 화면 아래쪽 가장자리로 옮겨야 나타나므로, 입력을 합성하지 말고 사용자에게 화면 확인을 부탁하십시오.**
4. 같은 화면에서 Firefox, 카카오톡, Zed, 탐색기 아이콘이 이전보다 나빠지지 않았는지 함께 확인받으십시오.
5. 화면 배율을 바꾸는 검증은 하지 마십시오. 사용자 설정을 되돌리지 못할 위험이 있습니다.

---

## 8. 하지 말아야 할 것

- 아이콘 크기 상수 `kIconDip`을 키워서 흐림을 가리려 하지 마십시오. 문제는 크기가 아니라 확대 열화입니다.
- `Square150x150Logo.png`(300 × 300)를 직접 읽어 쓰는 우회는 이번 범위가 아닙니다. 타일용 로고는 여백 규격과 배경 전제가 달라서 독 아이콘으로 그대로 쓸 수 없습니다.
- 아이콘 캐시 키 구조(`items_[i].key + L"|" + px`)를 바꾸지 마십시오. 지금 구조로도 `px`가 바뀌면 자동으로 다시 로드됩니다.
- 흐림을 보정하려고 샤프닝 필터를 새로 넣지 마십시오. 원본을 제대로 고르면 필요 없습니다.
