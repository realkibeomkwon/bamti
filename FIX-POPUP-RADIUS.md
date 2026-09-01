# 수정 지시서: 팝업 모서리 곡률을 독과 같게 맞춘다

`FIX-SEARCH-ICON.md` 다음에 하십시오. **`TASK-CONTROL-CENTER-UI.md`보다 먼저 해야 합니다.** 제어 센터 패널의 배경을 이 지시서가 바꾸기 때문입니다.

건드리는 파일은 `src/popup_surface.cpp`, `src/popup_surface.hpp`입니다.

---

## 1. 결함

독의 알약과 독 아이콘 우클릭 메뉴의 모서리 곡률이 다릅니다.

- 독: `src/dock.cpp`의 `kCornerRadiusDip = 20`. Direct2D로 직접 그립니다. 주석에 이유가 적혀 있습니다. "macOS Dock is ~20pt at the default bar height (~64pt). DWM ROUND/ROUNDSMALL cannot express that."
- 메뉴와 패널: `src/popup_surface.cpp`의 `PopupSurface::ApplyChrome`이 `dwm::kCornerRoundSmall`을 씁니다. DWM이 창을 대신 깎아 주며 반지름은 약 4px로 고정입니다.

메뉴는 독에서 자라나는 부속인데 곡률이 5배 차이가 나서 서로 다른 물건처럼 보입니다.

## 2. 어느 쪽으로 통일할 것인가 — 독의 20 DIP를 권합니다

두 가지 선택지를 견주면 이렇습니다.

| | 독을 4px로 낮추기 | 팝업을 20 DIP로 올리기 |
|---|---|---|
| 작업량 | 상수 한 줄 | 팝업 창을 레이어드 창으로 바꿔야 함 |
| 위험 | 없음 | 팝업 전체(독 메뉴, 상태 패널, 제어 센터)에 걸침 |
| 결과 | 독이 각져서 macOS 느낌이 사라짐 | 메뉴가 독과 한 덩어리로 읽힘 |
| 덤 | 없음 | `DockFillColor`의 알파(0.78/0.82)가 살아나 독과 같은 반투명이 됨 |

**독의 20 DIP로 올리는 쪽을 고르십시오.** 독은 항상 보이는 주 표면이고 이 제품의 인상을 결정합니다. 부속인 메뉴가 주 표면을 따라가는 것이 자연스럽습니다. 지금 팝업이 불투명한 것도 DWM에 곡률을 맡긴 결과인데, 이 작업을 하면 독과 같은 반투명까지 함께 얻습니다.

다만 좁은 메뉴에서 20이 과하게 보일 수 있으므로 **상한을 두십시오.**

```cpp
const float radius = (std::min)(static_cast<float>(DipToPx(20, dpi)),
                                (std::min)(width, height) * 0.5f - 1.0f);
```

곡률 값은 독과 팝업이 같은 상수를 보게 하십시오. `kCornerRadiusDip`을 공용 헤더로 옮기거나, 팝업 쪽에 같은 값을 두고 서로 주석으로 가리키게 하십시오. 두 값이 갈라지면 이 결함이 되살아납니다.

## 3. 수정

### 3-1. 창을 레이어드로 바꾼다

`PopupSurface::Create`에서 창 스타일에 `WS_EX_LAYERED`를 더합니다.

```cpp
  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED,
                          kPopupClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr, instance, this);
```

창 클래스의 배경 브러시를 지우십시오. 지금 `GetStockObject(BLACK_BRUSH)`가 들어가 있어서 그대로 두면 검은 사각형이 한 번 번쩍입니다.

```cpp
  wc.hbrBackground = nullptr;
```

`ApplyChrome`의 곡률을 `dwm::kCornerDoNotRound`로 바꾸십시오. 이제 우리가 직접 깎으므로 DWM이 한 번 더 깎으면 안 됩니다.

### 3-2. 렌더 타깃을 DIB 위의 DC 렌더 타깃으로 바꾼다

`ID2D1HwndRenderTarget`은 알파를 창 밖으로 내보내지 못합니다. **`src/dock.cpp`의 `Dock::EnsureLayeredTarget`과 `Dock::Present`를 그대로 본떠 옮기십시오.** 검증된 코드가 같은 저장소 안에 있습니다.

- `CreateDIBSection`으로 `width × height`, 32비트, 위에서 아래로(`biHeight = -height`) 만듭니다.
- `CreateCompatibleDC` + `SelectObject`로 메모리 DC에 겁니다.
- `d2d_->CreateDCRenderTarget`에 `DXGI_FORMAT_B8G8R8A8_UNORM` + `D2D1_ALPHA_MODE_PREMULTIPLIED`를 줍니다.
- 그리기 직전에 `BindDC(mem_dc, &rect)`를 부릅니다. DC 렌더 타깃은 매 프레임 묶어야 합니다.
- 크기가 바뀌면 DIB부터 다시 만듭니다. `Resize`는 없습니다.

`Create`에서 창이 숨어 있는 동안 렌더 타깃을 미리 만들어 두는 예열 코드가 있습니다(8×8로 만들어 둡니다). 그 의도를 그대로 지키십시오. 첫 메뉴가 느려지지 않게 하려는 장치입니다.

