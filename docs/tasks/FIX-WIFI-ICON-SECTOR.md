# 작업 지시서: Wi-Fi 아이콘을 부채꼴 한 개와 호 두 개로 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/clock_renderer.cpp` 와 `src/bar_layout.hpp` 와 `src/widgets/builtin.cpp` 셋입니다. 앞선 지시서 `TASK-WIFI-ICON-SIGNAL.md` 로 만든 `DrawWifiIcon` 의 도형과 단계 수를 고치는 작업이며, 그때 만든 배선과 플래그와 캐시 처리는 그대로 씁니다.

## 무엇이 틀렸는가

앞선 지시서가 아이콘을 **점 한 개와 호 세 개**로 지정했습니다. 사용자가 요청한 구성은 **부채꼴 한 개와 호 두 개**입니다. 맨 아래 요소는 원이 아니고, 그 위의 호는 셋이 아니라 둘입니다.

요소가 넷에서 셋으로 줄어들므로 신호 강도의 단계도 넷에서 **셋**으로 줄어듭니다.

| 단계 | 진해지는 요소 |
| --- | --- |
| 2 | 부채꼴 + 안쪽 호 + 바깥 호 |
| 1 | 부채꼴 + 안쪽 호 |
| 0 | 부채꼴만 |
| -1 (연결 안 됨) | 없음 |

## 치수는 어디서 나왔는가

아래의 좌표는 짐작한 값이 아니라 사용자가 제시한 참조 이미지를 픽셀 단위로 측정해서 뽑은 값입니다. 원본에서 아이콘의 잉크는 가로 178~235, 세로 74~117 을 차지했고, 여기에서 다음을 얻었습니다.

| 측정 항목 | 원본 픽셀 |
| --- | --- |
| 두 호와 부채꼴의 공통 중심 | (206.5, 112.5) |
| 획 굵기 | 8 |
| 안쪽 호 반지름 | 19 |
| 바깥 호 반지름 | 35 |
| 호의 반각 | 44.4° |
| 부채꼴이 중심 위로 뻗은 길이 | 7.5 |
| 부채꼴이 중심 아래로 뻗은 길이 | 4.5 |
| 부채꼴의 최대 폭 | 16 |

세로 44 픽셀을 12 단위로 옮겼으므로 배율은 픽셀당 0.2727 단위입니다. 아래의 모든 값은 이 배율로 환산한 결과입니다. **그러므로 값을 임의로 반올림하거나 조정하지 마십시오.** 조정이 필요하면 그대로 두고 보고해 주십시오.

---

## 1. `src/clock_renderer.cpp` — `DrawWifiIcon` 의 도형을 다시 짠다

함수의 선언과 호출 방식은 그대로 두고 본문만 갈아 냅니다. 좌표계가 18 × 13 에서 **16 × 12** 로 바뀝니다.

```cpp
const float s = (box.bottom - box.top) / 12.0f;
```

`s <= 0.0f` 이거나 인자 중 하나라도 널이면 그대로 돌아가는 앞부분은 그대로 둡니다.

**중심과 공통값.**

```
cx = box.left + 8.00 * s
cy = box.top  + 10.70 * s
sin = 0.69966   (44.4°)
cos = 0.71448
획 굵기 = 2.20 * s
```

`tint` 람다는 그대로 두되, 이제 인덱스가 0, 1, 2 세 개뿐입니다. 부채꼴이 0번, 안쪽 호가 1번, 바깥 호가 2번입니다.

### 1-1. 부채꼴

부채꼴은 세 꼭짓점을 이은 역삼각형을 **채운 뒤 같은 도형을 둥근 획으로 한 번 더 긋는** 방식으로 그립니다. 획의 절반인 1.10 만큼 도형이 사방으로 커지면서 세 모서리가 둥글어지고, 그 결과가 참조 이미지의 모양과 맞습니다.

```
아래 꼭짓점 = (cx,          cy + 0.15 * s)
왼쪽 꼭짓점 = (cx - 1.10 * s, cy - 0.95 * s)
오른쪽 꼭짓점 = (cx + 1.10 * s, cy - 0.95 * s)
```

