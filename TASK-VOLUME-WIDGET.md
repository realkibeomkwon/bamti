# 작업 지시서: 볼륨 위젯과 슬라이더 행

4단계에서 만든 내장 위젯(`src/widgets/builtin.cpp`)에 네 번째 위젯인 **볼륨**을 추가합니다. `PLAN-TRAY-TO-TOPBAR.md`와 이 지시서가 어긋나면 계획 문서가 우선입니다.

볼륨은 앞선 세 위젯과 성격이 다릅니다. 배터리와 CPU와 네트워크는 읽기만 하지만 볼륨은 **쓰기**를 합니다. 패널이 값을 보여 주는 데서 끝나지 않고 그 자리에서 값을 바꿔야 하므로, 프로토콜에 조절 가능한 행 하나(`slider`)를 새로 추가하고 팝업이 드래그를 처리하게 만드는 일이 이 작업의 절반을 차지합니다.

기존 위젯의 구조는 그대로 따릅니다. 새 스레드를 만들지 않고 `BuiltinWidgets`의 작업자 스레드 하나를 그대로 씁니다.

---

## 0. 완료 조건

- 상단바 빈 곳을 우클릭한 메뉴에 "볼륨" 항목이 있고, 켜면 상단바에 볼륨 위젯이 나타나며, 다시 실행해도 그 선택이 유지됩니다. **기본값은 꺼짐**입니다.
- 위젯을 클릭하면 볼륨 패널이 열리고, 슬라이더를 **누른 채 좌우로 끌면** 시스템 볼륨이 그에 따라 바뀝니다. 끄는 동안 패널이 닫히지 않습니다.
- 슬라이더의 아무 지점을 한 번 클릭하면 그 위치의 값으로 즉시 바뀝니다.
- 음소거 토글을 누르면 시스템 음소거가 바뀌고, 패널이 닫히지 않습니다.
- 다른 프로그램이나 키보드 볼륨 키로 볼륨을 바꾸면 위젯 표시가 따라 바뀝니다.
- 기본 출력 장치를 바꾸면 위젯이 새 장치의 볼륨을 보여 줍니다.
- 오디오 장치가 하나도 없는 상태에서 위젯이 사라지고, 장치가 돌아오면 다시 나타나며, 그 사이에 로그가 한 번만 남습니다.
- 네 위젯을 모두 켠 채로 10분 유휴 상태에서 bamti.exe의 CPU 사용률이 **0.2% 미만**입니다.
- 기존 파이프 클라이언트(v1, v2)의 동작이 그대로입니다.
- `/W4` 경고 없이 Debug와 Release가 모두 빌드됩니다.

---

## 1. 건드리는 파일

| 파일 | 할 일 |
|---|---|
| `src/widgets/volume.hpp` / `.cpp` | **새로 만듭니다.** Core Audio 접근을 감싸는 `VolumeControl`. |
| `src/widgets/builtin.hpp` / `.cpp` | 볼륨 표본 추출과 게시, 패널 이벤트 처리를 추가합니다. |
| `src/status_item.hpp` | `RowType::kSlider`를 추가합니다. |
| `src/status_source.hpp` | `StatusEvent`에 `value` 필드를 추가합니다. |
| `src/status_panel.cpp` | 슬라이더 행의 측정, 렌더, 드래그를 구현합니다. |
| `src/status_panel.hpp` | `StatusPanelContent`에 드래그 상태와 `arm_slider` 훅을 추가합니다. |
| `src/popup_surface.hpp` / `.cpp` | `PopupContent`에 드래그 훅을, `PopupSurface`에 드래그 상태를 추가합니다. |
| `src/settings.hpp` / `.cpp` | `volume` 키를 읽고 씁니다. |
| `src/menu_bar.cpp` | 메뉴 항목과 명령 처리, 드래그 중 패널 갱신 억제를 추가합니다. |
| `src/pipe_server.cpp` | `slider` 행 타입을 파싱합니다. |
| `CMakeLists.txt`, `bamti.vcxproj` | 새 소스 파일을 등록합니다. |
| `docs/STATUS-PROTOCOL.md` | `slider` 행과 `slide` 이벤트를 규격에 적습니다. |

---

