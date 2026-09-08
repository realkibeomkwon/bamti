# 작업 지시서: 상단바 Wi-Fi 아이콘을 3단 벡터로 다시 그리고 신호 강도를 불투명도로 나타낸다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

## 현재 상태

상단바의 네트워크 아이콘은 벡터 도형이 아니라 Segoe Fluent Icons 의 글리프 하나를 글자로 찍고 있습니다.

- `src/clock_renderer.cpp:33` `constexpr wchar_t kWifiFluent[] = L"\xE701";`
- `src/clock_renderer.cpp:570` `case VectorIcon::kWifi:` 에서 `DrawFluentOrFallback` 를 호출합니다.

이 글리프는 Windows 방식이라서 호가 네 개(4단)로 보이고, 굵기와 각도를 손댈 수 없습니다. 그리고 아이콘이 나타내는 정보는 연결 여부뿐입니다. `src/widgets/builtin.cpp:1306` 이 `SetVectorIcon(&item, VectorIcon::kWifi, 1.0f, 0)` 으로 값을 항상 1.0 으로 넘기고, `src/clock_renderer.cpp:571` 이 그 값을 `value < 0.5f` 한 번만 비교해서 전체 불투명도를 45% 와 100% 중 하나로 고릅니다. 신호 강도는 아예 수집하지 않습니다.

`QueryWlanStatus`(`src/control_center.cpp:104`)는 이미 `WLAN_CONNECTION_ATTRIBUTES` 를 얻어 놓고도 그 안의 `wlanAssociationAttributes.wlanSignalQuality`(0~100)를 버리고 있습니다. 강도를 새로 조회할 필요는 없고, 이미 받아 온 값을 구조체에 실어 주기만 하면 됩니다.

## 목표

macOS 방식과 같은 **점 한 개와 호 세 개**로 이루어진 벡터 아이콘을 직접 그립니다. 그리고 신호 강도를 **요소별 불투명도**로 나타냅니다.

- 가장 약할 때는 맨 아래 점만 진하게 칠하고 호 세 개는 모두 흐리게 둡니다.
- 강도가 올라갈수록 안쪽 호부터 차례로 진해집니다.
- 가장 강할 때는 맨 위 바깥 호까지 진해집니다.
- 연결되지 않았을 때는 점까지 포함해 네 요소를 모두 흐리게 둡니다.

`DrawEthernetIcon` 과 `DrawBluetoothIcon` 이 이미 같은 방식으로 벡터 도형을 그리고 있으므로, 그 두 함수의 구조와 이름 규칙을 그대로 따르십시오.

---

## 1. `src/control_center.hpp` — 신호 강도를 담을 자리를 만든다

`struct WlanStatus`(20행)에 필드 하나를 더합니다.

```cpp
struct WlanStatus {
  bool radio = false;
  bool connected = false;
  std::wstring name = L"연결 안 됨";
  int quality = 0;  // 0~100, 연결되지 않았으면 0
  double ms = 0.0;
};
```

## 2. `src/control_center.cpp` — 이미 받아 온 강도를 담는다

`QueryWlanStatus` 안에서 `info.connected = true;` 를 적는 자리(149행 근처)의 바로 앞에, 같은 `attrs` 에서 강도를 꺼내 담습니다. `WlanFreeMemory(attrs)` 보다 앞이어야 합니다.

```cpp
info.quality = static_cast<int>(attrs->wlanAssociationAttributes.wlanSignalQuality);
if (info.quality < 0) {
  info.quality = 0;
} else if (info.quality > 100) {
  info.quality = 100;
}
info.connected = true;
```

이 함수의 다른 부분은 건드리지 마십시오. 제어 센터가 쓰는 `kWifiGlyph`(`src/control_center.cpp:52`)도 이번 작업 범위가 아닙니다.

## 3. `src/status_item.hpp` — 연결 안 됨을 나타낼 플래그를 더한다

값(`icon.value`)이 이제 신호 강도를 나르므로, 연결 여부는 값으로 판단할 수 없습니다. 별도의 플래그 비트로 나눕니다. `kVectorFlagMuted`(41행) 아래에 한 줄을 더합니다.

```cpp
inline constexpr uint32_t kVectorFlagOffline = 1u << 2;
```

`HashStatusIcon` 은 `IconKind::kVector` 일 때 `value` 와 `flags` 를 모두 해시에 넣고 있으므로(`src/status_item.hpp:142`), 캐시 키 계산은 고칠 필요가 없습니다.

## 4. `src/bar_layout.hpp` — 아이콘의 폭과 높이를 정한다

