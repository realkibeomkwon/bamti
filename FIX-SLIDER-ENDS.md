# 수정 지시서: 슬라이더가 0%와 100%에 닿게 한다

`TASK-VOLUME-WIDGET.md`와 `FIX-WIDGET-BOARD-GATE.md`가 끝난 상태에서 이어서 하십시오.

---

## 1. 결함

상태 패널의 슬라이더 행(`RowType::kSlider`)이 양 끝에서 0%와 100%에 닿지 않습니다. 손잡이를 트랙 왼쪽 끝까지 끌어도 값이 0%가 되지 않고, 오른쪽 끝까지 끌어도 100%가 되지 않는다고 관측되었습니다.

원인은 두 갈래입니다. 하나는 코드만 읽어도 확정되는 렌더 결함이고, 다른 하나는 측정해야 판정되는 값 왕복 문제입니다. **둘 다 처리하십시오.**

### 1-1. 확정된 결함: 채운 막대와 손잡이가 서로 다른 식을 쓴다

`src/status_panel.cpp`의 `StatusPanelContent::Render`입니다.

```cpp
const float value = ClampUnit(row.value);
const float filled = left + (right - left) * value;          // (A) 트랙 전폭에 값을 곱한다
...
const float thumb_d = static_cast<float>(DipToPx(kPanelSliderThumbDip, dpi));
const float thumb_r = thumb_d * 0.5f;
const float cx = left + (right - left - thumb_d) * value + thumb_r;   // (B) 손잡이 지름만큼 들여서 곱한다
```

(A)와 (B)는 `value == 0.5`일 때만 일치합니다. 양 끝에서 최대 `thumb_r`(14dip 손잡이 기준 7dip)만큼 어긋납니다.

| value | 채운 막대 끝 (A) | 손잡이 중심 (B) | 차이 |
|---|---|---|---|
| 0.0 | `left` | `left + 7` | 7dip |
| 0.5 | 가운데 | 가운데 | 0 |
| 1.0 | `right` | `right - 7` | 7dip |

값이 100%여도 손잡이가 트랙 오른쪽 끝에서 7dip 안쪽에 멈춰 있고, 그 바깥으로 채운 막대만 삐져나옵니다. **사용자가 "끝에 닿지 않는다"고 보는 화면이 바로 이것입니다.**

### 1-2. 측정해야 하는 결함: 볼륨 값이 왕복하면서 어긋난다

`StatusPanelContent::DragTo`는 클릭 위치를 값으로 바꿀 때 트랙 양 끝을 손잡이 반지름만큼 들여서 계산합니다.

```cpp
const float lo = static_cast<float>(rc.left) + thumb_d * 0.5f;
const float hi = static_cast<float>(rc.right) - thumb_d * 0.5f;
float v = hi > lo ? (static_cast<float>(client.x) - lo) / (hi - lo) : 0.0f;
v = ClampUnit(v);
v = std::round(v / 0.02f) * 0.02f;
```

히트 사각형(`hits_`의 `rc`)은 `Measure`에서 `RECT{pad, y, width - pad, ...}`로 잡히고, `Render`의 트랙도 `left = pad`, `right = width - pad`입니다. 즉 히트 영역과 트랙의 가로 범위가 정확히 같습니다. 그러므로 트랙 왼쪽 끝을 클릭하면 `v`는 음수가 되었다가 `ClampUnit`으로 **0.0이 되고**, 오른쪽 끝은 **1.0이 됩니다.** 계산만 보면 끝값에 도달합니다.

그런데도 화면에 2%와 98%가 남는다면, 값을 되돌려주는 쪽이 범인입니다. 볼륨 위젯은 슬라이더 값을 직접 유지하지 않고 **1초마다 Core Audio에서 다시 읽어 덮어씁니다.** `src/widgets/builtin.cpp`의 `BuiltinWidgets::SampleVolume`입니다.

```cpp
const VolumeState state = volume_->Read();          // GetMasterVolumeLevelScalar
...
panel.rows.push_back(SliderRow("volume_level", L"볼륨", state.level, PercentText(pct, false)));
```

`IAudioEndpointVolume::SetMasterVolumeLevelScalar`는 인자를 그대로 저장하지 않습니다. 내부 표현이 dB이고 장치마다 단계가 정해져 있어서, 설정한 스칼라와 곧바로 읽은 스칼라가 다를 수 있습니다. 특히 0.0과 1.0 부근에서 그렇습니다. **이것이 사실인지 아닌지를 먼저 측정으로 확정하십시오.** 추측으로 코드를 고치지 마십시오.