## 2. 슬라이더 행 (`RowType::kSlider`)

### 2-1. 왜 게이지로는 안 되는가

`kGauge`는 이미 값을 막대로 그립니다. 그러나 게이지는 히트 영역을 만들지 않고(`Measure`가 `hits_`에 넣지 않습니다), 팝업은 마우스를 누른 채 움직이는 동작을 다루지 않습니다. 게이지에 히트만 붙이면 클릭 한 번으로 값을 정하는 것까지는 되지만 끄는 동작이 되지 않고, 무엇보다 읽기 전용 행과 조절 행을 같은 타입으로 두면 어느 게이지가 눌리는지 코드에서 구분할 수 없습니다. 그래서 별도 타입으로 나눕니다.

### 2-2. 자료 구조

`src/status_item.hpp`의 열거형에 추가합니다. **기존 항목의 순서를 바꾸지 마십시오.** `Fingerprint`가 `static_cast<int>(row.type)`을 문자열에 넣고 있어서 순서를 바꾸면 지문이 전부 어긋납니다.

```cpp
enum class RowType { kGauge, kKeyValue, kText, kSeparator, kToggle, kButton, kSlider };
```

`StatusRow`의 기존 필드를 그대로 씁니다. 새 필드를 만들지 마십시오.

- `value` — 0.0 ~ 1.0의 현재 값
- `value_text` — 오른쪽에 적을 값 표기(예: `72%`)
- `label` — 왼쪽 이름
- `row_id` — 이벤트에 실어 보낼 식별자
- `on` — 슬라이더에서는 쓰지 않습니다

`src/status_source.hpp`의 `StatusEvent`에 값을 하나 더합니다.

```cpp
struct StatusEvent {
  std::string id;
  std::string event;
  std::string row_id;
  std::string button;
  bool on = false;
  float value = 0.0f;   // 추가: event == "slide"일 때만 의미가 있다
};
```

### 2-3. 팝업의 드래그 훅

`src/popup_surface.hpp`의 `PopupContent`에 세 개를 더합니다. 기본 구현은 아무 일도 하지 않으므로 다른 콘텐츠(`OverflowContent`, 메뉴, 독)는 영향을 받지 않습니다.

```cpp
virtual bool DragRow(int /*index*/) const { return false; }
virtual void DragTo(int /*index*/, POINT /*client*/, UINT /*dpi*/) {}
virtual void DragEnd(int /*index*/) {}
```

`PopupSurface`에는 상태 하나와 조회 함수 하나를 더합니다.

```cpp
bool Dragging() const { return drag_index_ >= 0; }
// private
int drag_index_ = -1;
```

### 2-4. 드래그의 흐름

`src/popup_surface.cpp`를 다음과 같이 고칩니다.

**`WM_LBUTTONDOWN`** — 지금은 바깥 클릭만 처리하고 안쪽 클릭은 무시합니다. 안쪽이면서 눌린 행이 드래그 행이면 드래그를 시작합니다.

```cpp
if (in_self && content_ != nullptr) {
  const int index = content_->HitTest(pt, Dpi());
  if (index >= 0 && content_->DragRow(index)) {
    drag_index_ = index;
    content_->DragTo(index, pt, Dpi());
    InvalidateRect(hwnd_, nullptr, FALSE);
    UpdateWindow(hwnd_);
    return 0;
  }
}
```

`Open`이 `SetCapture(hwnd_)`를 걸어 두었으므로 커서가 팝업 밖으로 나가도 `WM_MOUSEMOVE`와 `WM_LBUTTONUP`이 계속 도착합니다. 그러니 드래그 중에 창 경계를 넘어도 값이 끊기지 않습니다.

**`WM_MOUSEMOVE`** — 드래그 중이면 호버 계산 대신 값을 갱신합니다. 히트 판정을 다시 하면 안 됩니다. 커서가 행 밖으로 벗어나도 붙잡고 있던 행을 계속 끌어야 하기 때문입니다.

```cpp
if (drag_index_ >= 0) {
  content_->DragTo(drag_index_, pt, Dpi());
  InvalidateRect(hwnd_, nullptr, FALSE);
  UpdateWindow(hwnd_);
  return 0;
}
```