새 아이콘은 정사각형이 아니라 가로로 넓습니다. `kBluetoothIconDip`(17행) 아래에 두 줄을 더합니다. 도형을 18 × 13 좌표계로 설계했으므로 폭은 그 비율로 계산합니다.

```cpp
inline constexpr float kWifiIconHeightDip = 12.0f;
inline constexpr float kWifiIconDip = kWifiIconHeightDip * (18.0f / 13.0f);
```

## 5. `src/bar_layout.cpp` — 배치가 그 폭을 쓰게 한다

`StatusIconWidth`(35행)에 분기를 하나 더합니다. 이더넷과 블루투스 분기 뒤에 같은 모양으로 붙이십시오.

```cpp
if (item.icon.kind == IconKind::kVector && item.icon.vector == VectorIcon::kWifi) {
  return kWifiIconDip;
}
```

여기를 빠뜨리면 배치는 16dip 을 잡아 두고 그림은 그보다 넓게 그려지므로, 아이콘이 옆 항목과 겹칩니다.

## 6. `src/clock_renderer.cpp` — 아이콘을 직접 그린다

### 6-1. 이름 없는 이름공간에 그리기 함수를 더한다

`DrawBluetoothIcon`(91행) 정의 바로 뒤에 다음 함수를 더합니다. 헤더에는 선언하지 마십시오. 두 이웃 함수와 마찬가지로 이 파일 안에서만 쓰는 함수입니다.

```cpp
void DrawWifiIcon(ID2D1RenderTarget* rt, ID2D1Factory* factory, ID2D1SolidColorBrush* brush,
                  ID2D1StrokeStyle* stroke, const D2D1_RECT_F& box, D2D1_COLOR_F color, int level);
```

**좌표계.** 높이를 13 등분한 값을 단위로 씁니다. `const float s = (box.bottom - box.top) / 13.0f;` 이고, `s <= 0.0f` 이면 아무것도 그리지 않고 돌아갑니다. `rt` 와 `factory` 와 `brush` 와 `stroke` 중 하나라도 널이면 역시 그대로 돌아갑니다. 원점은 `box.left` 와 `box.top` 입니다.

**중심.** 네 요소는 모두 한 점을 중심으로 놓입니다.

```
cx = box.left + 9.00 * s
cy = box.top  + 11.60 * s
```

**점.** 중심에 반지름 `1.30 * s` 인 원을 `FillEllipse` 로 채웁니다.

**호 세 개.** 반지름은 안쪽부터 `3.70 * s`, `6.50 * s`, `9.30 * s` 입니다. 각 호는 수직선을 기준으로 좌우 50°씩, 합해서 100°를 덮습니다. 반지름 `r` 마다 양 끝점은 이렇게 나옵니다.

```
왼쪽   = (cx - r * 0.76604, cy - r * 0.64279)
오른쪽 = (cx + r * 0.76604, cy - r * 0.64279)
```

`0.76604` 와 `0.64279` 는 각각 sin 50° 와 cos 50° 입니다. 파일 위쪽의 상수로 올리지 말고 함수 안의 지역 상수로 두십시오. 계산 결과를 확인하실 수 있도록 단위 좌표를 적어 둡니다. 값이 이와 다르게 나오면 부호나 중심을 잘못 쓴 것입니다.

| 반지름 | 왼쪽 끝 | 오른쪽 끝 | 꼭대기 |
| --- | --- | --- | --- |
| 3.70 | (6.17, 9.22) | (11.83, 9.22) | y = 7.90 |
| 6.50 | (4.02, 7.42) | (13.98, 7.42) | y = 5.10 |
| 9.30 | (1.88, 5.62) | (16.12, 5.62) | y = 2.30 |

각 호는 `ID2D1PathGeometry` 를 하나 만들어 왼쪽 끝에서 `BeginFigure(..., D2D1_FIGURE_BEGIN_HOLLOW)` 로 시작하고, `AddArc` 로 오른쪽 끝까지 잇고, `EndFigure(D2D1_FIGURE_END_OPEN)` 으로 닫습니다. `AddArc` 의 인자는 다음과 같습니다.

```cpp
sink->AddArc(D2D1::ArcSegment(right, D2D1::SizeF(r, r), 0.0f,
                              D2D1_SWEEP_DIRECTION_CLOCKWISE, D2D1_ARC_SIZE_SMALL));
```

y 축이 아래로 향하는 좌표계에서 왼쪽 끝에서 꼭대기를 지나 오른쪽 끝으로 가는 방향이 시계 방향이고, 100°는 180°보다 작으므로 `D2D1_ARC_SIZE_SMALL` 이 맞습니다.

**선 굵기.** `1.75f * s` 를 씁니다. `DrawGeometry` 에 넘기는 `stroke` 는 호출한 쪽에서 받은 `round_stroke_` 이므로 끝이 둥글게 마감됩니다.

