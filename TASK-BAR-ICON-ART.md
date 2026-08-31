# 작업 지시서: 상단바 아이콘을 Windows 11의 모양으로 다시 그린다

`FIX-MENU-DISMISS.md` 다음에 하십시오.

---

## 0. 완료 조건

1. 시작 단추의 Windows 로고가 셸이 쓰는 그라데이션 로고와 같아집니다.
2. 배터리 아이콘이 Windows 11 배터리 모양이 되고, 잔량과 상태에 따라 색이 붙습니다.
3. CPU 아이콘이 사용률만큼 채워지는 고리가 되고, 사용률 구간에 따라 색이 바뀝니다.
4. 볼륨 아이콘이 Windows 11 스피커 아이콘이 됩니다.
5. 네트워크 표시에 `B/s` 단위가 붙고, 패널의 `bamti 시작 이후` 문구가 사라집니다.
6. 배터리 패널의 잔량 막대에도 같은 색이 붙습니다.

---

## 1. 건드리는 파일

| 파일 | 하는 일 |
|---|---|
| `src/status_item.hpp` | `IconKind::kVector`와 그리기 인자를 더한다 |
| `src/theme.hpp` / `src/theme.cpp` | 잔량과 사용률의 색을 한 곳에 모은다 |
| `src/bar_layout.cpp` | 벡터 아이콘이 아이콘 자리를 차지하게 한다 |
| `src/clock_renderer.hpp` / `.cpp` | 실제 그리기. 로고, 배터리, 고리, 글리프 아이콘 |
| `src/widgets/builtin.cpp` | 각 위젯이 벡터 아이콘을 쓰게 한다. 네트워크 문구 |
| `src/status_panel.cpp` | 배터리 게이지 색 |
| `docs/STATUS-PROTOCOL.md` | 벡터 아이콘이 내장 전용임을 적는다 |

---

## 2. 지금 구조로는 왜 안 되는가

상단바의 아이콘은 전부 **글자**입니다. `src/widgets/builtin.cpp`가 유니코드 문자를 아이콘으로 넣습니다.

```cpp
constexpr wchar_t kBatteryGlyphs[] = L"▁▃▅▇█";
constexpr wchar_t kCpuGlyph[] = L"▦";
constexpr wchar_t kNetGlyph[] = L"⇅";
constexpr wchar_t kVolumeGlyph[] = L"♪";
```

`StatusBarText`가 이 글자를 항목 텍스트 앞에 붙이고, `bar_layout.cpp`가 합친 문자열의 폭을 재고, `clock_renderer.cpp`가 `DrawTextLayout` 한 번으로 그립니다. 그래서 아이콘 전체가 **한 가지 색**입니다. 잔량만 초록으로 칠하거나 사용률만큼 고리를 채우는 일이 구조적으로 불가능합니다.

비트맵 경로(`IconKind::kPng` 등)는 이미 있지만, 값이 바뀔 때마다 비트맵을 새로 만들고 캐시를 갈아 끼우게 되므로 1초마다 값이 바뀌는 위젯에는 맞지 않습니다.

그래서 **직접 그리는 아이콘 종류를 하나 더 만듭니다.**

---

## 3. 벡터 아이콘 파이프라인

### 3-1. `src/status_item.hpp`

```cpp
enum class IconKind { kNone, kGlyph, kPng, kFile, kHicon, kVector };

enum class VectorIcon : uint8_t {
  kNone = 0,
  kBattery,
  kCpu,
  kNetwork,
  kVolume,
};

inline constexpr uint32_t kVectorFlagCharging = 1u << 0;  // 배터리 충전 중
inline constexpr uint32_t kVectorFlagMuted    = 1u << 1;  // 볼륨 음소거

struct StatusIcon {
  IconKind kind = IconKind::kNone;
  std::wstring glyph;
  std::vector<uint8_t> bytes;
  std::wstring path;
  HICON hicon = nullptr;
  VectorIcon vector = VectorIcon::kNone;
  float value = 0.0f;      // 0.0~1.0. 잔량, 사용률, 볼륨 레벨
  uint32_t flags = 0;
  uint64_t cache_key = 0;
};
```

`HashStatusIcon`에 `kVector` 갈래를 더합니다. **`value`와 `flags`를 반드시 해시에 넣으십시오.** `menu_bar.cpp`의 세그먼트 비교가 `icon_key`로 변화를 감지하므로, 값이 바뀌었는데 키가 그대로면 화면이 갱신되지 않습니다.