**`WM_LBUTTONUP`** — 드래그 중이면 팝업을 닫지 않고 끝냅니다. `StickyRow` 경로가 하는 것과 같은 뒷정리를 합니다.

```cpp
if (drag_index_ >= 0) {
  const int index = drag_index_;
  drag_index_ = -1;
  press_inside_ = false;
  mouse_down_ = false;
  content_->DragEnd(index);
  InvalidateRect(hwnd_, nullptr, FALSE);
  if (after_tick_ != nullptr) {
    after_tick_(after_tick_ctx_);
  }
  return 0;
}
```

이 블록은 `if (!open_ || content_ == nullptr || !armed_)` 검사 **뒤에**, `in_allied` 처리 **앞에** 놓습니다.

**`Tick`** — 폴링이 드래그를 방해하지 않게 막습니다. 두 곳입니다.

- 바깥 클릭으로 닫는 판정(`if (saw_press && !mouse_down_)` 블록) 전체를 `drag_index_ < 0`일 때만 실행합니다.
- 호버 갱신(`if (content_ != nullptr)` 블록)도 `drag_index_ < 0`일 때만 실행합니다. 드래그 중에는 `hot_`을 건드리지 않습니다.

**`Dismiss`와 `Close`** — 어느 경로로 닫히든 `drag_index_ = -1`로 되돌립니다. `WM_CAPTURECHANGED`로 캡처를 잃는 경우가 여기에 걸립니다.

### 2-5. 측정과 렌더

`src/status_panel.cpp`에 상수를 더합니다.

```cpp
constexpr int kPanelSliderLabelDip = 18;
constexpr int kPanelSliderTrackDip = 6;
constexpr int kPanelSliderThumbDip = 14;
constexpr int kPanelSliderPadDip = 8;   // 트랙 위아래로 잡는 여유 히트 영역
constexpr int kPanelSliderGapDip = 10;
```

**`Measure`** — 슬라이더는 히트가 필요하므로 `hits_`에 넣습니다. 토글과 같은 방식입니다.

- 폭 계산: `kGauge`와 같게 `label + 12dip + value_text`를 씁니다.
- 높이: `kPanelSliderLabelDip + kPanelSliderPadDip * 2 + kPanelSliderTrackDip + kPanelSliderGapDip`.
- 히트 사각형: 이름 줄을 뺀 **트랙 영역에 위아래 `kPanelSliderPadDip`를 더한 띠**로 잡습니다. 트랙이 6dip밖에 안 되므로 그것만 히트로 잡으면 잡기가 너무 어렵습니다. 가로는 `pad`에서 `width - pad`까지입니다.

**`Render`** — 게이지와 같은 모양에 손잡이를 얹습니다.

- 이름은 왼쪽에, `value_text`는 오른쪽에 `muted` 색으로 그립니다.
- 트랙은 `track` 브러시, 채운 부분은 `fill` 브러시. 게이지와 달리 **둥근 모서리**로 그립니다(`FillRoundedRectangle`, 반지름은 트랙 높이의 절반).
- 손잡이는 채운 부분의 오른쪽 끝을 중심으로 `thumb` 브러시의 원을 그립니다. 반지름은 `kPanelSliderThumbDip / 2`.
- 손잡이 중심의 x는 `left + (right - left - thumb_d) * value + thumb_r`로 잡습니다. 그래야 값이 0일 때와 1일 때 손잡이가 트랙 밖으로 튀어나가지 않습니다.
- `hot_index`가 이 행이거나 드래그 중이면 손잡이를 조금 크게(반지름 +2px) 그립니다.

`hits_`를 소비하는 `hit_i` 증가 순서가 `Measure`가 넣은 순서와 정확히 일치해야 합니다. 어긋나면 `FIX-PANEL-HITS.md`가 다룬 결함이 재발합니다. 슬라이더 분기에서도 `NoteHitOutOfRange` 경계 검사를 버튼과 토글처럼 반드시 넣으십시오.

### 2-6. 값 계산과 이벤트

`StatusPanelContent`에 드래그 상태를 둡니다.

```cpp
// private
int drag_row_ = -1;
float drag_value_ = 0.0f;
```

`DragRow(index)`는 해당 행이 `kSlider`일 때 참입니다.

