# 작업 지시서: 독 우클릭 메뉴를 커서가 아니라 아이콘 위에 정렬한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/dock.cpp`, `src/dock.hpp`, `src/popup_surface.cpp`, `src/popup_surface.hpp` 입니다. 꼬리 모양은 이 지시서의 대상이 아닙니다. 그것은 `FIX-POPUP-TAIL.md` 에서 이어서 합니다. **이 지시서를 먼저 끝내십시오.**

---

## 1. 무엇이 어긋났는가

맥의 독 우클릭 메뉴는 **누른 아이콘의 가로 중심에 맞추어** 독 위에 뜹니다. 커서가 아이콘의 어디에 있었는지는 위치에 영향을 주지 않습니다. bamti 는 커서 자리에서 그대로 열리기 때문에, 같은 아이콘을 눌러도 매번 다른 자리에 뜹니다.

지금은 오른쪽 버튼을 뗀 자리를 그대로 앵커로 넘깁니다.

```cpp
    case WM_RBUTTONUP: {
      ...
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int index = HitTest(pt);
      if (index >= 0) {
        POINT screen = pt;
        ClientToScreen(hwnd_, &screen);
        OpenDockMenu(screen, index);
      }
```

그리고 `PopupSurface::Place` 는 그 앵커의 왼쪽에서 8 DIP 만 물러난 자리에 창을 놓습니다.

```cpp
    x = anchor_screen.x - inset;
    y = mode == Anchor::AboveAt ? anchor_screen.y - size.cy - gap : anchor_screen.y + gap;
```

## 2. 어디에 놓을 것인가

- 가로: **메뉴의 가운데를 아이콘의 가로 중심에 맞춥니다.** 화면 밖으로 나가면 지금처럼 화면 안으로 끌어 넣습니다.
- 세로: 지금과 같습니다. 독 창의 위쪽에서 4 DIP 띄운 자리에 메뉴의 아래쪽이 옵니다.

호버 라벨이 이미 같은 기준으로 자리를 잡고 있습니다(`Dock::UpdateHoverLabel`). 그 계산을 함수로 빼서 메뉴와 라벨이 같은 앵커를 쓰게 합니다.

## 3. 고칠 것

### 3.1 아이콘 앵커를 함수로 뺀다

`src/dock.hpp` 의 private 선언부, `RevealDockApp` 선언 부근에 넣습니다.

```cpp
  // 아이콘의 가로 중심(화면 좌표)과 독 창의 위쪽 변. 호버 라벨과 우클릭 메뉴가 함께 쓴다.
  bool IconAnchor(int index, POINT* center, int* dock_top) const;
```

`src/dock.cpp` 에 정의를 넣습니다. 자리는 `Dock::UpdateHoverLabel` 바로 앞이 좋습니다. 내용은 지금 `UpdateHoverLabel` 안에 있는 계산 그대로입니다.

```cpp
bool Dock::IconAnchor(int index, POINT* center, int* dock_top) const {
  if (hwnd_ == nullptr || index < 0 || index >= static_cast<int>(slots_.size())) {
    return false;
  }
  RECT dock{};
  if (GetWindowRect(hwnd_, &dock) == FALSE) {
    return false;
  }
  const int icon_px = Dip(kIconDip);
  const float x = SlotIconX(static_cast<size_t>(index));
  POINT pt{static_cast<int>(x + static_cast<float>(icon_px) * 0.5f + 0.5f), 0};
  ClientToScreen(hwnd_, &pt);
  if (center != nullptr) {
    *center = pt;
  }
  if (dock_top != nullptr) {
    *dock_top = dock.top;
  }
  return true;
}
```

`ClientToScreen` 은 `const` 멤버에서도 부를 수 있습니다. `SlotIconX` 도 `const` 입니다.

`Dock::UpdateHoverLabel` 을 이 함수를 쓰도록 바꿉니다. **넘기는 값이 지금과 같아야 합니다.**

```cpp
void Dock::UpdateHoverLabel() {
  POINT center{};
  int dock_top = 0;
  if (!shown_ || dragging_ || fullscreen_occluded_ || popup_.IsOpen() || hover_ < 0 ||
      hover_ >= static_cast<int>(items_.size()) || !IconAnchor(hover_, &center, &dock_top)) {
    label_.Hide();
    return;
  }
  label_.Show(items_[static_cast<size_t>(hover_)].display_name, center, dock_top, dark_);
}
```

### 3.2 메뉴가 커서 대신 아이콘을 앵커로 쓰게 한다