```cpp
case IconKind::kVector: {
  const uint8_t v = static_cast<uint8_t>(icon.vector);
  hash = Fnv1a64(&v, 1, hash);
  hash = Fnv1a64(reinterpret_cast<const uint8_t*>(&icon.value), sizeof(icon.value), hash);
  return Fnv1a64(reinterpret_cast<const uint8_t*>(&icon.flags), sizeof(icon.flags), hash);
}
```

`StatusBarText`도 고칩니다. 벡터 아이콘은 글자가 아니므로 텍스트 앞에 붙이면 안 됩니다.

```cpp
inline std::wstring StatusBarText(const StatusItem& item) {
  if (item.icon.kind == IconKind::kVector) {
    return item.text;
  }
  const bool glyph = item.icon.kind == IconKind::kGlyph && !item.icon.glyph.empty();
  ...
}
```

### 3-2. `src/bar_layout.cpp`

`IconIsBitmap`이 벡터를 포함하도록 넓힙니다. 이름도 뜻에 맞게 바꾸십시오.

```cpp
bool IconHasArt(IconKind kind) {
  return kind == IconKind::kPng || kind == IconKind::kFile || kind == IconKind::kHicon ||
         kind == IconKind::kVector;
}
```

호출하는 곳을 모두 바꿉니다(21행, 28행, 212행 부근). 아이콘 폭 `kStatusIconDip`과 텍스트 사이 간격 `kStatusIconGapDip`은 그대로 씁니다. 벡터 아이콘도 같은 16dip 자리를 차지합니다.

`seg.icon`에 아이콘을 복사하는 조건도 `IconHasArt`로 바꾸십시오. 그래야 `value`와 `flags`가 렌더러까지 전달됩니다.

### 3-3. `src/clock_renderer.cpp`

`Draw`의 세그먼트 반복에서 비트맵 분기 옆에 벡터 분기를 둡니다.

```cpp
if (seg.icon_kind == IconKind::kVector) {
  const float icon_top = (height_dip - 16.0f) * 0.5f;
  DrawVectorIcon(brush.Get(), seg.icon, D2D1::RectF(x0, icon_top, x0 + 16.0f, icon_top + 16.0f), dark);
  text_x = x0 + 16.0f + 4.0f;
} else if (bitmap) {
  // 기존 코드
}
```

`DrawVectorIcon`은 `ClockRenderer`의 비공개 멤버 함수로 두십시오. 렌더 타깃(`rt_`)과 DPI가 필요합니다.

```cpp
void ClockRenderer::DrawVectorIcon(ID2D1SolidColorBrush* brush, const StatusIcon& icon, const D2D1_RECT_F& box,
                                   bool dark);
```

`box`는 16×16 DIP입니다. 이 안에서만 그리십시오. 밖으로 나가면 옆 항목과 겹칩니다.

---

## 4. 시작 단추의 Windows 로고

### 4-1. 지금 무엇이 다른가

현재 `FillWindowsLogo`는 `C:\Windows\System32\@WLOGO_96x96.png`를 재현합니다. 그 파일을 실제로 열어 픽셀을 확인했습니다.

| 항목 | 값 |
|---|---|
| 그림 영역 | 96×96 안에서 (8,8)~(87,87), 즉 80×80 |
| 타일 | 38×38 네 개 |
| 간격 | 4px |
| 색 | `#0078D4` **단색**. 그라데이션 없음 |
| 모서리 | 각짐 |

지금 코드는 이 파일을 정확히 따르고 있으므로 버그가 아닙니다. 다만 **이 파일은 OOBE 화면용이고, 셸이 실제로 쓰는 로고가 아닙니다.**

셸이 쓰는 로고는 다음 파일에 있고, 그라데이션이 들어 있습니다.

```
C:\Windows\SystemApps\MicrosoftWindows.Client.Core_cw5n1h2txyewy\Icons\WindowsLogo.svg
```

내용 전체입니다.