`DragTo(index, client, dpi)`는 다음을 합니다.

1. 행을 찾습니다. `kSlider`가 아니면 아무 일도 하지 않습니다.
2. 히트 사각형의 좌우와 손잡이 지름으로 값을 구합니다.
   ```cpp
   const float thumb_d = static_cast<float>(DipToPx(kPanelSliderThumbDip, dpi));
   const float lo = static_cast<float>(rc.left) + thumb_d * 0.5f;
   const float hi = static_cast<float>(rc.right) - thumb_d * 0.5f;
   float v = hi > lo ? (static_cast<float>(client.x) - lo) / (hi - lo) : 0.0f;
   v = ClampUnit(v);
   ```
   `ClampUnit`은 `builtin.cpp`에 있는 것과 같은 계산입니다. `status_panel.cpp`에 지역 함수로 하나 두십시오. 헤더로 옮기지 마십시오.
3. **값을 0.02(2%) 단위로 스냅**합니다. 픽셀마다 이벤트를 보내면 1초에 수십 번 COM 호출이 일어납니다. 스냅한 값이 직전에 보낸 값과 같으면 이벤트를 보내지 않고 반환합니다.
4. 행의 `value`와 `value_text`를 바로 고칩니다. 표시가 손가락을 따라와야 합니다. `value_text`는 `%d%%` 형식으로 다시 만듭니다.
5. `drag_row_`와 `drag_value_`를 갱신하고 `host_.dispatch`로 이벤트를 보냅니다.
   ```cpp
   StatusEvent ev;
   ev.id = item_.id;
   ev.event = "slide";
   ev.row_id = target.row_id;
   ev.value = v;
   host_.dispatch(ev);
   ```

`DragEnd(index)`는 `drag_row_ = -1`로 되돌리고, `host_.arm_slider`가 있으면 마지막 값으로 한 번 호출합니다.

### 2-7. 드래그 중에 패널이 덮어써지지 않게

`MenuBar::RefreshOpenPanel`은 항목의 `revision`이 오르면 `status_panel_->Reset()`으로 내용을 통째로 갈아 끼웁니다. 볼륨 위젯은 값을 바꾼 직후 새 항목을 게시하므로, 드래그하는 동안 이 경로가 돌면 손가락이 잡고 있는 값이 매번 되돌아갑니다.

`RefreshOpenPanel`의 맨 앞에 다음을 넣습니다.

```cpp
if (status_popup_.Dragging()) {
  return;
}
```

드래그를 놓은 뒤에도 위젯이 새 값을 게시하기까지 짧은 틈이 있습니다. 그 사이에 옛 값이 한 번 그려지면 손잡이가 튀어 보입니다. 토글이 쓰는 `arm_toggle`과 같은 구조로 막습니다.

- `StatusPanelHost`에 `std::function<void(std::string id, std::string row_id, uint64_t revision, float value)> arm_slider;`를 더합니다.
- `MenuBar`에 `pending_slider_`(id, row_id, revision, value)와 `slider_armed_`를 둡니다.
- `RefreshOpenPanel`에서 `slider_armed_ && item->revision == pending_slider_.revision`이면 갱신을 건너뜁니다. 토글이 하는 판정과 같습니다.
- 새 `revision`이 도착하면 `slider_armed_ = false`로 풀립니다.
- 되돌리기 타이머는 만들지 마십시오. 토글은 실패하면 상태가 거꾸로 남지만, 볼륨은 다음 표본이 진짜 값을 가져오므로 되돌릴 필요가 없습니다. `kToggleTimerId` 경로를 건드리지 마십시오.

### 2-8. 파이프 프로토콜

`src/pipe_server.cpp`의 `ParseRowsV2`에 한 줄을 더합니다.

```cpp
} else if (*type == "slider") {
  row.type = RowType::kSlider;
}
```

`value`와 `value_text`와 `row_id`는 이미 공통 경로에서 읽으므로 따로 손댈 것이 없습니다.

`slide` 이벤트를 클라이언트에게 내보내는 일은 `toggle` 이벤트를 내보내는 경로를 그대로 따릅니다. `event`가 `slide`이고 `value`가 실려 있다는 점만 다릅니다. 값은 소수점 셋째 자리까지 적습니다.

