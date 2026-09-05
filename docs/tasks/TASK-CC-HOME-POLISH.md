# TASK-CC-HOME-POLISH — 제어센터 홈을 다른 시스템 패널과 같은 규격으로 맞추고, 빠른 설정 타일을 정리해 즉시 적용으로 바꾼다

이 문서는 2026-09-05 시점의 작업 지시서이며, 현재 코드의 설명이 아니라 당시의 기록이다.

## 배경

제어센터 홈(`Page::kHome`)은 `src/control_center.cpp`가 그리는데, 드릴다운 패널(Wi-Fi, 사운드, 블루투스, 배터리, CPU)과 치수·색·글꼴 규칙이 따로 논다. 홈은 `kCcWidthDip = 340`과 자체 상수를 쓰고, 패널은 `src/panel_style.hpp`의 `panel::kWidthDip = 308`과 공용 그리기 함수를 쓴다. 그래서 홈에서 Wi-Fi를 누르면 팝업 폭이 340에서 308로 줄어들고, 같은 성격의 요소가 서로 다른 크기로 보인다.

요청은 다섯 가지다.

1. Wi-Fi 행과 Bluetooth 행이 붙어 있으니 사이에 여백을 넣는다.
2. **비행기 모드 타일과 접근성 타일을 없앤다.** 빠른 설정에는 절전 모드와 야간 모드만 남긴다.
3. 남은 절전 모드와 야간 모드 타일이 Windows 설정 앱을 열기만 하는데, 눌렀을 때 바로 적용되게 한다.
4. 디스플레이와 사운드 슬라이더를 사운드 패널의 슬라이더와 같은 모양으로 바꾼다.
5. 홈 전체의 디자인 규격을 다른 시스템 패널에 맞춘다.

2번은 3번의 범위를 줄여 준다. 비행기 모드는 `asInvoker` 권한으로는 OS의 비행기 모드 플래그를 뒤집을 수 없어서 "모든 무선을 끄는 타일"이라는 어정쩡한 물건이 될 수밖에 없었고, 접근성은 켜고 끄는 단일 스위치가 아니라 설정 묶음이라 즉시 적용이라는 개념 자체가 성립하지 않았다. 둘 다 없애면 남는 두 타일은 모두 진짜 토글이 된다.

---

## 1부 — 레이아웃과 디자인 통일

### 1.1 홈 폭을 308로 내린다

`src/control_center.cpp` 위쪽 익명 네임스페이스의 상수를 고친다.

```cpp
constexpr int kCcWidthDip = 340;      // → 삭제하고 panel::kWidthDip(308)을 쓴다
constexpr int kPanelPadDip = 14;      // → 그대로 둔다. panel::kInsetDip과 값이 같다
constexpr int kCardWDip = 312;        // → 280  (= 308 - 14 * 2)
constexpr int kQuickTileWDip = 151;   // → 135  (= (280 - 10) / 2)
```

`ControlCenterContent::WidthDip()`은 페이지에 따라 갈라질 이유가 없어지므로 항상 `panel::kWidthDip`을 돌려주도록 단순화한다.

```cpp
int ControlCenterContent::WidthDip() const {
  return panel::kWidthDip;
}
```

`FooterRect()`가 `DipToPx(kCcWidthDip, dpi)`로 폭을 구하고 있으니 여기도 `panel::kWidthDip`으로 바꾼다.

이 변경으로 홈과 드릴다운 패널의 폭이 같아지고, 페이지를 오갈 때 팝업이 더는 늘었다 줄었다 하지 않는다.

### 1.2 Wi-Fi 행과 Bluetooth 행을 별개 카드로 떼어 놓는다

지금은 두 행이 한 장의 둥근 카드 안에 맞붙어 있다.

```cpp
constexpr int kConnectHDip = 104;      // 52 * 2
constexpr int kConnectRowHDip = 52;
```

```cpp
RECT ControlCenterContent::ConnectRowRect(UINT dpi, int row) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int y = pad + DipToPx(kConnectRowHDip, dpi) * row;   // ← 간격이 없다
  ...
}
```