**요소별 불투명도.** 점을 0번, 안쪽 호를 1번, 가운데 호를 2번, 바깥 호를 3번으로 셉니다. `level` 보다 번호가 크지 않은 요소는 `color` 를 그대로 쓰고, 더 큰 요소는 알파를 0.30 배로 낮춥니다. 이 파일에는 이미 `ScaleAlpha`(47행)가 있으므로 그것을 쓰십시오.

```cpp
auto tint = [&](int index) {
  brush->SetColor(index <= level ? color : ScaleAlpha(color, 0.30f));
};
```

`level` 이 -1 이면 네 요소가 모두 흐려지므로, 연결되지 않은 상태는 따로 분기하지 않아도 저절로 처리됩니다. 그리는 순서는 점, 안쪽 호, 가운데 호, 바깥 호입니다. 요소마다 그리기 직전에 색을 다시 지정해야 합니다.

### 6-2. `DrawVectorIcon` 의 `kWifi` 분기를 갈아 낸다

570~573행의 분기를 다음으로 바꿉니다.

```cpp
    case VectorIcon::kWifi:
      if (EnsureStroke() && d2d_) {
        const int level = (icon.flags & kVectorFlagOffline) != 0
                              ? -1
                              : static_cast<int>(std::lround(value * 3.0f));
        DrawWifiIcon(rt_.Get(), d2d_.Get(), brush, round_stroke_.Get(), box, ClockTextColor(dark), level);
      }
      break;
```

이 파일은 이미 `std::isfinite` 를 쓰고 있어 `<cmath>` 가 들어와 있습니다. 새 헤더를 더하지 마십시오.

### 6-3. 아이콘 상자의 크기 분기를 더한다

749~757행의 분기에 Wi-Fi 를 더합니다.

```cpp
      } else if (seg.icon.vector == VectorIcon::kWifi) {
        icon_w = kWifiIconDip;
        icon_h = kWifiIconHeightDip;
      }
```

`bar_layout.cpp` 의 `StatusIconWidth` 와 여기가 같은 상수를 봐야 합니다. 한쪽만 고치면 안 됩니다.

### 6-4. 쓰지 않게 된 상수를 지운다

33행 `kWifiFluent` 와 35행 `kWifiFallback` 은 이 변경 뒤에 참조하는 곳이 없습니다. 두 줄을 지우십시오. `DrawFluentOrFallback` 자체는 볼륨과 제어 센터가 계속 쓰므로 그대로 둡니다.

## 7. `src/widgets/builtin.hpp` — 단계를 기억할 자리를 만든다

`last_wifi_name_`(124행) 아래에 한 줄을 더합니다. `mu_` 로 보호되는 다른 멤버들과 같은 자리입니다.

```cpp
  int last_wifi_level_ = -1;
```

## 8. `src/widgets/builtin.cpp` — 강도를 단계로 바꿔서 넘긴다

### 8-1. 단계 계산 함수

파일 위쪽의 이름 없는 이름공간, `SetVectorIcon`(122행) 근처에 상수와 함수를 더합니다.

```cpp
constexpr int kWifiLevelEdges[3] = {25, 50, 75};
constexpr int kWifiLevelHysteresis = 3;

int WifiLevel(int quality, int prev) {
  int level = 0;
  for (int i = 0; i < 3; ++i) {
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

**이 함수가 필요한 이유를 밝혀 둡니다.** `SampleNetwork` 는 2초마다 돌고(`network_due_ = GetTickCount64() + kBrightnessPeriodMs`, 1251행), `HashStatusIcon` 은 `icon.value` 를 해시에 넣습니다. 그러므로 강도를 100 으로 나눈 값을 그대로 실어 보내면, 82 와 79 처럼 눈에 보이지 않는 차이만으로도 캐시 키가 달라져 상단바가 2초마다 다시 그려집니다. 눈에 보이는 단계만 값에 실어야 그림이 실제로 달라질 때에만 다시 그립니다. 경계에서 값이 오르내리며 단계가 흔들리는 것도 3 포인트의 여유로 막습니다.

`prev < 0` 을 먼저 걸러내는 것이 중요합니다. 이 검사가 없으면 첫 표본에서 `kWifiLevelEdges[-1]` 을 읽습니다.

### 8-2. `SampleNetwork` 에서 값을 실어 보낸다

1286~1287행의 잠금 구간에서 단계를 갱신합니다. 갱신한 값은 잠금 밖에서 쓰기 위해 지역 변수로 받아 둡니다.

```cpp
  int wifi_level = -1;
  {
    std::lock_guard lock(mu_);
    last_wifi_on_ = wifi.radio || wifi.connected;
    last_wifi_name_ = wifi.connected ? wifi.name : std::wstring(L"연결 안 됨");
    last_wifi_level_ = wifi.connected ? WifiLevel(wifi.quality, last_wifi_level_) : -1;
    wifi_level = last_wifi_level_;
    last_eth_on_ = route.kind == NetKind::kEthernet;
    last_eth_name_ = route.alias;
    publish = settings_.network && sink_ != nullptr;
  }
