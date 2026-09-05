# 작업 지시서: Ctrl을 누른 채 상단바 아이콘을 끌어 순서를 바꾼다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-ICON-ART.md` 다음에 하십시오. 두 작업이 `src/bar_layout.cpp`와 `src/menu_bar.cpp`를 함께 건드리므로 순서를 지켜야 충돌이 없습니다.

---

## 0. 완료 조건

1. `Ctrl`을 누른 채 상단바의 상태 아이콘을 끌면 아이콘이 따라 움직이고, 놓은 자리에 자리를 잡습니다.
2. 바꾼 순서가 `settings.json`에 저장되고 다시 켜도 유지됩니다.
3. `Ctrl` 없이 누르면 지금처럼 패널이 열립니다. 끌기가 클릭을 잡아먹지 않습니다.
4. 시작 단추와 시계는 끌 수 없습니다.

---

## 1. 지금 순서는 어떻게 정해지는가

`src/status_registry.cpp`의 `Snapshot`이 정렬합니다.

```cpp
std::sort(out.begin(), out.end(), [](const StatusItem& a, const StatusItem& b) {
  if (a.priority != b.priority) {
    return a.priority > b.priority;
  }
  return a.id < b.id;
});
```

우선순위가 높은 것이 앞에 오고, 같으면 아이디 순입니다. 내장 위젯의 우선순위는 배터리 40, 볼륨 35, CPU 30, 네트워크 20, 위젯 보드 10으로 코드에 박혀 있습니다.

`bar_layout.cpp`는 이 순서를 그대로 받아 **오른쪽에서 왼쪽으로** 놓습니다. 시계 왼쪽부터 시작해 `cursor`를 왼쪽으로 줄여 나갑니다. 즉 **목록의 앞이 화면의 오른쪽**입니다. 이 방향을 헷갈리면 끌기 방향이 뒤집힙니다.

---

## 2. 사용자 순서를 어디에 둘 것인가

### 2-1. 레지스트리가 아니라 상단바가 정한다

`StatusRegistry`는 항목을 모으고 정렬하는 곳이고 설정을 모릅니다. 사용자 순서를 넣겠다고 레지스트리에 설정 의존을 만들지 마십시오.

`MenuBar::Paint`가 `status_.Snapshot()`을 곧바로 `layout_.Compute`에 넘기고 있습니다.

```cpp
layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), status_.Snapshot());
```

이 사이에 재정렬 한 단계를 넣습니다.

```cpp
std::vector<StatusItem> MenuBar::OrderedItems() const;
```

`Snapshot()`을 받아 사용자 순서를 적용해 돌려줍니다. `Paint`와 `RefreshLayout` 등 `Compute`를 부르는 모든 자리에서 이 함수를 쓰십시오. 한 곳이라도 빠뜨리면 그릴 때와 히트 판정할 때 순서가 달라져서 엉뚱한 항목이 눌립니다. **`Compute` 호출 지점을 전부 찾아 바꾸십시오.**

### 2-2. 순서 목록의 규칙

`WidgetSettings`에 더합니다.

```cpp
inline constexpr size_t kBarOrderMax = 64;

struct WidgetSettings {
  ...
  std::vector<std::string> bar_order;   // 화면 오른쪽부터의 순서
};
```

재정렬 규칙입니다.

1. `bar_order`에 있는 아이디를 그 순서대로 먼저 놓습니다. 지금 화면에 없는 아이디는 건너뜁니다.
2. `bar_order`에 없는 항목은 뒤에 붙입니다. 이때 원래 정렬(우선순위, 아이디)을 유지합니다.

이렇게 하면 새로 생긴 위젯이나 새로 미러된 트레이 아이콘이 자동으로 왼쪽 끝에 붙고, 사용자가 손댄 것만 자리를 지킵니다.

`settings.cpp`의 저장과 읽기는 `tray_hidden_keys`와 같은 방식으로 하십시오. 문자열 배열이고, 읽을 때 `kBarOrderMax`로 자르는 것까지 같습니다.

---

## 3. 끌기

### 3-1. 시작