```cpp
const RECT connect0 = ConnectRowRect(dpi, 0);
const RECT connect1 = ConnectRowRect(dpi, 1);
fill_round(RECT{connect0.left, connect0.top, connect1.right, connect1.bottom}, CardFillColor(dark));  // ← 한 장으로 칠한다
```

아래 두 곳을 고쳐 빠른 설정 타일과 같은 `kQuickGapDip`(10) 간격으로 벌린다.

```cpp
constexpr int kConnectHDip = 114;   // 52 + 10 + 52

RECT ControlCenterContent::ConnectRowRect(UINT dpi, int row) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int y = pad + row * DipToPx(kConnectRowHDip + kQuickGapDip, dpi);
  return RECT{pad, y, pad + DipToPx(kCardWDip, dpi), y + DipToPx(kConnectRowHDip, dpi)};
}
```

`Render`에서는 두 행을 한 장으로 칠하지 말고 반복문 안에서 행마다 `fill_round(row.rc, CardFillColor(dark))`를 먼저 부른 뒤, 호버일 때 그 위에 `MenuItemHoverFill`을 덧칠한다. 빠른 설정 타일이 이미 그렇게 그리고 있으니 같은 순서를 따르면 된다.

연결 구역과 빠른 설정 구역이 같은 격자(행 52dip, 간격 10dip)를 쓰게 된다. 1.3절에서 빠른 설정이 한 행으로 줄어들면 두 구역의 높이는 달라지지만 격자는 그대로 맞는다.

### 1.3 빠른 설정 타일을 절전 모드와 야간 모드 둘만 남긴다

지금은 2행 2열이다.

```cpp
const Quick quick[] = {
    {kAirplane, 0, 0, kPlaneGlyph, L"비행기 모드", false},
    {kSaver, 1, 0, kSaverGlyph, L"절전 모드", saver_on_},
    {kNight, 0, 1, kNightGlyph, L"야간 모드", false},
    {kAccess, 1, 1, kAccessGlyph, L"접근성", false},
};
```

이것을 1행 2열로 줄인다.

```cpp
const Quick quick[] = {
    {kSaver, 0, 0, kSaverGlyph, L"절전 모드", saver_on_},
    {kNight, 1, 0, kNightGlyph, L"야간 모드", night_on_},
};
```

같이 지울 것.

- `HitId` 열거형에서 `kAirplane`과 `kAccess`를 뺀다. `kWifi`부터 시작하는 암묵적 연번이니 뒤의 값이 밀리는데, 이 값들은 저장되거나 프로세스 밖으로 나가지 않으므로 밀려도 문제가 없다.
- `BuildHits`에서 `add(kAirplane, ...)`와 `add(kAccess, ...)` 두 줄을 지우고, 남은 두 줄을 `QuickTileRect(dpi, 0, 0)`과 `QuickTileRect(dpi, 1, 0)`으로 고친다.
- `Invoke`에서 `case kAirplane`과 `case kAccess`를 지운다.
- 이제 아무도 쓰지 않는 `kPlaneGlyph`(E709)와 `kAccessGlyph`(E776) 상수를 지운다. 남겨 두면 `/W4`에서 경고가 난다.

구역 높이를 한 행짜리로 줄인다.

```cpp
constexpr int kQuickHDip = 114;   // → 52  (= kQuickTileHDip)
```

`kQuickTileWDip`은 1.1절에서 정한 135를 그대로 쓴다. 한 행에 두 장이 나란히 놓이는 구조는 바뀌지 않는다.

### 1.4 슬라이더를 사운드 패널과 같은 모양으로 바꾼다

사운드 패널(`RenderVolumePage`)의 슬라이더 규격은 다음과 같다.

| 항목 | 값 |
| --- | --- |
| 행 높이 | `kVolumeSliderRowHDip` = 28 |
| 좌우 아이콘 | `kVolumeSliderIconDip` = 14, 간격 `kVolumeSliderGapDip` = 8 |
| 트랙 높이 | `kVolumeTrackHDip` = 6, 모서리는 `corner::PillPx` |
| 트랙 배경 | `BadgeOffFill(dark)` |
| 트랙 채움 | `AccentFillColor(dark)` |
| 노브 | 지름 `kVolumeKnobDip` = 18인 원, `AccentOnColor(dark)` |
| 음소거 | 채움과 노브에 `ScaleAlpha(..., 0.40f)` |

