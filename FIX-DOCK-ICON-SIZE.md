# 수정 지시서: 독의 Claude 아이콘이 이웃보다 작게 보인다

`FIX-DOCK-ICON-BLUR-2.md`의 후속 작업입니다. 그 수정은 그대로 두고 이어서 진행하십시오.

건드리는 파일은 `src/icon_cache.hpp`, `src/icon_cache.cpp`, `src/dock.cpp` 셋입니다.

---

## 1. 증상

판이 포함된 자산으로 바꾼 뒤 Claude 아이콘은 선명해졌습니다. 사용자도 바뀐 모양을 유지하기로 했습니다. 다만 **아이콘이 옆의 다른 앱들보다 작게 보입니다.**

---

## 2. 측정으로 확정된 사실

각 앱에 대해 bamti가 실제로 쓰는 경로로 96픽셀 비트맵을 받아, 알파가 12를 넘는 화소의 경계 상자가 캔버스에서 차지하는 비율을 측정했습니다. Claude만 `SIIGBF_BIGGERSIZEOK`로 받았고 나머지는 지금 코드와 같은 방식입니다. 다시 조사하지 마십시오.

| 앱 | 경로 | 캔버스 | 잉크 경계 상자 | 가로 채움 |
|---|---|---|---|---|
| **Claude (판 포함)** | `aumid` | 88 × 88 | **76 × 76** | **86.4%** |
| Chrome | `exe_extract` | 96 × 96 | 96 × 96 | 100% |
| Zed | `exe_extract` | 96 × 96 | 96 × 96 | 100% |
| Cursor | `exe_extract` | 96 × 96 | 96 × 96 | 100% |
| KakaoTalk | `exe_extract` | 96 × 96 | 96 × 96 | 100% |
| GitKraken | `exe_extract` | 96 × 96 | 96 × 96 | 100% |
| Windows 탐색기 | `exe_shell` | 96 × 96 | 96 × 85 | 100% |
| Outlook | `exe_shell` | 96 × 96 | 92 × 88 | 95.8% |
| Teams | `exe_shell` | 96 × 96 | 89 × 93 | 92.7% |
| 캡처 도구 | `aumid` | 96 × 96 | 96 × 96 | 100% |
| 설정 | `aumid` | 96 × 96 | 96 × 90 | 100% |

`Square44x44Logo.png`의 잉크 경계는 `[6,6]`부터 `[81,81]`까지이며, 사방에 6픽셀씩 투명한 여백이 들어 있습니다.

---

## 3. 근본 원인

`FinalizeIconBitmap`(`src/icon_cache.cpp:457`)은 크기를 맞추기 전에 `CropPaddedJumbo`를 부릅니다. 그런데 그 안의 `FillsMostOfCanvas`(`src/icon_cache.cpp:183`)는 잉크가 캔버스의 **70퍼센트** 이상을 채우면 여백이 없다고 보고 크롭을 건너뜁니다.

```cpp
bool FillsMostOfCanvas(const InkBounds& b, int width, int height) {
  return b.width() >= width * 7 / 10 && b.height() >= height * 7 / 10;
}
```

Claude의 판 포함 자산은 86.4퍼센트를 채우므로 이 조건에 걸려서 크롭되지 않습니다. 그 결과 88픽셀 캔버스가 통째로 60픽셀로 줄어들고, 눈에 보이는 주황색 사각형은 `60 × 76 / 88`, 즉 **약 51.8픽셀**밖에 되지 않습니다. 반면 Chrome이나 Zed처럼 캔버스를 꽉 채운 아이콘은 60픽셀 전부를 씁니다. 이 차이가 사용자가 느낀 크기 차이입니다.

즉 흐림과는 다른 원인이며, 자산이 담고 있는 투명 여백을 그대로 그리고 있다는 점이 문제입니다.

---

## 4. 수정 방향

`FillsMostOfCanvas`의 임계값을 낮추지 마십시오. 그렇게 하면 모든 아이콘의 크롭 판정이 함께 바뀝니다.

대신 **판 포함 자산으로 넘어간 경로에서만** 투명 테두리를 잘라내십시오. 지금 그 경로를 타는 것은 Claude뿐이므로, 다른 아이콘은 한 화소도 달라지지 않습니다.

---

## 5. 구현

### 5-1. `FinalizeIconBitmap`에 여백 제거 인자를 추가한다

`src/icon_cache.hpp:32`의 선언을 다음으로 바꾸십시오. 기본값을 두어서 기존 호출부 여섯 곳은 손대지 않아도 되게 합니다.

```cpp
HBITMAP FinalizeIconBitmap(HBITMAP source, int px, bool straight_alpha, bool trim_padding = false);
```

`src/icon_cache.cpp:457`의 정의를 다음으로 바꾸십시오.