---

## 2. 수정 A: 렌더의 두 식을 하나로 통일한다

`src/status_panel.cpp`의 `Render` 안 슬라이더 분기를 고칩니다. 기준은 **손잡이 중심**입니다. 손잡이가 트랙 밖으로 나가면 안 되므로 이동 구간은 `[left + thumb_r, right - thumb_r]`로 두고, 채운 막대는 트랙 왼쪽 끝에서 손잡이 중심까지 그립니다.

```cpp
const float value = ClampUnit(row.value);
const float thumb_d = static_cast<float>(DipToPx(kPanelSliderThumbDip, dpi));
const float thumb_r = thumb_d * 0.5f;
const float lo = left + thumb_r;
const float hi = right - thumb_r;
const float cx = hi > lo ? lo + (hi - lo) * value : lo;
const float cy = track_top + radius;

const D2D1_ROUNDED_RECT track_rc{D2D1::RectF(left, track_top, right, track_bottom), radius, radius};
target->FillRoundedRectangle(track_rc, track.Get());
if (cx > left) {
  const D2D1_ROUNDED_RECT fill_rc{D2D1::RectF(left, track_top, cx, track_bottom), radius, radius};
  target->FillRoundedRectangle(fill_rc, fill.Get());
}
float draw_r = thumb_r;
if (hot_index == hit_i - 1 || drag_row_ == static_cast<int>(i)) {
  draw_r += 2.0f;
}
target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), draw_r, draw_r), thumb.Get());
```

`DragTo`의 `lo`/`hi`와 같은 식이 되었습니다. 두 곳이 같은 뜻의 값을 각자 계산하고 있으므로, `status_panel.cpp`의 익명 이름공간에 작은 헬퍼를 두고 양쪽에서 부르십시오.

```cpp
struct SliderGeometry {
  float lo = 0.0f;
  float hi = 0.0f;
  float thumb_r = 0.0f;
};

SliderGeometry SliderGeom(float left, float right, UINT dpi) {
  const float d = static_cast<float>(DipToPx(kPanelSliderThumbDip, dpi));
  const float r = d * 0.5f;
  return SliderGeometry{left + r, right - r, r};
}
```

`Render`는 `left`/`right`를, `DragTo`는 `rc.left`/`rc.right`를 넘깁니다. 두 값은 같아야 하며, 실제로 같습니다.

### 2-1. 100%일 때 오른쪽 끝이 비어 보이는 문제

`cx`가 `right - thumb_r`에서 멈추므로, 값이 100%여도 트랙 오른쪽 끝 7dip이 채워지지 않은 채 남습니다. 손잡이가 그 자리를 덮으므로 대개 눈에 띄지 않지만, 손잡이 색과 채움 색이 다르면 보입니다.

**손잡이가 채움 색과 같은 계열이면 그대로 두십시오.** 다르면 채움을 `cx + thumb_r`까지, 즉 손잡이 오른쪽 끝까지 그리되 `right`를 넘지 않게 자르십시오. 어느 쪽을 골랐는지 커밋 메시지에 한 줄로 적으십시오.

---

## 3. 측정: 볼륨 값이 왕복하면서 얼마나 어긋나는가

수정 A만으로 끝값 문제가 사라지는지 확인해야 합니다. 사라지지 않으면 1-2절의 왕복 오차가 원인입니다.

### 3-1. 로그를 붙인다

`src/widgets/volume.cpp`의 `VolumeControl::SetLevel`에서, 설정한 값과 **곧바로 다시 읽은 값**을 함께 남기십시오. 매번 남기면 로그가 넘치므로 값이 0.02 이하이거나 0.98 이상일 때만 남깁니다.

```cpp
bool VolumeControl::SetLevel(float level) {
  if (!Ensure() || !volume_) {
    return false;
  }
  const float want = ClampUnit(level);
  const HRESULT hr = volume_->SetMasterVolumeLevelScalar(want, nullptr);
  if (FAILED(hr)) {
    Release();
    return false;
  }
  if (want <= 0.02f || want >= 0.98f) {
    float got = 0.0f;
    if (SUCCEEDED(volume_->GetMasterVolumeLevelScalar(&got))) {
      Log(L"widget", L"volume set %.4f -> read %.4f", want, got);
    }
  }
  return true;
}
```