홈의 슬라이더는 이와 전혀 다르다. 24dip짜리 두꺼운 알약 트랙을 그리고, 노브 없이 왼쪽 끝 캡 안에 글리프를 넣고, 트랙 배경으로 `ScaleAlpha(fg, 0.12f)`를 쓴다. 이것을 위 표의 규격으로 갈아엎는다.

**카드 자체는 남긴다.** 홈은 카드 기반 레이아웃이고 연결 행과 빠른 설정 타일이 모두 카드이므로, 슬라이더만 카드를 벗기면 오히려 어긋난다. 카드 안의 내용물을 사운드 패널 규격으로 맞추는 것이 이번 작업이다.

카드 높이를 늘린다.

```cpp
constexpr int kSliderCardHDip = 56;   // → 60
constexpr int kSliderTrackHDip = 24;  // → 삭제한다. kVolumeTrackHDip(6)을 쓴다
```

카드 안 배치는 이렇게 잡는다. 좌표는 모두 카드 상단 기준이다.

| 요소 | 오프셋 | 높이 |
| --- | --- | --- |
| 제목("디스플레이", "사운드") | +8 | `panel::kSectionHDip` = 17 |
| 슬라이더 행 | +28 | `kVolumeSliderRowHDip` = 28 |
| 하단 여백 | +56 | 4 |

제목은 `panel::DrawSectionHeader`와 같은 눈금으로 그린다. 즉 글꼴은 `regular12_`, 색은 `ScaleAlpha(ClockTextColor(dark), 0.55f)`다. 지금 쓰는 `regular11_`은 다른 패널 어디에도 없는 크기다.

트랙의 좌우 끝은 카드 안쪽 여백 12dip, 아이콘 14dip, 간격 8dip을 더한 자리다.

```
track_l = card.left  + 12 + 14 + 8
track_r = card.right - 12 - 14 - 8
```

**흩어진 매직 넘버를 먼저 모은다.** 지금 `-12`와 `-26`과 `kSliderCardHDip`이 `SliderTrackRect`, `BuildHits`, `Render`의 `draw_slider_card` 세 곳에 각각 박혀 있어서, 한 곳만 고치면 히트 영역과 그림이 어긋난다. 헤더에 카드 사각형을 돌려주는 헬퍼를 새로 선언하고 세 곳이 그것을 공유하게 한다.

```cpp
RECT SliderCardRect(UINT dpi, bool brightness) const;
```

```cpp
RECT ControlCenterContent::SliderCardRect(UINT dpi, bool brightness) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  int y = pad + DipToPx(kConnectHDip + kSectionGapDip + kQuickHDip + kSectionGapDip, dpi);
  if (!brightness && brightness_ok_) {
    y += DipToPx(kSliderCardHDip + kSectionGapDip, dpi);
  }
  return RECT{pad, y, pad + DipToPx(kCardWDip, dpi), y + DipToPx(kSliderCardHDip, dpi)};
}

RECT ControlCenterContent::SliderTrackRect(UINT dpi, bool brightness) const {
  const RECT card = SliderCardRect(dpi, brightness);
  const int side = DipToPx(12 + kVolumeSliderIconDip + kVolumeSliderGapDip, dpi);
  const int row_top = card.top + DipToPx(28, dpi);
  const int y = row_top + (DipToPx(kVolumeSliderRowHDip, dpi) - DipToPx(kVolumeTrackHDip, dpi)) / 2;
  return RECT{card.left + side, y, card.right - side, y + DipToPx(kVolumeTrackHDip, dpi)};
}
```

`BuildHits`의 두 슬라이더 항목은 `add(kBrightness, SliderCardRect(dpi, true))`와 `add(kVolume, SliderCardRect(dpi, false))`로 줄어든다.