### 3-3. 배경을 직접 그린다

`PopupSurface::Render`가 지금 알파를 1.0으로 눌러 불투명 사각형을 칠하고 있습니다.

```cpp
  target_->Clear(D2D1::ColorF(fill.r, fill.g, fill.b, 1.0f));
  ...
  target_->FillRectangle(..., fill_.Get());
```

이것을 다음으로 바꾸십시오.

1. `Clear(D2D1::ColorF(0, 0, 0, 0))` — 완전히 투명하게 비웁니다.
2. `FillRoundedRectangle`로 `DockFillColor(dark_)`를 **원래 알파 그대로** 칠합니다. 억지로 1.0으로 만들지 마십시오.
3. `DrawRoundedRectangle`로 `DockStrokeColor(dark_)` 테두리를 1px 그립니다. 독이 같은 테두리를 그리고 있습니다(`src/dock.cpp`의 알약 그리기 참고). 사각형은 `0.5f` 안쪽으로 넣어야 선이 반 픽셀로 잘리지 않습니다.
4. 그다음에 `content_->Render(...)`를 부릅니다.

미리 곱해진 알파를 쓰므로, 반투명 배경 위에 내용을 그릴 때 색이 어긋나 보이면 브러시 색의 알파 처리를 다시 보십시오.

### 3-4. 화면에 올리는 경로를 바꾼다

지금은 모든 갱신이 `InvalidateRect` + `UpdateWindow`로 `WM_PAINT`를 부르고, 거기서 `Render()`가 돕니다. 레이어드 창은 `UpdateLayeredWindow`로 직접 올려야 하고 `WM_PAINT`가 온다는 보장이 없습니다.

`Present()`를 하나 만드십시오. `Render()`로 DIB에 그린 뒤 곧바로 `UpdateLayeredWindow`를 부릅니다.

```cpp
  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT src{0, 0};
  SIZE size{width, height};
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, mem_dc, &src, 0, &blend, ULW_ALPHA);
```

`src/popup_surface.cpp` 안의 `InvalidateRect(hwnd_, nullptr, FALSE)` + `UpdateWindow(hwnd_)` 짝을 전부 `Present()` 호출로 바꾸십시오. 열두 군데쯤 됩니다. `WM_PAINT` 처리기는 `Present()`를 부르도록 남겨 두어도 무해합니다.

`Open`에서 창을 먼저 보이고 그린다는 주석("Hidden windows do not receive WM_PAINT from UpdateWindow, so show first")이 있습니다. 레이어드 창에서는 `UpdateLayeredWindow`를 먼저 부르고 `ShowWindow(SW_SHOWNA)`를 나중에 부르는 편이 첫 프레임의 깜빡임이 없습니다. 순서를 바꾸고 결과를 확인하십시오.

### 3-5. 알파와 마우스 입력

`UpdateLayeredWindow`로 만든 창은 **알파가 0인 화소로 들어온 클릭을 아래 창으로 흘려보냅니다.** 깎인 모서리 바깥을 눌렀을 때 메뉴가 먹지 않고 아래 창이 받게 되므로, 이것은 개선입니다. 다만 바깥 클릭으로 팝업을 닫는 경로(`PopupSurface`의 닫힘 감시)가 이 변화로 깨지지 않는지 확인하십시오. 모서리 바깥을 눌렀을 때도 팝업은 닫혀야 합니다.

## 4. 검증

1. 독 아이콘을 우클릭한 메뉴의 모서리가 독 알약과 같은 곡률로 보여야 합니다. 나란히 놓고 화면을 찍어 비교하십시오.
2. 메뉴 배경이 독과 같은 정도로 비쳐야 합니다. 완전히 불투명하면 3-3의 알파를 여전히 누르고 있는 것입니다.
3. 상태 항목 패널, 트레이 아이콘 메뉴, 제어 센터 패널을 모두 열어 보십시오. **같은 표면을 쓰므로 전부 바뀝니다.** 하나라도 검은 사각형이 번쩍이거나 모서리가 각지면 안 됩니다.
4. 메뉴 항목이 하나뿐인 아주 작은 팝업을 열어 곡률 상한이 동작하는지 보십시오. 모서리가 서로 먹어 들어가 모양이 뭉개지면 안 됩니다.
5. 팝업의 첫 열림 지연을 로그로 확인하십시오. `popup` 태그에 `render target warm`과 `create render target`이 이미 찍히고 있습니다. **바꾸기 전 값과 바꾼 뒤 값을 함께 보고하십시오.**
6. 메뉴 항목 위로 마우스를 옮길 때 강조가 즉시 따라와야 합니다. 늦으면 3-4의 `Present()` 교체가 빠진 자리가 있는 것입니다.
7. 100%, 150%, 200% 배율에서 각각 확인하십시오.
8. 메뉴 바깥을 눌러 닫히는 동작, 드래그로 슬라이더를 움직이는 동작이 그대로인지 확인하십시오.

## 5. 보고할 것

- 독과 메뉴를 함께 찍은 화면.
- 첫 열림 지연의 이전/이후 측정값.
- `Present()`로 바꾼 자리의 개수와, `WM_PAINT`가 여전히 오는지 여부.