```xml
<svg xmlns="http://www.w3.org/2000/svg" width="12" height="12" viewBox="0 0 12 12" fill="none">
  <path d="M5.5 11.5H0.5C0.223858 11.5 8.05325e-09 11.2761 0 11V6H5.5V11.5ZM11.5 11C11.5 11.2761 11.2761 11.5 11 11.5H6V6H11.5V11ZM5.5 5.5H0V0.5C0 0.223858 0.223858 8.05318e-09 0.5 0H5.5V5.5ZM11 0C11.2761 0 11.5 0.223858 11.5 0.5V5.5H6V0H11Z" fill="url(#paint0_linear_72_1416)"/>
  <defs>
    <linearGradient id="paint0_linear_72_1416" x1="1.74259" y1="3.09048e-07" x2="9.23502" y2="11.5" gradientUnits="userSpaceOnUse">
      <stop stop-color="#4DD2FF"/>
      <stop offset="0.75" stop-color="#0078D4"/>
    </linearGradient>
  </defs>
</svg>
```

여기서 읽어야 할 사양입니다.

| 항목 | SVG 좌표 | 전체(11.5) 대비 비율 |
|---|---|---|
| 타일 한 변 | 5.5 | 47.83% |
| 타일 사이 간격 | 0.5 | 4.35% |
| 바깥 모서리 반지름 | 0.5 | 4.35% |
| 그라데이션 시작점 | (1.74259, 0) | (15.15%, 0%) |
| 그라데이션 끝점 | (9.23502, 11.5) | (80.30%, 100%) |
| 색 정지점 | `#4DD2FF` at 0.0, `#0078D4` at 0.75 | |

지금 코드의 간격은 `4.0f / 80.0f`, 즉 5.00%입니다. 실제 로고는 4.35%입니다. **사용자가 크로스 라인이 두껍다고 본 것이 이 차이입니다.**

모서리는 **네 타일의 바깥쪽 한 귀퉁이만** 둥급니다. 가운데 십자를 향한 세 모서리는 각져 있습니다. 위 `path`를 보면 좌하단 타일이 `(0, 11)` 부근에서만 곡선을 그리고 나머지는 직선입니다.

### 4-2. 어떻게 그리는가

`FillWindowsLogo`를 다시 씁니다. 픽셀 격자에 맞추던 정수 반올림은 **버리십시오.** 그라데이션과 둥근 모서리가 들어가면 어차피 안티에일리어싱이 필요하고, 정수 맞춤은 간격 비율을 망칩니다.

DIP 좌표로 그대로 계산합니다. 로고 한 변을 `S`(= `kStartLogoDip`, 현재 20dip)라 하면 다음과 같습니다.

```
tile = S * 5.5f / 11.5f
gap  = S * 0.5f / 11.5f
r    = S * 0.5f / 11.5f
```

네 타일의 좌상단은 `(0,0)`, `(tile+gap, 0)`, `(0, tile+gap)`, `(tile+gap, tile+gap)`입니다.

각 타일을 `ID2D1PathGeometry`로 만듭니다. 한 귀퉁이만 둥근 사각형이므로 `ID2D1GeometrySink`에 직선 네 개와 호 하나를 넣습니다. 좌상단 타일을 예로 들면 둥근 곳은 좌상단 귀퉁이입니다.

```cpp
// (x, y)가 타일의 좌상단, w가 한 변, r이 반지름
sink->BeginFigure(D2D1::Point2F(x + r, y), D2D1_FIGURE_BEGIN_FILLED);
sink->AddLine(D2D1::Point2F(x + w, y));
sink->AddLine(D2D1::Point2F(x + w, y + w));
sink->AddLine(D2D1::Point2F(x, y + w));
sink->AddLine(D2D1::Point2F(x, y + r));
sink->AddArc(D2D1::ArcSegment(D2D1::Point2F(x + r, y), D2D1::SizeF(r, r), 0.0f,
                              D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
sink->EndFigure(D2D1_FIGURE_END_CLOSED);
```

나머지 세 타일은 둥글릴 귀퉁이가 각각 우상단, 좌하단, 우하단입니다. 네 갈래를 나열해도 좋고 귀퉁이를 인자로 받는 헬퍼를 하나 만들어도 좋습니다. 읽기 쉬운 쪽을 고르십시오.

**네 타일을 하나의 `ID2D1PathGeometry`에 네 개의 figure로 넣으십시오.** 그래야 그라데이션이 로고 전체를 가로질러 한 번에 걸립니다. 타일마다 따로 채우면 각 타일 안에서 그라데이션이 반복되어 전혀 다른 그림이 됩니다.

### 4-3. 그라데이션 브러시