좌우 아이콘은 사운드 패널처럼 `DrawGlyphInked`로 그리고 색은 `ClockTextColor(dark)`를 쓴다.

- 사운드: 왼쪽 `kVolSmallGlyph`(E992), 오른쪽 `kVolLoudGlyph`(E995)
- 디스플레이: Segoe Fluent Icons에 밝기 낮음과 높음이 짝을 이루는 글리프가 없다. `kBrightGlyph`(E706) 하나를 양쪽에 쓰되 왼쪽은 11dip, 오른쪽은 14dip 상자에 그려서 크기로 방향을 나타낸다.

트랙과 노브를 그리는 부분은 `RenderVolumePage`의 해당 대목을 그대로 옮겨 쓰면 된다. 사운드 카드는 `muted_`일 때 채움과 노브에 0.40 알파를 먹이고, 값은 `muted_ ? 0.0f : volume_`이 아니라 `volume_`을 그대로 쓴다. 음소거를 값 0으로 뭉개면 음소거를 풀었을 때 노브가 튀어서, 사운드 패널의 표현과도 어긋난다.

`DragTo`의 홈 분기와 볼륨 페이지 분기는 이제 기하가 같아지므로 하나로 합친다.

```cpp
const RECT track = (page_ == Page::kVolume && hit.id == kVolume) ? VolumeSliderTrackRect(dpi)
                                                                 : SliderTrackRect(dpi, hit.id == kBrightness);
const float knob_r = DipToPxF(kVolumeKnobDip, dpi) * 0.5f;
const SliderGeometry geom{static_cast<float>(track.left) + knob_r, static_cast<float>(track.right) - knob_r, knob_r};
```

`SliderGeomThick`을 부르던 자리가 사라지는데, `src/slider_geom.hpp`의 다른 사용처를 확인하고 아무도 안 쓰면 그때 지운다. 이번 작업에서 함부로 지우지 않는다.

### 1.5 글꼴과 색을 패널 쪽에 맞춘다

홈에만 있는 눈금을 패널이 쓰는 눈금으로 옮긴다.

| 자리 | 지금 | 바꿀 값 | 근거 |
| --- | --- | --- | --- |
| 연결 행 제목(Wi-Fi, Bluetooth) | `semibold13_` | `semibold14_` | 패널 제목이 `semibold14_`다 |
| 연결 행 부제(SSID, 켜짐/꺼짐) | `regular11_` | `regular12_` | 패널 섹션 헤더가 `regular12_`다 |
| 빠른 설정 타일 이름 | `regular12_` | `regular14_` | 패널 목록 행이 `regular14_`다 |
| 슬라이더 카드 제목 | `regular11_` | `regular12_` | 1.4절 참고 |
| 슬라이더 트랙 배경 | `ScaleAlpha(fg, 0.12f)` | `BadgeOffFill(dark)` | 패널의 트랙과 토글이 모두 이 색이다 |

배지 원의 지름과 텍스트 시작 위치도 정리한다.

- 연결 행 배지는 34dip을 유지한다. 홈에서 가장 큰 행이고 이 크기가 의도된 강조다. 텍스트 시작은 `rc.left + 12 + 34 + 9 = rc.left + 55`로 옮긴다. 원과 텍스트 사이를 9dip 띄우는 것은 `panel::kTextLeftDip`(49 = 14 + 26 + 9)이 쓰는 규칙과 같다.
- 빠른 설정 타일 배지는 30dip에서 `panel::kCircleDip`(26)으로 줄인다. 텍스트 시작은 `rc.left + 10 + 26 + 9 = rc.left + 45`다.

호버 채움의 모서리 반지름은 지금대로 `corner::kOverlayDip`을 쓴다. 패널은 `corner::HoverPx`를 쓰지만, 홈의 호버는 카드 전체를 덮으므로 카드 모서리를 그대로 따라야 자연스럽다. 이것은 통일 대상이 아니다.

### 1.6 홈 높이

`HeightDip()`의 홈 분기가 위 변경을 반영하도록 고친다. 상수만 바뀌므로 식 자체는 그대로 두면 되지만, 계산 결과를 확인해 둔다.

