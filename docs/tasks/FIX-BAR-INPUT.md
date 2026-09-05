# 수정 지시서: 재정렬 ESC 취소를 없애고, 휠로 볼륨을 조절한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

이 지시서를 가장 먼저 하십시오. 다른 네 지시서와 파일이 겹치지 않습니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, `src/status_source.hpp`, `src/widgets/builtin.cpp`, `docs/STATUS-PROTOCOL.md`입니다.

---

## 1. 재정렬 중 ESC 취소를 없앤다

### 1-1. 현재 동작

`TASK-BAR-REORDER.md`로 들어간 기능입니다. Ctrl을 누른 채 상단바 아이콘을 끌어 순서를 바꾸는 도중에 ESC를 누르면 원래 순서로 되돌립니다.

`src/menu_bar.cpp`의 `WndProc`입니다.

```cpp
    case WM_KEYDOWN:
      if (reorder_active_ && wparam == VK_ESCAPE) {
        CancelReorder();
        return 0;
      }
      break;
```

### 1-2. 수정

사용자가 필요 없다고 판정했습니다. 위 `case WM_KEYDOWN:` 블록을 통째로 지우십시오.

**`CancelReorder`는 지우지 마십시오.** `WM_CAPTURECHANGED`에서 여전히 씁니다.

```cpp
      if (reorder_active_ && reinterpret_cast<HWND>(lparam) != hwnd_) {
        CancelReorder();
      }
```

상단바는 `WS_EX_NOACTIVATE` 창이라서 키 입력을 받을 일이 거의 없습니다. `WM_KEYDOWN`을 지운 뒤 다른 곳에서 이 메시지를 쓰고 있지 않은지 `menu_bar.cpp`를 한 번 훑어 확인하십시오.

### 1-3. 검증

1. Ctrl을 누른 채 상단바 아이콘을 끌기 시작하고, 놓기 전에 ESC를 누릅니다. 아무 일도 일어나지 않고 끌기가 계속되어야 합니다.
2. 그 상태에서 마우스를 놓으면 순서가 바뀌고 설정에 저장되어야 합니다.
3. 끌던 도중에 다른 창이 마우스 캡처를 가져가면(예: 화면 잠금) 원래 순서로 되돌아가야 합니다. 이 경로는 그대로 살아 있어야 합니다.

---

## 2. 볼륨 아이콘 위에서 휠을 돌리면 볼륨이 바뀌게 한다

### 2-1. 목표

상단바의 볼륨 위젯에 마우스 포인터를 올려 둔 채 휠을 돌리면, 팝업을 열지 않고 바로 볼륨이 오르내려야 합니다. 한 눈금(`WHEEL_DELTA` = 120)에 2%씩 움직입니다. 볼륨 슬라이더가 이미 0.02 단위로 반올림하고 있으므로 같은 단위를 씁니다.

### 2-2. 프로토콜에 `scroll` 이벤트를 더한다

`src/status_source.hpp`의 `StatusEvent` 주석을 고칩니다. 필드를 새로 만들지 말고 `value`를 재사용하십시오.

```cpp
struct StatusEvent {
  std::string id;
  std::string event;
  std::string row_id;
  std::string button;
  bool on = false;
  // event == "slide"일 때는 절대값(0.0~1.0), event == "scroll"일 때는 상대 증감이다.
  float value = 0.0f;
};
```

`docs/STATUS-PROTOCOL.md`의 이벤트 절에 `scroll`을 추가하십시오. 다음 내용을 담습니다.

- `scroll`은 상단바 세그먼트 위에서 휠을 돌렸을 때 나갑니다.
- `value`는 상대 증감이며 단위 스케일(0.0~1.0 축)입니다. 한 눈금이 `+0.02` 또는 `-0.02`입니다.
- 받는 쪽은 현재값에 더한 뒤 0.0~1.0으로 잘라 씁니다.
- 처리하지 않는 항목은 조용히 무시합니다.

### 2-3. 상단바에서 휠 메시지를 받는다

`src/menu_bar.hpp`의 `MenuBar`에 누적 필드를 하나 더합니다.

```cpp
  int wheel_accum_ = 0;
```

`src/menu_bar.cpp`의 `WndProc`에 `WM_MOUSEWHEEL`을 더합니다. **`lparam`은 클라이언트 좌표가 아니라 화면 좌표입니다.** `ScreenToClient`를 반드시 거치십시오.