```cpp
D2D1_GRADIENT_STOP stops[2];
stops[0].position = 0.0f;
stops[0].color = D2D1::ColorF(0x4DD2FF);
stops[1].position = 0.75f;
stops[1].color = D2D1::ColorF(0x0078D4);
```

`CreateGradientStopCollection`으로 모으고 `CreateLinearGradientBrush`에 시작점과 끝점을 줍니다. 좌표는 로고 좌상단을 원점으로 한 DIP 값입니다.

```
start = (S * 1.74259f / 11.5f, 0.0f)
end   = (S * 9.23502f / 11.5f, S * 11.5f / 11.5f)
```

정지점이 0.75에서 끝나므로 그 뒤 구간은 마지막 색이 이어집니다. D2D의 기본 확장 모드가 `D2D1_EXTEND_MODE_CLAMP`이므로 따로 지정하지 않아도 같은 결과입니다.

### 4-4. 캐시

지오메트리와 그라데이션 브러시를 매 프레임 새로 만들면 안 됩니다. 상단바는 시계 때문에 1초마다 다시 그려집니다.

`ClockRenderer`의 멤버로 들고, `DropTarget()`에서 함께 버리십시오. 브러시는 렌더 타깃에 묶이므로 반드시 함께 버려야 합니다. 지오메트리는 팩터리에 묶이지만 로고 크기가 바뀌면 다시 만들어야 하므로, 만들 때 쓴 크기를 함께 기억해 두었다가 달라지면 다시 만드십시오.

```cpp
Microsoft::WRL::ComPtr<ID2D1PathGeometry> logo_geom_;
Microsoft::WRL::ComPtr<ID2D1LinearGradientBrush> logo_brush_;
float logo_geom_size_ = 0.0f;
```

### 4-5. 좌표계 주의

지오메트리를 로고 좌상단이 원점인 좌표로 만들고, 그릴 때 `SetTransform`으로 옮기면 크기가 바뀔 때만 다시 만들면 됩니다. 다만 `Draw`가 이미 스크롤 보정용 변환을 걸어 두었으므로, **기존 변환을 읽어서 곱하고 끝나면 되돌려 놓으십시오.** 덮어쓰면 로고가 엉뚱한 자리에 그려집니다.

```cpp
D2D1_MATRIX_3X2_F saved{};
rt_->GetTransform(&saved);
rt_->SetTransform(D2D1::Matrix3x2F::Translation(logo_left, logo_top) * saved);
rt_->FillGeometry(logo_geom_.Get(), logo_brush_.Get());
rt_->SetTransform(saved);
```

---

## 5. 배터리 아이콘

### 5-1. 모양

16×16 DIP 상자 안에 가로로 눕힌 배터리를 그립니다. Windows 11 작업 표시줄의 배터리와 같은 비율입니다.

| 부분 | 좌표(상자 좌상단 기준, DIP) |
|---|---|
| 몸통 바깥 | `(0.5, 4.5)`~`(13.5, 11.5)`, 모서리 반지름 `1.5` |
| 몸통 테두리 두께 | `1.0` |
| 단자 | `(14.0, 6.75)`~`(15.5, 9.25)`, 모서리 반지름 `0.75` |
| 채움 영역 | 몸통 안쪽에서 `1.0`씩 들여쓴 사각형, 모서리 반지름 `0.75` |

채움은 안쪽 사각형의 왼쪽 끝에서 `잔량 × 안쪽 폭`만큼 그립니다. 잔량이 0이면 채우지 않습니다. 잔량이 아주 작을 때 1픽셀도 안 되는 막대가 남지 않도록, 채울 폭이 `0.5` DIP 미만이면 그리지 마십시오.

테두리와 단자는 항상 **텍스트 색**으로 그립니다(`ClockTextColor(dark)`). 채움만 상태 색을 씁니다. Windows 11이 그렇게 그립니다.

### 5-2. 충전 중 표시

충전 중이면(`kVectorFlagCharging`) 몸통 위에 번개를 겹쳐 그립니다. 번개는 채움과 테두리 위에 그리고, 배경과 구분되도록 **테두리 색과 대비되는 색**으로 칠하십시오. 밝은 테마에서는 흰색, 어두운 테마에서는 채움 위에서도 보이도록 진한 색입니다.

번개는 꼭짓점 여섯 개짜리 다각형이면 충분합니다. 상자 기준 대략 다음 점들을 잇습니다.

```
(7.6, 5.6) (5.4, 8.6) (6.9, 8.6) (6.4, 11.4) (8.6, 8.2) (7.1, 8.2)
```