```
14 + 114 + 10 + 52 + 10 + (60 + 10) + 60 + 10 + 28 + 14 = 382   (디스플레이 슬라이더가 있을 때)
14 + 114 + 10 + 52 + 10 +            60 + 10 + 28 + 14 = 312   (없을 때)
```

타일 두 장이 빠지면서 홈이 62dip 짧아진다.

---

## 2부 — 남은 두 타일을 즉시 적용으로 바꾼다

지금은 네 타일이 모두 설정 앱을 열기만 한다.

```cpp
case kAirplane: OpenSettingsPage(L"ms-settings:network-airplanemode"); break;   // 1.3절에서 지운다
case kSaver:    OpenSettingsPage(L"ms-settings:batterysaver");         break;   // 2.1절
case kNight:    OpenSettingsPage(L"ms-settings:night-light");          break;   // 2.2절
case kAccess:   OpenSettingsPage(L"ms-settings:easeofaccess");         break;   // 1.3절에서 지운다
```

1.3절에서 비행기 모드와 접근성을 지우고 나면 절전 모드와 야간 모드만 남는다. 이 둘을 즉시 적용으로 바꾼다.

절전 모드는 **UI 스레드에서 전원 API를 직접 부르면 안 된다.** `SetBatterySaver`는 전원 정책을 다시 적용하고 플래그가 따라올 때까지 최대 3초를 기다리므로, 그동안 팝업이 멈춘다. 블루투스 라디오 토글이 이미 `StatusEvent`를 위젯 워커 스레드로 넘기는 방식을 쓰고 있으니 같은 길을 따른다. 야간 모드는 레지스트리 읽고 쓰기뿐이라 UI 스레드에서 처리해도 된다.

### 2.1 절전 모드 — 이미 있는 경로를 잇기만 하면 된다

배터리 페이지의 토글이 쓰는 경로가 그대로 살아 있다. `src/power_saver.cpp`의 `SetBatterySaver`가 실제 구현이고, `BuiltinWidgets::OnEvent`가 `bamti.widget/battery` + `toggle` + `saver`를 받아 워커 스레드로 넘긴다. 홈 타일에서 같은 이벤트를 쏘면 끝난다.

`Invoke`의 `case kSaver`를 배터리 페이지 토글(`case kPageToggle`의 `page_ == Page::kBattery` 분기)과 같은 판정으로 바꾼다.

```cpp
case kSaver:
  if (!battery_saver_toggle_ok_) {
    OpenSettingsPage(L"ms-settings:batterysaver");
    break;
  }
  if (battery_ac_) {
    // AC 전원에서는 Windows가 절전 모드를 켜지 않는다. 설정 앱으로 넘긴다.
    OpenSettingsPage(L"ms-settings:batterysaver");
    break;
  }
  if (host_.dispatch) {
    StatusEvent ev;
    ev.id = "bamti.widget/battery";
    ev.event = "toggle";
    ev.row_id = "saver";
    ev.on = !saver_on_;
    host_.dispatch(ev);
  }
  saver_on_ = !saver_on_;
  PresentHost();
  break;
```

`saver_on_`을 미리 뒤집는 것은 낙관적 반영이다. 다음 폴링에서 `ApplyLive`가 실제 상태로 덮으므로, 실패하면 몇백 밀리초 뒤에 원래대로 돌아온다. 이것이 눌러도 아무 반응이 없는 것보다 낫다.

### 2.2 야간 모드 — CloudStore 블롭을 직접 고친다

**여기가 이번 작업에서 가장 위험한 부분이다.** 야간 모드에는 공개 API가 없고, 상태가 HKCU 아래 이진 블롭 하나로만 존재한다.

```
HKCU\Software\Microsoft\Windows\CurrentVersion\CloudStore\Store\DefaultAccount\Current
  \default$windows.data.bluelightreduction.bluelightreductionstate
  \windows.data.bluelightreduction.bluelightreductionstate
값 이름: Data (REG_BINARY)
```

이 개발기에서 야간 모드가 꺼진 상태의 블롭을 읽어 보면 41바이트다.