`WM_LBUTTONDOWN`에서 판정합니다. 지금은 시작 단추만 보고 나머지는 `break`로 빠집니다.

```cpp
case WM_LBUTTONDOWN: {
  POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
  if (HitStart(pt)) { ... }
  if ((GetKeyState(VK_CONTROL) & 0x8000) != 0) {
    if (const BarSegment* seg = HitSegment(pt)) {
      if (seg->kind == SegmentKind::kStatus) {
        BeginReorder(seg->id, pt);
        return 0;
      }
    }
  }
  ...
}
```

`HitSegment`는 `kStatus`와 `kOverflow`만 돌려줍니다. **넘침 단추(`kOverflow`)는 끌 수 없게 하십시오.** 넘침 단추는 항목이 아니라 나머지를 여는 손잡이입니다.

`BeginReorder`는 다음을 합니다.

- 끄는 항목의 아이디를 기억합니다.
- `SetCapture(hwnd_)`를 부릅니다.
- 열려 있는 상태 패널을 닫습니다(`status_popup_.Close()`).
- 끌기 시작 지점을 기억합니다.

`SetCapture`를 부르면 `WM_CAPTURECHANGED`가 옵니다. 지금 그 처리기는 `start_pressed_`만 되돌립니다. 끌기 중에 캡처를 빼앗기면(다른 창이 캡처를 가져가거나 사용자가 Alt+Tab을 누르면) **끌기를 취소하고 원래 순서로 되돌리십시오.** 어중간한 상태로 남기지 마십시오.

### 3-2. 문턱

누르자마자 끌기로 보면 안 됩니다. `Ctrl+클릭`을 하려던 사용자가 손을 조금만 떨어도 순서가 바뀝니다.

가로로 `GetSystemMetrics(SM_CXDRAG)` 픽셀만큼 움직이기 전에는 아무것도 하지 마십시오. 문턱을 넘은 뒤부터 자리바꿈을 계산합니다.

문턱을 넘지 않은 채 손을 떼면 **아무 일도 일어나지 않아야 합니다.** 패널을 열지도 마십시오. `Ctrl`을 누른 클릭은 끌기 의도이지 열기 의도가 아닙니다.

### 3-3. 자리바꿈

`WM_MOUSEMOVE`에서, 끌기 중이라면 지금 마우스의 x 좌표가 어느 항목 위에 있는지 봅니다.

`layout_.last().segments`에서 `kStatus`인 것만 왼쪽에서 오른쪽 순으로 훑어, 마우스 x가 **그 세그먼트의 가운데를 넘었는지**로 판정하십시오. 세그먼트 사각형 안에 들어왔는지로 판정하면 항목 사이 간격에서 아무 일도 일어나지 않아 끌기가 끊겨 보입니다.

끄는 항목을 목록에서 빼고, 판정된 자리에 다시 끼워 넣습니다. 그 결과를 곧바로 `bar_order`의 작업본에 반영하고 다시 그리십시오. **놓기 전에 화면이 미리 바뀌는 방식**입니다. 반투명 유령 아이콘을 따로 그리지 마십시오. 그리기 경로가 복잡해지고 얻는 것이 적습니다.

자리가 실제로 바뀐 경우에만 `InvalidateRect`를 부르십시오. 마우스가 움직일 때마다 바 전체를 다시 그리면 CPU를 씁니다.

### 3-4. 끝

`WM_LBUTTONUP`에서 끌기 중이었다면 다음을 합니다.

- `ReleaseCapture()`
- 문턱을 넘었다면 지금 순서를 `WidgetSettings::bar_order`에 확정하고 `SaveWidgetSettings`를 부릅니다.
- 클릭 이벤트를 **보내지 마십시오.** `status_.Dispatch`도 `OpenStatusPanel`도 부르면 안 됩니다.

지금 `WM_LBUTTONUP`에는 `skip_left_up_` 깃발이 이미 있습니다(더블클릭이 클릭을 겹쳐 내는 것을 막는 용도). 끌기에 그 깃발을 재사용하지 말고 별도 상태로 두십시오. 뜻이 다른 두 가지를 한 깃발에 섞으면 나중에 한쪽을 고칠 때 다른 쪽이 깨집니다.