이 값은 눈으로 맞춘 시작점입니다. 실제 화면에서 배터리 몸통 안에 들어가는지 보고 조정하십시오. 몸통 밖으로 나가면 안 됩니다.

### 5-3. 색

`src/theme.hpp`에 함수를 더합니다. 배터리 패널의 게이지도 같은 색을 써야 하므로 한 곳에 두어야 합니다.

```cpp
D2D1_COLOR_F BatteryFillColor(bool dark, float level, bool charging);
uint32_t BatteryFillRgb(bool dark, float level, bool charging);
```

| 조건 | 밝은 테마 | 어두운 테마 |
|---|---|---|
| 충전 중 | `#0F7B0F` | `#5BC85B` |
| 잔량 10% 이하 | `#C42B1C` | `#FF99A4` |
| 잔량 20% 이하 | `#9D5D00` | `#FCE100` |
| 그 밖 | `#0F7B0F` | `#5BC85B` |

충전 중이면 잔량과 무관하게 초록입니다. Windows 11이 그렇게 합니다.

### 5-4. 위젯 쪽 변경

`src/widgets/builtin.cpp`의 `SampleBattery`에서 `SetGlyph` 호출을 지우고 벡터 아이콘을 넣습니다.

```cpp
item.icon.kind = IconKind::kVector;
item.icon.vector = VectorIcon::kBattery;
item.icon.value = static_cast<float>(pct) / 100.0f;
item.icon.flags = charging ? kVectorFlagCharging : 0;
item.icon.cache_key = HashStatusIcon(item.icon);
```

`SetGlyph`처럼 아이콘을 채우고 해시까지 갱신하는 작은 헬퍼 `SetVectorIcon(item, vector, value, flags)`를 익명 이름공간에 두고 네 위젯이 함께 쓰십시오. **`cache_key` 갱신을 빠뜨리면 화면이 갱신되지 않습니다.**

`kBatteryGlyphs` 상수는 지우십시오.

### 5-5. 배터리 패널의 게이지

`src/status_panel.cpp`의 게이지 채움 색은 지금 항목의 `accent`나 기본 강조색을 씁니다.

```cpp
const D2D1_COLOR_F fill_c = item_.accent != 0 ? D2D1::ColorF(item_.accent) : DockIndicatorColor(dark);
```

`accent`가 0이 아니면 상단바의 **텍스트 색까지** 바뀝니다(`clock_renderer.cpp`의 `StatusItemColor`). 배터리 퍼센트 글자가 초록이 되는 것은 원하는 결과가 아닙니다.

그러므로 `accent`를 쓰지 말고, `StatusRow`에 색을 실어 보내십시오. `StatusRow`에 `uint32_t fill_rgb = 0;`을 더하고, 0이 아니면 게이지 채움에 그 색을 쓰게 합니다. 배터리 위젯의 `GaugeRow`에 `BatteryFillRgb`가 돌려준 값을 넣습니다.

이 필드는 **내장 위젯 전용입니다.** 파이프 파서에 추가하지 마십시오.

---

## 6. CPU 고리

### 6-1. 모양

16×16 DIP 상자 가운데에 고리를 그립니다.

| 항목 | 값(DIP) |
|---|---|
| 중심 | `(8.0, 8.0)` |
| 반지름(고리 중심선) | `5.5` |
| 두께 | `2.0` |

배경 고리를 먼저 그립니다. 텍스트 색의 알파를 `0.18`로 낮춘 색으로 원을 한 바퀴 그립니다. 그 위에 사용률만큼 호를 겹칩니다.

호는 **12시에서 시작해 시계 방향**으로 `사용률 × 360도`만큼 돕니다.

`ID2D1PathGeometry`에 `AddArc`로 그리고, `ID2D1StrokeStyle`로 끝을 둥글게(`D2D1_CAP_STYLE_ROUND`) 하십시오. 그래야 로딩 고리처럼 보입니다.

```cpp
const float a = value * 6.28318530718f;   // 라디안
const D2D1_POINT_2F start{cx, cy - r};
const D2D1_POINT_2F end{cx + r * std::sin(a), cy - r * std::cos(a)};
sink->BeginFigure(start, D2D1_FIGURE_BEGIN_HOLLOW);
sink->AddArc(D2D1::ArcSegment(end, D2D1::SizeF(r, r), 0.0f, D2D1_SWEEP_DIRECTION_CLOCKWISE,
                              value > 0.5f ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL));
sink->EndFigure(D2D1_FIGURE_END_OPEN);
```