```
오프셋 0    43 42 01 00 0a 02 01 00 2a 06
오프셋 10   8e 9c 87 cc 06                    ← Unix 초를 담은 varint
오프셋 15   2a 2b 0e 13
오프셋 19   43 42 01 00 d0 0a 02 c6 14 d6 b1 d4 d5 df 9a dc ed 01 00 00 00 00
```

오프셋 10의 varint를 base-128로 풀면 1770114574이고, 이는 2026년 2월의 Unix 시각이다. 즉 야간 모드 상태를 마지막으로 바꾼 순간이 여기에 적혀 있다.

널리 알려진 규칙은 이렇다.

- 야간 모드가 켜지면 오프셋 18에 `10 00` 두 바이트가 끼어들고 전체 길이가 43바이트가 된다.
- 오프셋 10의 시각 varint를 지금 시각으로 갱신하지 않으면 Windows가 변경을 무시한다.

**이 규칙을 믿고 바로 구현하지 말고, 먼저 실측으로 확인한다.** Windows 빌드마다 블롭 형식이 달라질 수 있다.

1. 지금 상태(꺼짐)의 `Data`를 파일로 덤프한다.
2. Windows 설정에서 야간 모드를 손으로 켠다.
3. 켜진 상태의 `Data`를 덤프한다.
4. 두 덤프를 바이트 단위로 비교해서 위 규칙이 맞는지 확인한다. `rtk diff`는 다른 파일을 같다고 보고한 적이 있으므로 `cmp -l`이나 `fc /b`로 비교한다.
5. 다시 손으로 끄고 원래 블롭으로 돌아왔는지 확인한다.

실측이 규칙과 다르면 그 결과를 이 문서에 적고 구현을 맞춘다.

구현은 `src/night_light.hpp`와 `src/night_light.cpp`를 새로 만들어 담는다. `CMakeLists.txt`의 `add_executable` 목록에 `src/night_light.cpp`를 더한다.

```cpp
#pragma once

namespace bamti {

struct NightLightState {
  bool known = false;
  bool on = false;
};

NightLightState QueryNightLight();
// 성공하면 참을 돌려준다. 블롭 형식이 예상과 다르면 아무것도 쓰지 않고 거짓을 돌려준다.
bool SetNightLight(bool on);

}  // namespace bamti
```

구현에서 반드시 지킬 것.

- **쓰기 전에 형식을 검사한다.** 앞 4바이트가 `43 42 01 00`이 아니거나, 길이가 35바이트 미만이거나, 오프셋 15의 `2a`가 자리에 없으면 **아무것도 쓰지 말고** 거짓을 돌려준다. 호출부는 그때 설정 앱을 연다. 형식을 모르는 채로 쓰면 사용자의 디스플레이 설정을 망가뜨린다.
- **원본을 남긴다.** 처음 쓰기를 시도할 때 원본 블롭을 `%LOCALAPPDATA%\bamti\night_light_backup.bin`에 한 번만 저장한다. 경로는 `src/paths.cpp`의 헬퍼를 쓴다. 이미 파일이 있으면 덮어쓰지 않는다.
- 오프셋 10의 varint는 5바이트 고정이 아니라 값에 따라 길이가 달라진다. 최상위 비트가 0인 바이트가 나올 때까지 읽어 끝을 찾고, 새 시각을 같은 방식으로 인코딩해 그 구간을 통째로 갈아 끼운다. 길이가 달라질 수 있으므로 뒤쪽을 밀어야 한다. `10 00`의 삽입 위치도 varint 길이에 따라 움직이니 오프셋 18을 상수로 박지 말고 varint 끝에서부터 계산한다.
- `RegSetValueExW`로 `REG_BINARY`를 쓴다. 키가 없으면 만들지 말고 거짓을 돌려준다.

**상태 읽기는 위젯 스레드까지 갈 것 없이 제어센터 안에서 처리한다.** 레지스트리에서 41바이트를 읽는 일이라 UI 스레드에서 불러도 무방하고, `ControlCenterContent::QuerySlowState`가 마침 `kSlowPeriodMs`(2초)마다 도는 자리인데 지금은 `slow_due_`만 갱신하는 빈 껍데기다. 여기에 읽기를 넣으면 `ControlCenterLive`와 `src/widgets/builtin.cpp`를 건드릴 필요가 없다.