`Dock::OpenDockMenu` 의 매개변수에서 커서 좌표를 뺍니다. 부르는 곳이 한 군데뿐입니다.

```cpp
  // src/dock.hpp
  void OpenDockMenu(int index);
```

```cpp
  // src/dock.cpp, WM_RBUTTONUP
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int index = HitTest(pt);
      if (index >= 0) {
        OpenDockMenu(index);
      } else if (popup_.IsOpen()) {
        popup_.Close();
      }
```

`Dock::OpenDockMenu` 안에서 앵커를 만듭니다. `popup_.Open` 을 부르기 직전, 지금 `screen` 을 쓰던 자리입니다.

```cpp
  POINT center{};
  int dock_top = 0;
  if (!IconAnchor(index, &center, &dock_top)) {
    Log(L"dock", L"menu anchor missing index=%d", index);
    return;
  }
  const POINT anchor{center.x, dock_top};
  popup_.SetDark(dark_);
  if (!popup_.Open(menu_content_.get(), anchor, PopupSurface::Anchor::AboveCenter)) {
```

### 3.3 가운데 정렬 모드를 만든다

`Anchor::AboveAt` 은 이 독 메뉴 한 곳에서만 씁니다. 상단바의 팝업은 모두 `BelowAt` 이나 `RightOf` 입니다. 그러므로 **이름을 `AboveCenter` 로 바꾸고 동작을 가운데 정렬로 바꿉니다.** 쓰지 않는 모드를 새로 늘리지 않기 위해서입니다.

`src/popup_surface.hpp`

```cpp
  // AboveCenter 는 앵커의 가로 중심에 메뉴의 가운데를 맞추고 앵커 위에 놓는다.
  enum class Anchor { AboveCenter, BelowAt, RightOf };
```

같은 파일의 멤버 기본값도 함께 고칩니다.

```cpp
  Anchor mode_ = Anchor::AboveCenter;
```

`src/popup_surface.cpp` 의 `Place`

```cpp
  } else if (mode == Anchor::AboveCenter) {
    x = anchor_screen.x - size.cx / 2;
    y = anchor_screen.y - size.cy - gap;
  } else {
    x = anchor_screen.x - inset;
    y = anchor_screen.y + gap;
  }
```

`inset` 은 `BelowAt` 에서 계속 쓰므로 남겨 둡니다. 아래쪽의 화면 경계 보정(`info.rcWork` 네 줄)은 그대로 둡니다.

## 4. 검증

**빌드는 CMake 로 하십시오.**

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

새 바이너리는 WMI 로 띄웁니다.

```powershell
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '"D:\repos\bamti\build\Release\bamti.exe"'
}
"rc=$($r.ReturnValue) pid=$($r.ProcessId)"
```

**화면에 클릭이나 키 입력을 합성하지 마십시오.** 아래 항목을 사용자에게 부탁하고 결과를 받으십시오.

1. 독 가운데쯤의 아이콘을 **왼쪽 끝에서** 우클릭하고, 다시 **오른쪽 끝에서** 우클릭합니다. 두 번 모두 메뉴가 **같은 자리**에 떠야 합니다.
2. 메뉴의 가로 중심이 아이콘의 가로 중심과 맞아야 합니다.
3. 독 맨 왼쪽 아이콘과 맨 오른쪽 아이콘에서도 메뉴가 화면 밖으로 잘리지 않아야 합니다.
4. 메뉴의 "옵션" 항목에 올렸을 때 하위 메뉴가 예전처럼 오른쪽에 붙어야 합니다.
5. 아이콘에 마우스를 올렸을 때 나오는 이름 라벨의 위치가 예전과 같아야 합니다. `IconAnchor` 로 빼면서 달라지면 안 됩니다.
6. 상단바의 시계, 제어 센터, 상태 아이콘 팝업이 예전과 같은 자리에 떠야 합니다. `Place` 를 건드렸으므로 함께 봅니다.

## 5. 하지 말 것

- 메뉴의 세로 위치나 4 DIP 간격을 바꾸지 마십시오.
- `BelowAt` 과 `RightOf` 의 계산을 바꾸지 마십시오. 상단바 팝업이 전부 여기에 걸려 있습니다.
- 꼬리 모양을 이번에 그리지 마십시오. 다음 지시서에서 합니다.
- `MeasureMenuRows` 나 메뉴 행 치수를 바꾸지 마십시오.
- 드래그, 호버, 클릭 처리(`WM_LBUTTONUP`, `WM_MOUSEMOVE`)를 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