**두 가지 경계를 반드시 처리하십시오.**

- 사용률이 0에 가까우면 호를 그리지 마십시오. 시작점과 끝점이 같아지면 D2D가 호를 그리지 못하거나 한 바퀴를 다 그립니다. 값이 `0.005` 미만이면 배경 고리만 남기십시오.
- 사용률이 1에 가까우면 시작점과 끝점이 다시 같아집니다. 값이 `0.995` 이상이면 호 대신 `DrawEllipse`로 온전한 원을 그리십시오.

`D2D1_FIGURE_BEGIN_HOLLOW`와 `D2D1_FIGURE_END_OPEN`을 쓰고 `DrawGeometry`로 그립니다. `FillGeometry`가 아닙니다.

### 6-2. 색

`src/theme.hpp`에 더합니다.

```cpp
D2D1_COLOR_F CpuRingColor(bool dark, float usage);
```

| 사용률 | 밝은 테마 | 어두운 테마 |
|---|---|---|
| 30% 미만 | `#0F7B0F` | `#5BC85B` |
| 60% 미만 | `#9D5D00` | `#FCE100` |
| 85% 미만 | `#C4520A` | `#FF8C00` |
| 그 이상 | `#C42B1C` | `#FF99A4` |

경계에서 색이 튀는 것은 정상입니다. 보간하지 마십시오. 사용자가 구간을 눈으로 읽을 수 있어야 합니다.

### 6-3. 스트로크 스타일 캐시

`ID2D1StrokeStyle`은 팩터리에 묶이고 상태가 없으므로 한 번 만들어 `ClockRenderer` 멤버로 들고 계십시오. 매 프레임 만들면 안 됩니다.

### 6-4. 위젯 쪽 변경

`SampleCpu`에서 `SetGlyph(&item, kCpuGlyph)`를 `SetVectorIcon(&item, VectorIcon::kCpu, static_cast<float>(pct) / 100.0f, 0)`으로 바꾸십시오. `kCpuGlyph` 상수는 지웁니다.

CPU 표본 주기가 5초(`kCpuPeriodMs`)이므로 고리도 5초마다 움직입니다. **주기를 줄이지 마십시오.** 상주 비용 예산이 걸려 있습니다.

---

## 7. 볼륨 아이콘과 네트워크 아이콘

### 7-1. Segoe Fluent Icons를 쓴다

이 컴퓨터에 `Segoe Fluent Icons`가 설치되어 있는 것을 확인했습니다(`C:\Windows\Fonts\SegoeIcons.ttf`). Windows 11의 셸이 쓰는 바로 그 아이콘 폰트입니다. 스피커와 네트워크는 색을 나눠 칠할 필요가 없으므로, 직접 그리지 말고 이 폰트의 글리프를 그리십시오.

| 아이콘 | 코드포인트 | 조건 |
|---|---|---|
| 음소거 | `U+E74F` | `kVectorFlagMuted` |
| 볼륨 0 | `U+E992` | 레벨 0 |
| 볼륨 1 | `U+E993` | 0 초과 33% 이하 |
| 볼륨 2 | `U+E994` | 33% 초과 66% 이하 |
| 볼륨 3 | `U+E995` | 66% 초과 |
| 네트워크 | `U+E839` | 항상 |

### 7-2. 왜 항목 텍스트에 그냥 붙이면 안 되는가

지금처럼 글리프를 텍스트 앞에 붙이면 `bar_layout.cpp`의 텍스트 포맷(`Segoe UI` 계열)으로 그려집니다. 그 폰트에는 이 코드포인트가 없으므로 대체 글꼴로 넘어가거나 두부(`□`)가 나옵니다. **아이콘 전용 텍스트 포맷을 따로 만들어야 합니다.**

`ClockRenderer`에 `IDWriteTextFormat`을 하나 더 들고, 글꼴 이름을 `Segoe Fluent Icons`, 크기를 16dip, 가로·세로 정렬을 가운데로 두십시오. `DrawVectorIcon`의 볼륨과 네트워크 갈래는 이 포맷으로 `DrawText`를 한 번 부르면 끝입니다. 색은 텍스트 색을 그대로 씁니다.