```cpp
void ControlCenterContent::QuerySlowState(bool force) {
  const ULONGLONG now = GetTickCount64();
  if (!force && now < slow_due_) {
    return;
  }
  slow_due_ = now + kSlowPeriodMs;
  const NightLightState night = QueryNightLight();
  night_known_ = night.known;
  night_on_ = night.on;
}
```

`src/control_center.hpp`의 비공개 멤버에 `bool night_known_ = false;`와 `bool night_on_ = false;`를 더한다.

`Invoke`의 `case kNight`는 이렇게 된다.

```cpp
case kNight:
  if (!night_known_ || !SetNightLight(!night_on_)) {
    OpenSettingsPage(L"ms-settings:night-light");
    break;
  }
  night_on_ = !night_on_;
  PresentHost();
  break;
```

`Render`의 `quick[]`에서 이 타일의 `on`도 `night_on_`으로 바꾼다.

---

## 검증

빌드하고 앱을 띄운 뒤 다음을 확인한다.

1. 홈 팝업 폭이 308dip이고, Wi-Fi를 눌러 패널로 들어갔다 나와도 폭이 변하지 않는다.
2. Wi-Fi 카드와 Bluetooth 카드 사이에 10dip 간격이 보이고, 마우스를 올리면 각 카드만 따로 밝아진다.
3. 빠른 설정에 절전 모드와 야간 모드 두 장만 한 줄로 놓여 있다. 비행기 모드와 접근성이 있던 자리에 빈 공간이나 잘린 카드가 남아 있지 않다.
4. 디스플레이와 사운드 슬라이더가 사운드 패널의 슬라이더와 같은 두께, 같은 노브, 같은 좌우 아이콘 배치를 갖는다. 두 화면을 나란히 놓고 눈으로 비교한다.
5. 슬라이더를 끌면 값이 따라오고, 트랙 양 끝에서 0과 100에 정확히 닿는다. 카드 어느 곳을 눌러도 드래그가 시작된다.
6. 절전 모드 타일을 누르면 설정 앱이 뜨지 않고 배지가 강조색으로 바뀐다. Windows 설정의 배터리 절약 모드 표시도 함께 바뀐다. AC 전원이면 설정 앱이 뜬다.
7. 야간 모드 타일을 누르면 화면 색온도가 즉시 따뜻해지고, Windows 설정의 야간 모드 스위치도 켜져 있다. 다시 누르면 돌아온다.
8. `%LOCALAPPDATA%\bamti\night_light_backup.bin`이 만들어져 있고 크기가 41바이트다.

**주의.** 검증 절차에 레지스트리 쓰기를 임의로 끼워 넣지 않는다. 야간 모드 확인은 앱의 타일을 눌러서만 하고, 되돌릴 때도 타일이나 Windows 설정을 쓴다. 레지스트리를 손으로 편집해 상태를 만들지 않는다.

빌드 경고는 `/W4`에서 0개를 유지한다. 특히 1.3절에서 지운 글리프 상수와 열거값이 남아 있지 않은지 확인한다.

---

## 작업 순서

1부와 2부는 서로 독립적이다. 1부를 먼저 끝내고 화면을 확인한 뒤 2부로 넘어가면 문제가 생겼을 때 원인을 가리기 쉽다.

1부 안에서는 타일을 지우는 1.3절을 폭을 줄이는 1.1절 바로 뒤에 처리하는 편이 낫다. 격자 상수가 한 번에 정리되고, 그 뒤의 슬라이더 작업이 확정된 좌표 위에서 진행된다.

2부 안에서는 절전 모드(2.1)를 먼저 하고 야간 모드(2.2)를 나중에 한다. 절전 모드는 이미 있는 배선을 잇는 일이고, 야간 모드는 레지스트리를 직접 고치는 일이라 위험이 훨씬 크다.