`docs/STATUS-PROTOCOL.md`에 행 타입 `slider`와 이벤트 `slide`를 추가하고, 값의 범위(0.0~1.0)와 스냅 단위(0.02)를 적습니다.

---

## 3. 볼륨 읽기와 쓰기

### 3-1. 새 파일 `src/widgets/volume.hpp` / `.cpp`

Core Audio를 감싸는 얇은 클래스 하나만 만듭니다. 스레드도 만들지 않고 락도 걸지 않습니다. **`BuiltinWidgets`의 작업자 스레드에서만** 호출한다는 전제로 씁니다.

```cpp
#pragma once

#include <string>
#include <windows.h>
#include <wrl/client.h>

struct IAudioEndpointVolume;
struct IMMDeviceEnumerator;

namespace bamti {

struct VolumeState {
  bool ok = false;
  float level = 0.0f;   // 0.0 ~ 1.0
  bool muted = false;
  std::wstring device;  // 기본 출력 장치의 표시 이름
};

class VolumeControl {
 public:
  VolumeControl() = default;
  VolumeControl(const VolumeControl&) = delete;
  VolumeControl& operator=(const VolumeControl&) = delete;
  ~VolumeControl();

  VolumeState Read();
  bool SetLevel(float level);
  bool SetMute(bool muted);
  void Release();

 private:
  bool Ensure();

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume_;
  std::wstring device_;
  bool logged_cost_ = false;
  bool logged_fail_ = false;
};

}  // namespace bamti
```

`.cpp`에서 `<mmdeviceapi.h>`, `<endpointvolume.h>`, `<functiondiscoverykeys_devpkey.h>`, `<propvarutil.h>`를 포함합니다. 링크에는 `ole32`와 `propsys`가 이미 들어 있어 `CMakeLists.txt`의 `target_link_libraries`는 그대로 두어도 됩니다. 빌드가 심벌을 못 찾으면 그때 추가하십시오.

### 3-2. `Ensure`

1. `enumerator_`가 비어 있으면 `CoCreateInstance(__uuidof(MMDeviceEnumerator), ...)`로 만듭니다.
2. `volume_`이 비어 있으면 `GetDefaultAudioEndpoint(eRender, eMultimedia, &device)`로 장치를 얻고, `device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr, &volume_)`로 인터페이스를 얻습니다.
3. 같은 기회에 `IPropertyStore`로 `PKEY_Device_FriendlyName`을 읽어 `device_`에 담습니다.
4. 어느 단계든 실패하면 `Release()`로 전부 비우고 거짓을 돌려줍니다. 실패 로그는 `logged_fail_`로 한 번만 남깁니다. 성공하면 `logged_fail_`을 다시 거짓으로 돌려서, 장치가 빠졌다가 돌아오는 왕복이 각각 한 번씩 기록되게 합니다.

**비용 게이트.** 처음 `Ensure`가 성공할 때 `QueryPerformanceCounter`로 잰 소요 시간을 한 번만 기록합니다. `builtin.cpp`가 `GetIfTable2 took %.2f ms`로 남기는 방식을 그대로 따르십시오.

```
[widget] volume endpoint acquire took %.2f ms
```

`Read`의 소요 시간도 첫 회에 한 번 남깁니다. **1ms를 넘으면 그 사실을 검증 보고에 적으십시오.** 주기를 늦추거나 캐시 전략을 바꿀지는 그 숫자를 보고 정합니다. 지금 미리 최적화하지 마십시오.

### 3-3. 기본 장치가 바뀌는 경우

`volume_`을 캐시해 두면 기본 장치가 바뀌어도 옛 장치를 계속 가리킵니다. 표본마다 `GetDefaultAudioEndpoint`를 다시 부르면 확실하지만 비용이 듭니다.

**다음 규칙으로 처리합니다.**