`volume.cpp`는 이미 `log.hpp`를 포함하고 있습니다.

### 3-2. 무엇을 보는가

앱을 다시 빌드해 띄우고, 볼륨 슬라이더를 왼쪽 끝까지 끌었다가 오른쪽 끝까지 끄십시오. 로그에서 다음을 확인합니다.

| 관측 | 뜻 | 대응 |
|---|---|---|
| `set 0.0000 -> read 0.0000`, `set 1.0000 -> read 1.0000` | 왕복 오차 없음 | 3-3을 건너뛴다 |
| `set 0.0000 -> read 0.0200` 처럼 어긋남 | 장치가 스칼라를 자기 단계로 반올림한다 | 3-3을 적용한다 |
| 로그가 아예 없음 | 끝값이 슬라이더에서 나오지 않는다 | 2절 수정이 덜 됐거나 `DragTo`의 반올림이 원인이다. `DragTo`가 만든 `v`를 임시로 로그에 남겨 확인한다 |

### 3-3. 왕복 오차가 있을 때만 적용한다

장치가 되돌려준 값을 그대로 UI에 쓰면 사용자가 끝까지 끌어도 끝에 닿지 않습니다. 이때는 **사용자가 지정한 값을 짧게 우선합니다.**

`BuiltinWidgets`에 마지막으로 설정한 값과 그 시각을 기억해 두고, `SampleVolume`이 읽어 온 값이 그 값과 0.03 이내로 다르면서 설정 후 2초가 지나지 않았다면, 읽은 값 대신 설정한 값을 게시하십시오. 2초가 지나면 무조건 읽은 값을 따릅니다. 그래야 볼륨 키나 다른 앱이 볼륨을 바꿨을 때 추종이 끊기지 않습니다.

**이 처리는 3-2에서 어긋남이 실제로 관측되었을 때만 넣으십시오.** 관측되지 않으면 넣지 마십시오. 있지도 않은 문제를 위한 상태 변수가 남습니다.

### 3-4. 측정용 로그의 처리

3-1에서 넣은 로그는 **남겨 두십시오.** 조건이 양 끝 근처로 좁고, 슬라이더를 끌 때만 나오므로 상주 비용이 없습니다.

---

## 4. 하지 말아야 할 것

- `DragTo`의 `std::round(v / 0.02f) * 0.02f` 스텝을 없애지 마십시오. 2% 단위 스텝은 0과 1을 정확히 표현할 수 있으므로 끝값 문제와 무관합니다.
- 히트 사각형(`Measure`의 `hit.rc`)의 가로 범위를 바꾸지 마십시오. 트랙과 같아야 클릭 위치와 화면이 맞습니다.
- 게이지 행(`RowType::kGauge`)의 렌더를 건드리지 마십시오. 게이지는 손잡이가 없어서 전폭 식이 맞습니다.
- `kPanelSliderThumbDip` 값을 바꾸지 마십시오. 이번 수정은 식의 불일치를 없애는 것이지 크기 조정이 아닙니다.
- 레지스트리에 쓰지 마십시오. 검증 절차에도 넣지 마십시오.

---

## 5. 검증

1. Release 빌드가 경고 없이 통과합니다.
2. 볼륨 위젯을 켜고 상단바의 볼륨 항목을 눌러 패널을 엽니다.
3. 손잡이를 트랙 **왼쪽 끝까지** 끕니다. 값 표시가 `0%`가 되고, 손잡이 중심이 트랙 왼쪽 끝에서 손잡이 반지름만큼 안쪽에 놓이며, 그 왼쪽으로 채운 막대가 삐져나오지 않습니다.
4. 손잡이를 **오른쪽 끝까지** 끕니다. 값 표시가 `100%`가 되고, 손잡이가 트랙 밖으로 나가지 않습니다.
5. 트랙의 왼쪽 끝을 **클릭**하면 0%, 오른쪽 끝을 **클릭**하면 100%가 됩니다.
6. 값을 50%로 두었을 때 채운 막대의 끝과 손잡이의 중심이 같은 자리에 있습니다.
7. 슬라이더를 끄는 동안 Windows 소리 설정의 볼륨 값이 함께 움직입니다.

---

## 6. 커밋

```
fix: 슬라이더가 양 끝에서 0%와 100%에 닿게 한다
```

3-3을 적용했다면 그 이유가 된 측정값을 커밋 본문에 한 줄로 적으십시오.