```cpp
sink->BeginFigure(bottom, D2D1_FIGURE_BEGIN_FILLED);
sink->AddLine(left);
sink->AddLine(right);
sink->EndFigure(D2D1_FIGURE_END_CLOSED);
```

닫은 뒤에 두 번 그립니다. 채우기만 하면 모서리가 뾰족하게 남아 옆의 둥근 호와 어울리지 않습니다.

```cpp
rt->FillGeometry(path.Get(), brush);
rt->DrawGeometry(path.Get(), brush, width, stroke);
```

`round_stroke_` 는 `lineJoin` 이 `D2D1_LINE_JOIN_ROUND` 이고 양 끝이 둥근 마감이므로(`src/clock_renderer.cpp:512` 근처), 이렇게 하면 도형이 1.10 만큼 커지면서 모서리가 둥글어집니다. 그래서 부채꼴의 실효 치수는 이렇게 됩니다.

| 실효 치수 | 계산 | 참조 이미지 |
| --- | --- | --- |
| 중심 위로 | 0.95 + 1.10 = 2.05 | 2.05 |
| 중심 아래로 | 0.15 + 1.10 = 1.25 | 1.23 |
| 최대 반폭 | 1.10 + 1.10 = 2.20 | 2.18 |

**꼭짓점 세 개는 작지만 획이 굵어서 결과가 커집니다.** 삼각형 자체가 작아 보인다고 크기를 키우지 마십시오.

### 1-2. 호 두 개

반지름은 `5.20f * s` 와 `9.55f * s` 입니다. 그리는 방법은 지금 코드의 `draw_arc` 람다 그대로이고, 반지름 세 개를 호출하던 자리를 두 개로 줄입니다.

```cpp
draw_arc(5.20f * s, 1);
draw_arc(9.55f * s, 2);
```

### 1-3. 좌표 확인표

계산 결과를 확인하실 수 있도록 단위 좌표를 적어 둡니다. 값이 이와 다르게 나오면 중심이나 부호를 잘못 쓴 것입니다.

| 요소 | 반지름 | 왼쪽 끝 | 오른쪽 끝 | 꼭대기(획 중심) |
| --- | --- | --- | --- | --- |
| 안쪽 호 | 5.20 | (4.36, 6.98) | (11.64, 6.98) | y = 5.50 |
| 바깥 호 | 9.55 | (1.32, 3.88) | (14.68, 3.88) | y = 1.15 |

요소 사이의 빈틈은 부채꼴과 안쪽 호 사이가 2.05 이고, 두 호 사이가 2.15 입니다. 참조 이미지에서도 이 둘은 정확히 같지는 않으므로 억지로 맞추지 마십시오.

도형이 상자 안에 들어가는지도 확인해 두었습니다. 바깥 호의 위쪽 가장자리가 y = 0.05 이고, 부채꼴의 아래쪽 끝이 y = 11.95 이며, 가로로는 0.22 부터 15.78 까지입니다.

### 1-4. 단계를 되돌리는 계산

`DrawVectorIcon` 의 `kWifi` 분기(612행 근처)에서 곱하는 수를 3 에서 **2** 로 바꿉니다.

```cpp
        const int level = (icon.flags & kVectorFlagOffline) != 0
                              ? -1
                              : static_cast<int>(std::lround(value * 2.0f));
```

이 줄 말고 분기의 다른 부분은 그대로 둡니다.

## 2. `src/bar_layout.hpp` — 아이콘의 폭을 새 좌표계에 맞춘다

높이는 12dip 그대로이고 좌표계의 가로가 16 이므로, 폭도 16dip 입니다.

```cpp
inline constexpr float kWifiIconHeightDip = 12.0f;
inline constexpr float kWifiIconDip = 16.0f;
```

`kWifiIconDip` 을 `kWifiIconHeightDip * (18.0f / 13.0f)` 로 계산하던 식을 지우십시오. 새 좌표계에서는 높이 12 단위가 곧 12dip 이라 단위와 dip 가 1대1로 맞으므로, 비율로 적을 이유가 없습니다.

`src/bar_layout.cpp` 의 `StatusIconWidth` 와 `src/clock_renderer.cpp` 의 상자 크기 분기는 이 상수를 이름으로 읽고 있으므로 고칠 것이 없습니다.