`ClockRenderer`는 `IDWriteFactory`를 들고 있지 않습니다. `BarLayout`이 들고 있으므로, `ClockRenderer::Initialize`에서 `DWriteCreateFactory`를 한 번 더 부르십시오. 공유 팩터리이므로 비용이 거의 없습니다.

### 7-3. 폰트가 없을 때

`Segoe Fluent Icons`가 없는 환경이 있을 수 있습니다. 텍스트 포맷 생성이 실패하면 **기존 글리프 문자로 되돌아가십시오.** 상단바에 두부가 뜨는 것보다 낫습니다. 되돌아간 사실은 시작 로그에 한 번만 남기십시오.

```cpp
Log(L"bar", L"Segoe Fluent Icons missing; using fallback glyphs");
```

### 7-4. 위젯 쪽 변경

`SampleVolume`에서 벡터 아이콘을 씁니다.

```cpp
item.icon.kind = IconKind::kVector;
item.icon.vector = VectorIcon::kVolume;
item.icon.value = state.level;
item.icon.flags = state.muted ? kVectorFlagMuted : 0;
item.icon.cache_key = HashStatusIcon(item.icon);
```

`SampleNet`은 `VectorIcon::kNetwork`, 값은 0으로 둡니다.

`kVolumeGlyph`, `kVolumeMuteGlyph`, `kNetGlyph` 상수는 7-3의 되돌림 경로에서 계속 쓰이므로 **지우지 말고 그대로 두십시오.** 되돌림에 쓰는 값이라는 주석을 한 줄 붙이십시오.

`kBoardGlyph`(위젯 보드 단추)는 이번 범위가 아닙니다. 그대로 두십시오.

---

## 8. 네트워크 표시

### 8-1. 상단바에 단위를 붙인다

지금은 축약 표기만 나옵니다.

```cpp
const std::wstring in_c = FormatScaled(in_bps, ScaleStyle::kCompact);   // "12K"
const std::wstring out_c = FormatScaled(out_bps, ScaleStyle::kCompact);
std::wstring text = in_c;
text += L'/';
text += out_c;                                                          // "12K/3K"
```

`12K`가 초당 12킬로바이트인지 누적 12킬로바이트인지 화면만 봐서는 알 수 없습니다. 단위를 붙이십시오.

`ScaleStyle`에 갈래를 하나 더합니다. 접미사만 다르고 나머지는 `kCompact`와 같습니다.

```cpp
enum class ScaleStyle { kCompact, kCompactPerSec, kBytes, kBytesPerSec };
```

`kCompactPerSec`은 `12KB/s`, `980B/s`처럼 만듭니다. 즉 `kCompact`의 접미사 뒤에 `B/s`를 붙인 것입니다.

구분자로 슬래시를 쓰지 마십시오. 단위 안에 이미 슬래시가 있어서 `12KB/s/3KB/s`가 되면 읽을 수 없습니다. 가운뎃점을 쓰십시오.

```cpp
std::wstring text = FormatScaled(in_bps, ScaleStyle::kCompactPerSec);
text += L" · ";
text += FormatScaled(out_bps, ScaleStyle::kCompactPerSec);
```

`kStatusTextMaxChars`가 32자이므로 넘칠 걱정은 없습니다. 다만 상단바에서 차지하는 폭이 넓어지므로, 실제 화면에서 시계와 부딪히지 않는지 확인하십시오. 부딪히면 넘침 팝업으로 밀려나는 것이 정상 동작입니다.

### 8-2. 패널에서 문구를 뺀다

```cpp
std::wstring since = L"bamti 시작 이후 받기 ";
```

`bamti 시작 이후`라는 앞머리를 지웁니다. 누적값 자체는 쓸모가 있으니 행은 남기고 문구만 바꾸십시오.

```cpp
std::wstring since = L"누적 받기 ";
since += FormatScaled(static_cast<double>(session_in), ScaleStyle::kBytes);
since += L" · 보내기 ";
since += FormatScaled(static_cast<double>(session_out), ScaleStyle::kBytes);
```

누적량에는 `kCompact`가 아니라 `kBytes`가 맞습니다. 지금 코드가 `kCompact`를 쓰고 있어서 누적 240메가바이트가 `240M`으로만 나옵니다. 함께 고치십시오.

`session_in_`과 `session_out_`을 세는 코드는 건드리지 마십시오.

---

## 9. 하지 말아야 할 것