- 평소에는 캐시한 `volume_`을 씁니다.
- `Read`나 `SetLevel`이나 `SetMute`가 `AUDCLNT_E_DEVICE_INVALIDATED`나 그 밖의 실패를 돌려주면 `Release()` 후 다음 표본에서 다시 얻습니다.
- 그것만으로는 "장치는 살아 있는데 기본값이 다른 장치로 바뀐" 경우를 잡지 못합니다. 이를 위해 **20초마다 한 번** `Ensure`를 강제로 다시 하도록 `BuiltinWidgets` 쪽에서 `Release()`를 호출합니다. 20초는 재획득 비용(3-2절의 측정값)이 밝혀지기 전의 잠정값입니다. 획득이 1ms를 넘으면 보고에 적으십시오.

`IMMNotificationClient`는 이번에 만들지 마십시오. 콜백 등록은 아파트먼트 문제를 함께 끌고 들어오는데, 지금 얻는 이득이 그 위험에 못 미칩니다.

### 3-4. 아파트먼트에 대한 주의

`BuiltinWidgets::WorkerLoop`은 `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`로 STA를 열고, 메시지 펌프 없이 `WaitForMultipleObjects`로 대기합니다. `MMDeviceEnumerator`는 프로세스 안에서 도는 서버라 이 상태에서도 호출이 됩니다.

여기서 지켜야 할 것이 둘입니다.

1. `VolumeControl`이 들고 있는 인터페이스 포인터를 **작업자 스레드 밖에서 쓰지 마십시오.** 메뉴 스레드나 콜백에서 만지면 아파트먼트를 넘는 호출이 되고, 펌프가 없어 멈춥니다.
2. `IAudioEndpointVolumeCallback`을 **등록하지 마십시오.** 값 변경은 3-5절의 폴링으로 따라갑니다. 콜백은 임의 스레드에서 들어오는데, 지금 구조에서 그 값을 안전하게 받아 넘길 자리가 마땅치 않습니다.

`CoInitializeEx`를 MTA로 바꾸지 마십시오. 같은 스레드가 `ShellExecuteW`를 부르고 있습니다.

### 3-5. 표본 주기

```cpp
constexpr ULONGLONG kVolumePeriodMs = 1000;
```

1초입니다. 키보드 볼륨 키를 눌렀을 때 최대 1초 늦게 따라옵니다. 배터리(60초)나 CPU(5초)보다 잦지만, `Read`는 캐시한 인터페이스에 대고 두 번 호출하는 것이 전부라 `GetIfTable2`(2초 주기, 이 컴퓨터에서 첫 회 2.27ms)보다 훨씬 쌉니다.

**유휴 CPU 예산(0.2%)을 넘으면 주기를 늘리지 말고 먼저 원인을 재십시오.** 1초 폴링이 원인인지, `Ensure` 재획득이 원인인지를 3-2절 측정으로 가릅니다.

`SetActive(false)`(전체화면 가림, 잠금, 화면 꺼짐)일 때 표본을 멈추는 기존 규칙을 그대로 따릅니다. `SampleDue`의 판정에 볼륨을 더하고, `HasSampleDeadlineLocked`와 `NextDeadlineLocked`에도 더하십시오. **`settings_.volume`이 꺼져 있으면 `VolumeControl::Release()`를 불러 인터페이스를 놓으십시오.** 쓰지 않는 오디오 장치를 붙잡고 있을 이유가 없습니다.

---

## 4. 위젯 사양 (`bamti.widget/volume`)

### 4-1. 상수

```cpp
constexpr char kVolumeId[] = "bamti.widget/volume";
constexpr int kVolumePriority = 35;   // 배터리(40)와 CPU(30) 사이
constexpr wchar_t kVolumeGlyph[] = L"♪";       // ♪
constexpr wchar_t kVolumeMuteGlyph[] = L"♪";   // 같은 글리프에 상태로 구분한다
```

**글리프는 실제로 그려 보고 정하십시오.** 스피커 모양 문자(U+1F508~U+1F50A)는 이모지 폰트로 넘어가 다른 위젯과 색과 크기가 어긋날 수 있고, Segoe MDL2의 사설 영역 글리프는 이 팝업이 쓰는 폰트에 없습니다. `♪`가 두부(□)로 나오면 `~`나 `≈` 같은 기본 다국어 평면의 문자로 대체하고, 무엇을 왜 골랐는지 검증 보고에 한 줄로 적으십시오.

### 4-2. 막대 표시

