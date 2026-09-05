# 작업 지시서 19: 마우스 이동 메시지 경고는 오탐이다

`[perf] msg flood 560/s top=[bamti.Dock:0x0200 x503]` 경고를 조사했습니다. **결론부터 말하면 폭주가 아닙니다.** 정상적인 마우스 입력이 낮게 잡힌 경고 임계값을 넘겼을 뿐입니다. 다만 그 과정에서 마우스 이동 처리 경로에 불필요한 시스템 호출이 있다는 것을 확인했으므로, 임계값과 함께 그 부분을 정리합니다.

---

## 1. 측정 결과

### 1.1 임계값이 정상 입력보다 낮다

`src/host.cpp:21`입니다.

```cpp
constexpr UINT kMsgFloodLimit = 500;
```

그리고 로그에 남은 모든 경고의 실측값입니다.

```
512/s  544/s  554/s  512/s  555/s  525/s  560/s
```

**전부 512에서 560 사이**입니다. 임계값 500을 10퍼센트 남짓 넘긴 값이고, 그중 대부분이 `bamti.Dock`의 `0x0200`(`WM_MOUSEMOVE`)입니다.

폴링 레이트가 500Hz에서 1000Hz인 마우스는 초당 500건 안팎의 `WM_MOUSEMOVE`를 보냅니다. 관측값은 그 범위 안에 정확히 들어옵니다.

### 1.2 지속되지 않는다

로그 10,000줄이 넘는 기간 동안 `msg flood`는 **일곱 건**뿐입니다. 프로그램을 22:33에 띄우고 22:41까지 관찰하는 동안에는 두 건이었습니다. 마우스를 움직이지 않는 구간에는 한 건도 남지 않았습니다.

되먹임 구조라면 조건이 유지되는 내내 매초 찍혀야 합니다. 산발적으로만 나타난다는 것은 **사람이 마우스를 움직인 순간에만 발생한다**는 뜻입니다.

### 1.3 진짜 폭주와는 규모가 100배 다르다

지시서 10에서 고쳤던 `WM_MOUSELEAVE` 되먹임의 실측값입니다.

```
[perf] msg flood 57971/s top=[bamti.Dock:0x02A3 x57963]
```

초당 **57,971건**이었습니다. 지금의 560건과는 규모가 완전히 다릅니다. 현재 임계값 500은 저 되먹임을 잡으려고 정한 값인데, 실제로는 정상 입력까지 함께 걸러 내고 있습니다.

### 1.4 성능 지표에 이상이 없다

같은 시간대의 로그입니다.

```
[perf] rebuild skip fingerprint items=18 0ms
[perf] draw bind=0.40 brush=0.01 begin=0.70 draw=0.28 end=1.11 (ms, avg)
[perf] bar full[n=23 avg=4.7 max=7.3]ms
```

렌더링과 재구성 모두 정상 범위입니다. UI 스레드가 포화된 흔적이 없습니다.

### 1.5 처리 경로 자체는 잘 짜여 있다

`src/dock.cpp`의 `WM_MOUSEMOVE` 핸들러를 확인했습니다.

- `CancelHideTimer()`는 `hide_armed_` 플래그로 막혀 있어 실제 `KillTimer` 호출은 드뭅니다.
- `RenderLayered()`는 `hit != hover_`일 때만 부릅니다. 같은 슬롯 안에서 움직이면 다시 그리지 않습니다.

여기까지는 손댈 곳이 없습니다.

### 1.6 다만 매번 실행되는 시스템 호출이 하나 있다

`ArmMouseLeave`(`src/dock.cpp:2161`)에는 가드가 없습니다.

```cpp
void Dock::ArmMouseLeave() {
  if (hwnd_ == nullptr || !shown_) {
    return;
  }
  TRACKMOUSEEVENT track{};
  ...
  TrackMouseEvent(&track);   // ← WM_MOUSEMOVE마다 무조건 호출된다
}
```

`TrackMouseEvent`의 `TME_LEAVE` 등록은 **한 번 걸면 마우스가 창을 떠날 때까지 유효**합니다. 떠나면 `WM_MOUSELEAVE`가 한 번 오고 등록이 해제됩니다. 그러므로 이미 등록된 상태에서 다시 부르는 것은 효과가 없는 커널 호출입니다. 지금은 초당 500번 그렇게 하고 있습니다.

`ArmHotMouseLeave`(2172행)도 같은 구조이고, `HandleHot`의 `WM_MOUSEMOVE`에서 매번 호출됩니다.

---

## 2. 수정 내용