- 아이콘 크기 `kStatusIconDip`(16dip)과 간격 `kStatusIconGapDip`을 바꾸지 마십시오. 트레이 미러 아이콘과 폭이 어긋납니다.
- 벡터 아이콘과 `StatusRow::fill_rgb`를 파이프 프로토콜에 노출하지 마십시오. 외부 앱이 아이콘 종류를 지정하게 하면 종류를 늘릴 때마다 프로토콜이 깨집니다. `docs/STATUS-PROTOCOL.md`에 **내장 위젯 전용이며 파이프로 받지 않는다**고 한 줄 적으십시오. `pipe_server.cpp`의 파서는 손대지 마십시오.
- 그리기 결과를 비트맵으로 캐시하지 마십시오. 값이 자주 바뀌므로 캐시가 매번 무효가 됩니다. 도형 몇 개를 직접 그리는 편이 쌉니다.
- CPU와 볼륨의 표본 주기를 바꾸지 마십시오.
- 상단바의 다시 그리기 주기를 늘리지 마십시오. `ArmRepaint`와 `RefreshLayout`의 판정을 건드리지 마십시오.
- `@WLOGO_96x96.png`나 `WindowsLogo.svg`를 실행 시간에 읽지 마십시오. 값은 이 문서에 적힌 대로 코드에 상수로 넣습니다. 시스템 파일에 의존하면 파일이 바뀌거나 없을 때 로고가 사라집니다.
- 레지스트리에 쓰지 마십시오.

---

## 10. 검증

### 10-1. 빌드와 화면

1. Release 빌드가 경고 없이 통과합니다.
2. 시작 단추의 로고가 왼쪽 위는 밝은 하늘색, 오른쪽 아래는 진한 파랑인 그라데이션입니다. 네 타일 사이 간격이 이전보다 눈에 띄게 얇습니다. 로고 바깥 네 귀퉁이가 살짝 둥글고 가운데 십자 쪽 모서리는 각져 있습니다.
3. 배터리 아이콘이 가로로 눕힌 배터리 모양이고 안쪽 채움이 잔량만큼입니다. 전원을 뽑으면 채움이 줄어드는 것을 60초 안에 볼 수 있습니다.
4. 전원을 연결하면 배터리 안에 번개가 나타나고 채움이 초록이 됩니다.
5. CPU 아이콘이 고리이고, 부하를 걸면 채워진 각도가 커지고 색이 초록에서 노랑, 주황으로 넘어갑니다.
6. 볼륨 아이콘이 스피커 모양이고, 볼륨을 0으로 내리면 음파가 사라지고, 음소거하면 X 표시가 붙은 모양으로 바뀝니다.
7. 네트워크 항목이 `12KB/s · 3KB/s` 꼴로 보입니다.
8. 네트워크 패널에서 `bamti 시작 이후`라는 문구가 사라졌고 누적값이 `MB`나 `GB` 단위로 보입니다.
9. 배터리 패널의 잔량 막대 색이 아이콘 채움 색과 같고, 상단바의 퍼센트 글자는 테마 색 그대로입니다.
10. 밝은 테마와 어두운 테마를 모두 확인합니다. 설정에서 앱 모드를 바꾸고 상단바가 따라오는지 봅니다.

### 10-2. 그리기 비용

`NotePerf`가 남기는 `draw_ms`를 봅니다. 아이콘을 전부 켠 상태에서 **수정 전과 비교해 0.5ms 이상 늘지 않아야 합니다.** 늘었다면 지오메트리나 브러시를 매 프레임 다시 만들고 있는 것입니다. 캐시를 확인하십시오.

측정값을 커밋 메시지나 이 문서 끝에 적어 두십시오.

### 10-3. DPI

DPI가 다른 모니터로 옮기거나 배율을 바꾸었을 때 로고와 아이콘이 깨지지 않는지 봅니다. 특히 4-4의 지오메트리 캐시가 크기 변화를 감지하는지 확인하십시오.

---

## 11. 커밋

내용이 많으므로 나누십시오.

```
feat: 상단바에 직접 그리는 아이콘 종류를 더한다
feat: 시작 단추에 셸의 그라데이션 로고를 그린다
feat: 배터리와 CPU 아이콘을 값에 따라 색이 바뀌게 그린다
feat: 볼륨과 네트워크에 Segoe Fluent Icons를 쓴다
fix: 네트워크 표시에 단위를 붙이고 패널 문구를 고친다
```