- 정상: 글리프 + `72%` 형식의 텍스트. `state = kNormal`.
- 음소거: 글리프 + `음소거`. `state = kOff`.
- 툴팁: `볼륨 72% · DELL U4025QW`처럼 값과 장치 이름을 붙입니다. 음소거면 `볼륨 음소거 · <장치>`.
- 장치를 얻지 못하면 `DropItem(kVolumeId)`으로 항목을 내립니다. 배터리가 없을 때 하는 처리와 같습니다.

### 4-3. 패널

```
제목      볼륨
부제목    <기본 출력 장치 이름>

[슬라이더] 볼륨                    72%
[토글]     음소거                   ( )
─────────────────────────────
[단추]     소리 설정 열기
```

- 슬라이더 행의 `row_id`는 `volume_level`입니다.
- 토글 행의 `row_id`는 `volume_mute`입니다.
- 단추 행의 `row_id`는 `sound_settings`이고, `ms-settings:sound`를 엽니다. 기존 `PendingAction`에 항목을 더해 `Execute`에서 `ShellOpen`으로 처리하십시오.

### 4-4. 이벤트 처리

`BuiltinWidgets::OnEvent`에 다음을 더합니다.

```cpp
if (ev.event == "slide" && ev.id == kVolumeId && ev.row_id == "volume_level") {
  std::lock_guard lock(mu_);
  pending_level_ = ClampUnit(ev.value);   // std::optional<float>
} else if (ev.event == "toggle" && ev.id == kVolumeId && ev.row_id == "volume_mute") {
  std::lock_guard lock(mu_);
  pending_mute_ = ev.on;                  // std::optional<bool>
}
// 이어서 wake_event_를 SetEvent 한다
```

**액션 큐(`actions_`)에 넣지 마십시오.** 드래그 한 번에 수십 개의 `slide`가 오는데, 큐에 쌓으면 손을 뗀 뒤에도 밀린 값들이 차례로 적용되어 손잡이가 뒤늦게 움직입니다. `std::optional`에 **마지막 값만 덮어씁니다.**

작업자 루프에서는 `acts.swap(actions_)`와 같은 자리에서 `pending_level_`과 `pending_mute_`를 꺼내 비우고, 값이 있으면 `SetLevel` / `SetMute`를 부른 뒤 **곧바로 `SampleVolume()`을 한 번 더 돌려** 새 값을 게시합니다. 그래야 패널이 1초를 기다리지 않고 갱신됩니다.

### 4-5. 지문

`Publish`의 지문 슬롯에 `fp_volume_`을 더하고, `DropItem`과 `SetSettings`의 `note_drop`에도 `kVolumeId`를 더하십시오. `drop` 배열의 크기가 `const char* drop[4]`로 고정되어 있으니 **5로 늘리십시오.** 그대로 두면 네 항목을 동시에 끌 때 배열을 넘어 씁니다.

---

## 5. 설정과 메뉴

### 5-1. `WidgetSettings`

```cpp
bool volume = false;
bool Any() const { return battery || cpu || network || volume || widget_board; }
```

`LoadWidgetSettings`에 `s.volume = json::GetBool(*widgets, "volume").value_or(false);`를, `FormatSettings`에 `"volume"` 키를 더합니다. 키 순서는 `network` 다음, `widget_board_button` 앞입니다.

### 5-2. 메뉴

`src/menu_bar.cpp`의 명령 번호는 10~13이 위젯, 14~18과 20~22가 트레이 관련으로 이미 차 있습니다. **볼륨은 19를 씁니다.**

```cpp
constexpr UINT kWidgetVolumeCmd = 19;
```

여기서 함정이 하나 있습니다. 지금 명령 처리는 범위로 판정합니다.

```cpp
if (cmd >= kWidgetBatteryCmd && cmd <= kWidgetBoardCmd) {
```

19는 이 범위 밖이고, 범위를 19까지 넓히면 14~18의 트레이 명령이 함께 걸려듭니다. **범위 판정을 명시적인 비교로 바꾸십시오.**

```cpp
if (cmd == kWidgetBatteryCmd || cmd == kWidgetCpuCmd || cmd == kWidgetNetworkCmd ||
    cmd == kWidgetVolumeCmd || cmd == kWidgetBoardCmd) {
```