### 2-1. 경고 임계값을 올린다 (`src/host.cpp`)

```cpp
// 폴링 레이트가 높은 마우스는 정상 이동만으로도 초당 500건을 넘긴다.
// 되먹임 폭주는 만 단위로 나타나므로 그 사이에 선을 긋는다.
constexpr UINT kMsgFloodLimit = 3000;
```

3000은 정상 입력(최대 560 관측)의 다섯 배이면서, 지시서 10에서 본 되먹임(57,971)의 20분의 1입니다. 어느 쪽으로도 여유가 충분합니다.

주석을 함께 남겨 주십시오. 이 숫자를 나중에 다시 낮추지 않도록 근거가 코드에 남아야 합니다.

### 2-2. `TME_LEAVE` 재등록을 건너뛴다 (`src/dock.cpp`, `src/dock.hpp`)

`dock.hpp`의 멤버에 플래그 두 개를 추가합니다. `hide_armed_` 근처가 적당합니다.

```cpp
  bool leave_armed_ = false;
  bool hot_leave_armed_ = false;
```

`ArmMouseLeave`와 `ArmHotMouseLeave`에 가드를 넣습니다.

```cpp
void Dock::ArmMouseLeave() {
  if (hwnd_ == nullptr || !shown_ || leave_armed_) {
    return;
  }
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  if (TrackMouseEvent(&track) != FALSE) {
    leave_armed_ = true;
  }
}
```

`ArmHotMouseLeave`도 `hot_hwnd_`와 `hot_leave_armed_`로 같게 고칩니다.

**해제 지점을 반드시 함께 넣어야 합니다.** 등록은 `WM_MOUSELEAVE`가 배달되면 자동으로 풀리므로, 플래그도 그때 내려야 다음 진입에서 다시 등록됩니다.

- `Dock::HandleMessage`의 `case WM_MOUSELEAVE:` 맨 앞에서 `leave_armed_ = false;`
- `Dock::HandleHot`의 `case WM_MOUSELEAVE:` 맨 앞에서 `hot_leave_armed_ = false;`

창을 숨길 때도 내려 주십시오. `HidePill`에서 `leave_armed_ = false;`를 실행합니다. 창이 숨겨지면 `ArmMouseLeave`의 `!shown_` 가드에 걸려 등록이 불가능한데, 플래그가 참으로 남아 있으면 다시 보일 때 영영 등록되지 않습니다.

이 해제 처리를 빠뜨리면 **마우스가 독을 떠나도 숨지 않는 결함**이 생깁니다. 검증 3항에서 반드시 확인해 주십시오.

---

## 3. 검증

1. 빌드한 뒤 실행하고, 마우스를 독 위에서 30초 정도 활발히 움직입니다. 로그에 `[perf] msg flood`가 **더 이상 나오지 않아야** 합니다.
2. 독을 띄우고 마우스를 바깥으로 빼면 독이 정상으로 숨어야 합니다. 여러 번 반복해서 항상 숨는지 확인합니다. 2-2의 플래그 해제가 제대로 들어갔는지 보는 항목이며, 이번 수정에서 가장 깨지기 쉬운 부분입니다.
3. 화면 아래 가장자리에 마우스를 대면 독이 다시 나타나야 합니다. `hot_leave_armed_` 쪽 확인입니다.
4. 아이콘 위에서 옆 아이콘으로 옮길 때 호버 표시가 따라와야 합니다. `WM_MOUSEMOVE` 처리 자체는 건드리지 않았으므로 그대로여야 합니다.
5. 드래그로 아이콘 순서를 바꾸는 동작이 그대로인지 확인합니다.

---

## 4. 주의 사항

- 이번 작업은 **성능 문제를 고치는 것이 아닙니다.** 1.4에서 보았듯 성능 지표에 이상이 없습니다. 목적은 오탐 경고를 없애 진짜 폭주가 묻히지 않게 하는 것과, 효과 없는 커널 호출을 걷어내는 것입니다. 이 범위를 넘어 마우스 처리 구조를 다시 짜지 마십시오.
- `WM_MOUSEMOVE`에서 좌표가 직전과 같으면 건너뛰는 최적화는 **넣지 마십시오.** 그런 중복이 실제로 오는지 측정으로 확인하지 않았습니다. 근거 없는 최적화입니다.
- `ShowPill()`이 `TaskbarController::Rehide()`를 무조건 호출하는 점을 확인했지만, 이는 `bamti.DockHot`의 `WM_MOUSEMOVE` 경로이고 관측값이 초당 23건에 그칩니다. 이번 범위 밖이므로 손대지 마십시오.