```cpp
    case WM_MOUSEWHEEL: {
      POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      ScreenToClient(hwnd_, &pt);
      wheel_accum_ += GET_WHEEL_DELTA_WPARAM(wparam);
      const int notches = wheel_accum_ / WHEEL_DELTA;
      wheel_accum_ -= notches * WHEEL_DELTA;
      const auto hit = HitTest(pt);
      Log(L"bar", L"wheel notches=%d hit=%hs", notches, hit ? hit->id.c_str() : "none");
      if (notches == 0 || !hit) {
        return 0;
      }
      StatusEvent ev;
      ev.id = hit->id;
      ev.event = "scroll";
      ev.row_id = "volume_level";
      ev.value = static_cast<float>(notches) * 0.02f;
      status_.Dispatch(ev);
      return 0;
    }
```

`row_id`를 `"volume_level"`로 고정해도 됩니다. 지금 휠을 받는 항목이 볼륨뿐이고, 다른 항목은 이 이벤트를 무시하기 때문입니다. 세그먼트 종류를 가려 볼륨일 때만 보내는 방식으로 좁혀도 좋습니다. 둘 중 하나를 고르고 주석으로 이유를 남기십시오.

`GET_WHEEL_DELTA_WPARAM`과 `GET_X_LPARAM`은 `windowsx.h`에 있습니다. `menu_bar.cpp`가 이미 `GET_X_LPARAM`을 쓰고 있으므로 헤더는 들어와 있을 것입니다.

### 2-4. 위젯이 상대 증감을 처리한다

`src/widgets/builtin.cpp`의 `BuiltinWidgets::OnEvent`에 분기를 더합니다. `pending_level_`이 `std::optional<float>`이므로, 이미 보류 중인 값이 있으면 그 값을 기준으로 삼아야 휠을 빠르게 굴렸을 때 눈금이 씹히지 않습니다.

```cpp
  } else if (ev.event == "scroll" && ev.id == kVolumeId) {
    std::lock_guard lock(mu_);
    const float base = pending_level_.has_value() ? *pending_level_ : last_volume_;
    const float next = ClampUnit(base + ev.value);
    pending_level_ = next;
    if (ev.value > 0.0f && last_muted_) {
      pending_mute_ = false;  // 볼륨을 올리면 음소거를 푼다.
    }
    wake = true;
  }
```

`last_volume_`과 `last_muted_`는 작업자 스레드가 갱신하므로 반드시 `mu_`를 잡은 채 읽으십시오. `LiveForControlCenter`가 같은 방식으로 읽고 있습니다.

볼륨을 내려서 0이 되었을 때 음소거로 넘길지는 **넘기지 마십시오.** Windows의 볼륨 키도 0에서 음소거로 바뀌지 않습니다.

### 2-5. 휠 메시지가 오지 않을 가능성

상단바는 `WS_EX_NOACTIVATE` 창이라 포커스를 잡지 않습니다. `WM_MOUSEWHEEL`은 원래 포커스 창으로 갑니다. Windows 10부터는 "마우스를 가리킬 때 비활성 창 스크롤"(레지스트리 `HKCU\Control Panel\Desktop`의 `MouseWheelRouting`)이 기본으로 켜져 있어서 커서 아래 창으로 보내 주지만, 이 설정이 꺼져 있으면 메시지가 오지 않습니다.

그래서 2-3의 `Log` 한 줄을 넣게 했습니다. **먼저 만들고 측정하십시오.**

- 로그에 `wheel notches=...`가 찍히면 그대로 끝입니다.
- 한 줄도 찍히지 않으면 저수준 마우스 훅 같은 대체 경로를 **임의로 넣지 말고**, 로그와 함께 그 사실을 보고하십시오. 훅은 상주 비용과 입력 지연에 영향을 주므로 따로 판단할 사안입니다.

### 2-6. 검증

1. 상단바 볼륨 아이콘에 포인터를 올리고 휠을 위로 세 눈금 굴립니다. 볼륨이 6% 오르고 아이콘의 글리프 단계가 따라와야 합니다.
2. 아래로 굴리면 같은 폭으로 내려가고, 0%와 100%에서 더 넘어가지 않아야 합니다.
3. 음소거 상태에서 위로 굴리면 음소거가 풀리면서 볼륨이 올라야 합니다.
4. 볼륨이 아닌 세그먼트(시계, 배터리, 트레이 아이콘) 위에서 굴리면 아무 일도 없어야 합니다.
5. 휠을 빠르게 열 눈금 굴렸을 때 20% 가까이 움직여야 합니다. 한두 눈금만 반영되면 2-4의 누적 처리가 틀린 것입니다.
6. 로그에서 `wheel notches=` 줄을 확인해 메시지가 실제로 도착하는지 기록하십시오.

---

## 3. 보고할 것

- 지운 `WM_KEYDOWN` 블록과 남긴 `CancelReorder` 경로.
- `wheel notches=` 로그의 실제 출력. 도착하지 않으면 그 사실.
- 휠 한 눈금이 실제로 몇 %를 움직였는지.