```cpp
HBITMAP FinalizeIconBitmap(HBITMAP source, int px, bool straight_alpha, bool trim_padding) {
  if (source == nullptr) {
    return nullptr;
  }
  BgraImage image;
  const bool ok = BitmapToBgra(source, image);
  DeleteObject(source);
  if (!ok) {
    return nullptr;
  }
  if (trim_padding) {
    TrimTransparentBorder(image);
  } else {
    CropPaddedJumbo(image);
  }
  ZeroTransparentRgb(image);
  ...
}
```

나머지 줄은 그대로 둡니다.

### 5-2. `TrimTransparentBorder`를 만든다

`src/icon_cache.cpp`의 익명 네임스페이스 안, `CropPaddedJumbo` 바로 아래에 넣으십시오.

```cpp
// 앱 자산이 규격대로 남겨 둔 투명 여백을 걷어내서, 그림이 아이콘 칸을 이웃과 같은 정도로
// 채우게 한다. 여백이 아니라 작은 그림을 크게 늘리는 일이 없도록, 이미 캔버스의 절반 이상을
// 채우고 있을 때에만 자른다.
void TrimTransparentBorder(BgraImage& image) {
  if (image.width < 8 || image.height < 8) {
    return;
  }
  const InkBounds ink = FindInkBounds(image, 12);
  if (!ink.ok()) {
    return;
  }
  if (ink.width() < image.width / 2 || ink.height() < image.height / 2) {
    return;
  }
  CropImageToBounds(image, ink);
}
```

`CropImageToBounds`는 이미 사방에 1픽셀씩 여유를 두고 자르므로 경계가 잘려 나가지 않습니다.

### 5-3. 판 포함 경로에서만 이 인자를 켠다

`src/dock.cpp`의 AUMID 분기(`prefer_plated`를 쓰는 자리, 2050줄 부근)에서 `FinalizeIconBitmap` 호출을 다음으로 바꾸십시오.

```cpp
    if (shell != nullptr) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false, prefer_plated)) {
```

`FinalizeIconBitmap`을 부르는 나머지 다섯 자리는 건드리지 마십시오.

### 5-4. 기대되는 결과

Claude의 88픽셀 자산은 `[5,5]`부터 `[82,82]`까지, 즉 78 × 78로 잘립니다. 이것을 60픽셀로 줄이면 눈에 보이는 주황색 사각형이 `60 × 76 / 78`, 즉 **약 58.5픽셀**이 됩니다. 이웃 아이콘이 55.6픽셀(Teams)부터 60픽셀(Chrome, Zed)까지이므로 같은 범위에 들어옵니다.

---

## 6. 검증

1. `out/cmake-debug` 구성으로 빌드가 경고 없이 통과해야 합니다.
2. 로그의 `icon source=aumid ... plated=1` 줄이 Claude에만 붙어 있어야 합니다. 이 값이 곧 여백 제거가 걸린 항목이므로, 다른 앱에 붙었다면 잘못된 것입니다.
3. 독의 Claude 아이콘이 이웃 아이콘과 비슷한 크기로 보이는지 확인해야 합니다. **독은 마우스를 화면 아래쪽 가장자리로 옮겨야 나타나므로, 입력을 합성하지 말고 사용자에게 화면 확인을 부탁하십시오.**
4. 같은 화면에서 캡처 도구, 설정, 터미널, Chrome, Zed, KakaoTalk 아이콘의 크기가 이전과 같은지 함께 확인받으십시오. 이 앱들은 코드 경로가 달라지지 않았으므로 한 화소도 바뀌면 안 됩니다.
5. 확인이 끝나면 `FIX-DOCK-ICON-BLUR-2.md`의 변경과 이번 변경을 각각 별도 커밋으로 남기십시오. 저장소의 기존 관례대로 `fix:` 접두사와 한국어 선언형 문구를 쓰십시오.

---

## 7. 하지 말아야 할 것

- `FillsMostOfCanvas`의 70퍼센트 임계값을 바꾸지 마십시오. 모든 아이콘의 판정이 함께 흔들립니다.
- `CropPaddedJumbo`의 기존 동작을 고치지 마십시오. 점보 목록에서 온 비트맵을 위한 별개의 판정입니다.
- `kIconDip`을 키워서 크기를 맞추려 하지 마십시오. 그러면 이웃 아이콘까지 함께 커집니다.
- 여백 제거를 `FinalizeIconBitmap`의 기본 동작으로 만들지 마십시오. Teams와 Outlook처럼 여백을 의도적으로 둔 아이콘까지 커집니다.
- 잘라낸 뒤 가로와 세로 비율을 억지로 정사각형에 맞추지 마십시오. `BgraToBitmap`이 이미 비율을 지키면서 크기를 맞춥니다.