## 3. `src/widgets/builtin.cpp` — 단계를 셋으로 줄인다

### 3-1. 경계와 계산 함수

`kWifiLevelEdges` 를 두 개로 줄이고 반복 범위도 함께 줄입니다. 0 부터 100 까지를 셋으로 나누므로 경계는 34 와 67 입니다.

```cpp
constexpr int kWifiLevelEdges[2] = {34, 67};
constexpr int kWifiLevelHysteresis = 3;

int WifiLevel(int quality, int prev) {
  int level = 0;
  for (int i = 0; i < 2; ++i) {
    if (quality >= kWifiLevelEdges[i]) {
      level = i + 1;
    }
  }
  if (prev < 0) {
    return level;
  }
  if (level == prev + 1 && quality < kWifiLevelEdges[prev] + kWifiLevelHysteresis) {
    return prev;
  }
  if (level == prev - 1 && quality >= kWifiLevelEdges[level] - kWifiLevelHysteresis) {
    return prev;
  }
  return level;
}
```

**반복 범위를 함께 줄이는 것이 중요합니다.** `i < 3` 을 그대로 두면 원소가 두 개인 배열의 세 번째 칸을 읽습니다. 히스테리시스의 두 조건은 그대로 두어도 안전합니다. `level == prev + 1` 이면 `prev` 는 0 이나 1 이고, `level == prev - 1` 이면 `level` 이 0 이나 1 이라서 두 첨자 모두 배열 안에 머뭅니다.

### 3-2. 아이콘에 싣는 값

`SampleNetwork` 의 Wi-Fi 분기에서 나누는 수를 3 에서 **2** 로 바꿉니다.

```cpp
    const float level = static_cast<float>(wifi_level < 0 ? 0 : wifi_level);
    SetVectorIcon(&item, VectorIcon::kWifi, level / 2.0f, 0);
```

값이 0, 0.5, 1 세 가지만 나오므로 `std::lround(value * 2.0f)` 가 단계를 정확히 되돌립니다.

`kVectorFlagOffline` 을 쓰는 `else` 분기와 히스테리시스를 갱신하는 잠금 구간은 그대로 둡니다.

---

## 검증

### 빌드

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

### 판정 기준

1. 아이콘의 맨 아래가 원이 아니라 **아래로 뾰족한 부채꼴**입니다. 위쪽 두 모서리와 아래 꼭짓점이 모두 둥급니다.
2. 부채꼴 위의 호가 **두 개**입니다. 세 개로 보이면 `draw_arc` 호출을 하나 지우지 않은 것입니다.
3. 부채꼴의 폭이 그 높이와 비슷합니다. 실효 치수로 폭이 4.40 이고 높이가 3.30 이므로 세로보다 가로가 조금 더 깁니다.
4. 아이콘이 상단바 위아래로 잘리지 않고, 옆의 블루투스와 볼륨 아이콘에 닿지 않습니다.
5. 로그의 `wifi quality=... level=...` 에서 level 이 0 과 2 사이입니다. 3 이 찍히면 곱하거나 나누는 수를 덜 고친 것입니다.

### 신호 강도

Wi-Fi 를 끄는 방식으로 검증하지 마십시오. 네트워크가 끊기면 작업하는 에이전트 자신의 API 연결도 함께 끊깁니다. `netsh wlan show interfaces` 로 지금 강도를 읽고, 67 이상이면 세 요소가 모두 진한 것이 맞습니다.

### bamti 재시작

**bamti 는 새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. 상단바 창(클래스 `bamti.MenuBar`)에 `WM_COMMAND` 로 명령 1번을 보내면 정리 절차를 거쳐 끝납니다.

---

## 이번 작업에서 하지 않는 것

- `WlanStatus.quality` 와 `kVectorFlagOffline` 과 아이콘 캐시 배선은 앞선 작업에서 이미 자리를 잡았으므로 건드리지 않습니다.
- 제어 센터의 Wi-Fi 행이 쓰는 글리프는 그대로 둡니다.
- 도형의 크기가 어색해 보여도 값을 스스로 조정하지 마십시오. 위의 좌표는 참조 이미지를 측정해서 서로 맞물리게 잡은 값이라, 하나만 움직이면 균형이 깨집니다.