### 3-5. Esc로 취소

끌기 중에 `Esc`를 누르면 원래 순서로 되돌리고 끌기를 끝내십시오. 상단바는 `WS_EX_NOACTIVATE`라 키 입력을 직접 받지 못하지만, 캡처를 쥐고 있는 동안에는 `WM_KEYDOWN`이 옵니다. 오지 않으면 이 기능은 넣지 말고, 넣지 않았다고 보고하십시오. **동작하지 않는 기능을 넣는 것보다 없는 편이 낫습니다.**

### 3-6. 커서

끌기 중에는 `WM_SETCURSOR`에서 `IDC_SIZEWE`를 돌려주십시오. 지금 그 처리기가 시작 단추 위에서 `IDC_HAND`를 돌려주는 구조이므로 갈래를 하나 더하면 됩니다.

`Ctrl`을 누른 채 상태 아이콘 위에 있을 때에도 같은 커서를 보여 주면 "끌 수 있다"는 것이 드러납니다. 넣으십시오.

---

## 4. 저장 시점

`SaveWidgetSettings`는 파일을 씁니다. 끌기 도중에 부르지 마십시오. 마우스를 움직일 때마다 디스크에 쓰게 됩니다.

**손을 뗄 때 한 번만** 저장하십시오.

`ApplySettings`가 `widgets_`와 `tray_`에 설정을 다시 밀어 넣는데, 순서는 두 곳 모두와 무관합니다. 순서만 바뀐 경우에는 `ApplySettings`를 부르지 말고 저장만 하십시오. 위젯을 껐다 켜는 부작용이 생깁니다.

---

## 5. 하지 말아야 할 것

- 항목의 `priority`를 고쳐서 순서를 바꾸지 마십시오. `priority`는 위젯이 스스로 정하는 값이고, 다음 표본에서 덮어써집니다.
- `StatusRegistry::Snapshot`의 정렬을 건드리지 마십시오. 순서를 모르는 다른 사용처(넘침 팝업 등)가 깨집니다.
- 넘침 팝업 안에서는 끌기를 지원하지 마십시오. 이번 범위가 아닙니다.
- 시계와 시작 단추와 경고 문구 세그먼트는 끌기 대상이 아닙니다.
- `Ctrl` 없이 끄는 것은 지원하지 마십시오. 클릭과 구분되지 않습니다.
- 레지스트리에 쓰지 마십시오.

---

## 6. 검증

1. Release 빌드가 경고 없이 통과합니다.
2. 위젯 네 개를 모두 켭니다. `Ctrl`을 누른 채 배터리 아이콘을 왼쪽으로 끕니다. 끄는 동안 아이콘이 다른 아이콘 사이로 미끄러져 들어갑니다.
3. 손을 뗍니다. 순서가 유지됩니다.
4. `%LOCALAPPDATA%`의 `settings.json`을 열어 `bar_order`에 항목 아이디가 순서대로 들어 있는지 봅니다.
5. bamti를 끄고 다시 켭니다. 순서가 그대로입니다.
6. `Ctrl` 없이 아이콘을 클릭합니다. 패널이 평소대로 열립니다.
7. `Ctrl`을 누른 채 아이콘을 **누르기만 하고 움직이지 않고** 뗍니다. 패널이 열리지 않고 순서도 바뀌지 않습니다.
8. 끄는 도중에 `Alt+Tab`으로 다른 창으로 갑니다. 끌기가 취소되고 순서가 원래대로 돌아옵니다.
9. 위젯 하나를 껐다가 다시 켭니다. 그 항목이 저장된 자리로 돌아옵니다.
10. 위젯을 전부 끄고 트레이 미러만 남긴 상태에서도 끌기가 동작합니다.

---

## 7. 커밋

```
feat: Ctrl을 누른 채 끌어 상단바 아이콘 순서를 바꾼다
feat: 상단바 아이콘 순서를 설정에 저장한다
```