```

연결이 끊기면 `last_wifi_level_` 을 -1 로 되돌립니다. 그래야 다시 연결될 때 예전 단계가 히스테리시스의 기준으로 남아 첫 단계를 잘못 붙잡지 않습니다.

그리고 1305~1312행의 두 분기를 고칩니다. 이더넷 분기는 그대로 둡니다.

```cpp
  } else if (route.kind == NetKind::kWifi) {
    const float level = static_cast<float>(wifi_level < 0 ? 0 : wifi_level);
    SetVectorIcon(&item, VectorIcon::kWifi, level / 3.0f, 0);
    item.state = StatusState::kOn;
    tip = L"Wi-Fi · ";
    tip += wifi.connected ? wifi.name : std::wstring(L"연결 안 됨");
  } else {
    SetVectorIcon(&item, VectorIcon::kWifi, 0.0f, kVectorFlagOffline);
    item.state = StatusState::kOff;
    tip = L"연결 안 됨";
  }
```

값이 0, 1/3, 2/3, 1 네 가지만 나오므로 `std::lround(value * 3.0f)` 는 원래 단계를 정확히 되돌립니다.

---

## 검증

### 빌드

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

### 판정 기준

1. 상단바의 Wi-Fi 아이콘이 호 세 개와 점 하나로 보입니다. 호가 네 개로 보이면 글리프가 그대로 남아 있는 것입니다.
2. 아이콘이 옆 항목과 겹치지 않고, 아이콘과 이웃 사이의 간격이 배터리나 블루투스와 비슷합니다. 겹치면 5번 항목을 빠뜨린 것입니다.
3. 강도에 따라 진한 요소의 개수가 달라집니다. 공유기에서 멀어지면 바깥 호부터 흐려지고, 가까이 가면 다시 진해집니다.
4. 연결을 끊으면 점까지 포함해 네 요소가 모두 흐려집니다.
5. 강도가 같은 단계에 머무는 동안에는 상단바를 다시 그리지 않습니다.

### 강도를 실제로 확인하는 방법

Wi-Fi 를 끄는 방식으로 검증하지 마십시오. 네트워크가 끊기면 작업하는 에이전트 자신의 API 연결도 함께 끊깁니다. 대신 지금 붙어 있는 신호의 강도를 읽어서, 그 값이 몇 단계에 해당하는지 계산한 뒤 화면과 견주십시오.

```powershell
netsh wlan show interfaces
```

여기서 나오는 `신호` 백분율이 `wlanSignalQuality` 와 같은 값입니다. 25 미만이면 점만, 25 이상 50 미만이면 안쪽 호까지, 50 이상 75 미만이면 가운데 호까지, 75 이상이면 바깥 호까지 진해야 합니다.

`Log(L"widget", ...)` 로 `wifi.quality` 와 단계를 한 번 찍어 두면 판정이 쉬워집니다. 다만 2초마다 도는 함수이므로 `static bool` 로 한 번만 찍는 이 파일의 기존 방식을 따르십시오.

### bamti 재시작

**bamti 는 새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. WMI 가 돌려주는 새 pid 는 재시작의 증거가 되지 못합니다. 상단바 창(클래스 `bamti.MenuBar`)에 `WM_COMMAND` 로 명령 1번을 보내면 정리 절차를 거쳐 끝납니다.

---

## 이번 작업에서 하지 않는 것

- 제어 센터의 Wi-Fi 행이 쓰는 글리프(`src/control_center.cpp:52` 의 `kWifiGlyph`)는 그대로 둡니다. 상단바 아이콘만 바꿉니다.
- 도구 설명(`item.tooltip`)에 강도 백분율을 더하지 않습니다. 아이콘 자체로 강도를 나타내는 것이 이번 요청입니다.
- `SampleNetwork` 가 `kBrightnessPeriodMs` 를 주기로 쓰고 있는 것은 이름이 어긋난 상태이지만, 이번에는 고치지 않습니다.
- 아이콘의 형태를 미세하게 조정할 필요가 생기면 `kWifiIconHeightDip` 과 `kWifiIconDip` 두 상수만 만지십시오. 도형의 단위 좌표는 바꾸지 마십시오.