같은 블록 안의 `else { next.widget_board = !next.widget_board; }`도 `else if (cmd == kWidgetBoardCmd)`로 바꾸십시오. 지금은 마지막 `else`가 무엇이든 받아 버립니다.

`ShowContextMenu`에는 네트워크 다음 줄에 항목을 더합니다.

```cpp
AppendMenuW(menu, MF_STRING | (s.volume ? MF_CHECKED : 0), kWidgetVolumeCmd, L"볼륨");
```

---

## 6. 하지 말아야 할 것

- **`RowType` 열거형의 기존 순서를 바꾸지 마십시오.** `kSlider`는 반드시 맨 뒤에 붙입니다.
- **`kGauge`의 동작을 바꾸지 마십시오.** 배터리와 CPU 패널이 그대로 그려져야 합니다.
- **볼륨 전용 스레드를 만들지 마십시오.** 작업자 스레드는 하나입니다.
- **`IAudioEndpointVolumeCallback`이나 `IMMNotificationClient`를 등록하지 마십시오.**
- **`CoInitializeEx`의 아파트먼트를 바꾸지 마십시오.**
- 드래그 중에 `hot_`을 갱신하지 마십시오. 손잡이가 깜빡입니다.
- 슬라이더에 토글의 되돌리기 타이머(`kToggleTimerId`)를 붙이지 마십시오.
- `status_panel.cpp`의 `hits_` 채우는 순서와 소비하는 순서를 어긋나게 하지 마십시오.
- 이 지시서에 없는 위젯(날씨, 마이크 볼륨, 앱별 볼륨 믹서)을 만들지 마십시오.

---

## 7. 검증

빌드는 Debug와 Release를 모두 `/W4` 경고 없이 통과해야 합니다.

손으로 확인할 것은 다음과 같습니다.

1. 메뉴에서 볼륨을 켜면 상단바에 나타나고, bamti를 다시 띄워도 켜진 채로 있습니다.
2. 위젯을 클릭해 패널을 열고 슬라이더를 **끝에서 끝까지 천천히 끕니다.** 시스템 볼륨이 따라 움직이고, 패널이 닫히지 않고, 손잡이가 뒤로 튀지 않습니다.
3. 커서를 팝업 **밖으로 끌고 나갔다가** 다시 들어옵니다. 값이 계속 따라오고 팝업이 닫히지 않습니다.
4. 슬라이더의 왼쪽 끝과 오른쪽 끝을 각각 한 번씩 클릭합니다. 0%와 100%가 되고 손잡이가 트랙 밖으로 나가지 않습니다.
5. 음소거 토글을 두 번 눌러 켰다 끕니다. 시스템 음소거가 따라 바뀌고 패널이 닫히지 않습니다.
6. 패널을 **연 채로** 키보드 볼륨 키를 누릅니다. 1초 안에 슬라이더가 따라 움직입니다.
7. 배터리 패널과 CPU 패널을 열어 게이지가 예전 그대로인지 봅니다.
8. 기본 출력 장치를 다른 장치로 바꿉니다. 20초 안에 부제목과 값이 새 장치의 것으로 바뀝니다.
9. 네 위젯을 모두 켜고 10분 유휴 상태에서 CPU 사용률을 잽니다. 0.2% 미만이어야 합니다.
10. 로그에서 다음을 확인해 검증 보고에 숫자를 그대로 적습니다.
    - `[widget] volume endpoint acquire took ... ms`
    - `[widget] volume read took ... ms`

각 항목의 결과를 적어 보고하십시오. **하지 못한 확인은 하지 못했다고 적으십시오.** 되었다고 적힌 항목은 실제로 해 본 것이어야 합니다.

---

## 8. 커밋

작업 단위로 나누어 커밋합니다. 메시지는 저장소의 기존 관례를 따릅니다(한국어 한 줄, `feat:` / `fix:` 접두어, 평서형 종결).

권장 분할입니다.

1. `feat: 상태 패널에 슬라이더 행과 드래그를 추가한다` — 프로토콜, 팝업, 패널
2. `feat: 볼륨 위젯을 추가한다` — `volume.hpp/.cpp`, `builtin`, 설정, 메뉴
3. `docs: 슬라이더 행과 slide 이벤트를 프로토콜에 적는다`
