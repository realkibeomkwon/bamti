# 작업 지시서: 독 호버 라벨에도 꼬리를 붙인다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

**`FIX-POPUP-TAIL.md` 를 끝낸 뒤에 하십시오.** 거기에서 만든 `corner::BuildCallout` 과 `corner::CalloutCache` 를 그대로 씁니다. 새로 만들 도형 코드는 없습니다.

건드리는 파일은 `src/dock_label.cpp` 와 `src/dock_label.hpp` 두 개입니다.

---

## 1. 무엇을 만드는가

독 아이콘에 마우스를 올리면 나오는 이름 라벨에도, 우클릭 메뉴와 같은 꼬리를 붙입니다. 꼭짓점은 아이콘의 가로 중심을 가리킵니다.

라벨은 알약(높이 26 DIP)이라 메뉴 카드보다 훨씬 낮습니다. 메뉴의 꼬리(밑변 30, 높이 12)를 그대로 쓰면 알약에 비해 큽니다. **같은 2.5 대 1 비율을 유지하면서 밑변 20 DIP, 높이 8 DIP 로 줄입니다.**

## 2. 고칠 것

### 2.1 치수와 상태를 더한다

`src/dock_label.cpp` 의 익명 이름공간에 넣습니다.

```cpp
// 메뉴 꼬리와 같은 2.5 대 1 비율이되, 알약 높이 26 DIP 에 맞게 줄인 값이다.
constexpr int kLabelTailBaseDip = 20;
constexpr int kLabelTailHeightDip = 8;
```

`src/dock_label.hpp` 의 비공개부에 넣습니다. `corner.hpp` 는 `dock_label.cpp` 가 이미 포함하고 있으므로, 헤더에도 `#include "corner.hpp"` 를 더하십시오.

```cpp
  int body_px_ = 0;    // 알약의 높이. 창 높이에서 꼬리를 뺀 값이다.
  int tail_px_ = 0;
  float apex_px_ = 0.0f;  // 꼭짓점 x. 창의 클라이언트 좌표계다.
  corner::CalloutCache callout_;
```

### 2.2 창 높이에 꼬리 자리를 더한다

`DockLabel::Show` 의 크기와 위치 계산을 고칩니다. **가로 계산과 화면 경계 보정은 그대로 둡니다.**

```cpp
  const UINT dpi = Dpi();
  const int pad_x = DipToPx(kLabelPadXDip, dpi);
  const int body = DipToPx(kLabelHeightDip, dpi);
  const int tail = DipToPx(kLabelTailHeightDip, dpi);
  const int tail_base = DipToPx(kLabelTailBaseDip, dpi);
  const int gap = DipToPx(8, dpi);
  const int text_w = static_cast<int>(std::ceil(PopupTextWidth(dpi, text_)));
  int width = (std::max)(body, text_w + pad_x * 2);
  // 알약은 양 끝이 반원이라 밑변이 놓일 곳이 좁다. 짧은 이름에서 꼬리가
  // 조각으로 줄지 않게 최소 너비를 준다.
  width = (std::max)(width, tail_base + body + DipToPx(4, dpi));
  const int height = body + tail;
  if (width <= 0 || height <= 0) {
    Hide();
    return;
  }

  int x = icon_center_screen.x - width / 2;
  const int y = dock_top - gap - height;
```

화면 경계로 `x` 를 밀어 넣는 기존 코드 **뒤에** 꼭짓점을 기억합니다.

```cpp
  body_px_ = body;
  tail_px_ = tail;
  apex_px_ = static_cast<float>(icon_center_screen.x - x);
```

`SetWindowPos` 와 `Present` 호출은 그대로입니다. 창이 8 DIP 만큼 높아지고 그만큼 위로 올라가므로, **꼬리 끝이 예전에 알약의 아래 변이 있던 자리**에 옵니다.

### 2.3 알약과 꼬리를 한 도형으로 그린다

`DockLabel::Present` 에서 알약을 그리는 부분을 고칩니다.

```cpp
  const float width = static_cast<float>(dib_w_);
  const float height = static_cast<float>(dib_h_);
  const float body = height - static_cast<float>(tail_px_);
  const float radius = corner::PillPx(body);
  const D2D1_ROUNDED_RECT pill{D2D1::RectF(0.5f, 0.5f, width - 0.5f, body - 0.5f), radius, radius};
```

**높이가 아니라 알약 높이로 반지름을 잡는 것이 중요합니다.** 꼬리까지 반원으로 말리면 안 됩니다.

칠과 테두리는 도형 하나로 그립니다.

```cpp
  corner::Tail tail{};
  tail.apex_x = apex_px_;
  tail.base_px = static_cast<float>(DipToPx(kLabelTailBaseDip, dpi));
  tail.height_px = static_cast<float>(tail_px_);
  tail.tip_px = corner::ToPx(corner::kTailTipDip, dpi);
  ID2D1PathGeometry* shape = tail_px_ > 0 ? callout_.Get(d2d_.Get(), pill.rect, radius, tail) : nullptr;
  if (fill) {
    if (shape != nullptr) {
      target_->FillGeometry(shape, fill.Get());
    } else {
      target_->FillRoundedRectangle(pill, fill.Get());
    }
  }
  if (stroke) {
    if (shape != nullptr) {
      target_->DrawGeometry(shape, stroke.Get(), 1.0f);
    } else {
      target_->DrawRoundedRectangle(pill, stroke.Get(), 1.0f);
    }
  }
```

글자는 알약 안에만 놓입니다. 지금은 창 전체 높이를 넘기고 있으므로 **`height` 를 `body` 로 바꾸십시오.**

```cpp
    DrawPopupText(target_.Get(), dpi, text_, D2D1::RectF(pad_x, 0.0f, width - pad_x, body), text.Get(),
                  DWRITE_TEXT_ALIGNMENT_CENTER);
```

`BindDC` 가 실패해서 렌더 타깃을 버리는 자리에 `callout_.Reset();` 을 함께 넣으십시오. `ReleaseLayeredTarget` 에도 같은 줄을 넣습니다.

## 3. 검증

**빌드는 CMake 로 하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B D:\repos\bamti\build
cmake --build D:\repos\bamti\build --config Release
```

**bamti 는 새로 띄우기 전에 먼저 종료시켜야 합니다.** 단일 인스턴스이지만 뒤에 뜬 쪽이 양보하는 방식이라, 그냥 띄우면 새 프로세스가 스스로 끝나고 예전 바이너리가 계속 돌아갑니다. WMI 가 돌려주는 새 pid 는 재시작의 증거가 되지 못합니다.

상단바 창(클래스 `bamti.MenuBar`)에 `WM_COMMAND` 로 명령 1번을 보내면 `DestroyWindow` 를 거쳐 작업 표시줄과 작업 영역까지 되돌린 뒤 끝납니다.

```powershell
Add-Type -TypeDefinition @'
using System; using System.Text; using System.Runtime.InteropServices;
public static class Bar {
  public delegate bool EnumProc(IntPtr h, IntPtr p);
  [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr p);
  [DllImport("user32.dll", CharSet=CharSet.Unicode)] public static extern int GetClassNameW(IntPtr h, StringBuilder s, int n);
  [DllImport("user32.dll")] public static extern bool PostMessageW(IntPtr h, uint m, IntPtr w, IntPtr l);
  public static IntPtr Find() {
    IntPtr f = IntPtr.Zero;
    EnumWindows((h, p) => { var sb = new StringBuilder(256); GetClassNameW(h, sb, 256);
      if (sb.ToString() == "bamti.MenuBar") { f = h; return false; } return true; }, IntPtr.Zero);
    return f;
  }
}
'@ -Language CSharp
$h = [Bar]::Find()
if ($h -ne [IntPtr]::Zero) { [void][Bar]::PostMessageW($h, 0x0111, [IntPtr]1, [IntPtr]0) }
$deadline = (Get-Date).AddSeconds(20)
while ((Get-Process bamti -EA SilentlyContinue) -and (Get-Date) -lt $deadline) { Start-Sleep -Milliseconds 500 }
"종료 후 남은 프로세스: " + $(if (Get-Process bamti -EA SilentlyContinue) { (Get-Process bamti).Id -join ',' } else { '없음' })
```

프로세스가 사라진 것을 확인한 뒤에 새 바이너리를 WMI 로 띄웁니다. 세션이 끝나도 살아 있어야 하므로 `Start-Process` 를 쓰지 마십시오.

```powershell
$r = Invoke-CimMethod -ClassName Win32_Process -MethodName Create -Arguments @{
  CommandLine = '"D:\repos\bamti\build\Release\bamti.exe"'
}
"rc=$($r.ReturnValue) pid=$($r.ProcessId)"
```

재시작이 실제로 일어났는지는 로그로 판정하십시오. `[host] start` 줄이 새로 늘어야 합니다.

```powershell
Select-String -Path "$env:USERPROFILE\.bamti\bamti.log" -Pattern '\[host\] start ' | Select-Object -Last 2
```

**화면에 클릭이나 키 입력을 합성하지 마십시오.** 아래를 사용자에게 부탁하고 결과를 받으십시오. 화면 캡처로 모양을 재는 것은 괜찮습니다.

1. 독 가운데쯤의 아이콘에 마우스를 올립니다. 라벨 아래에서 꼬리가 나와 아이콘을 가리켜야 합니다.
2. 알약의 테두리와 꼬리의 테두리가 한 줄로 이어져야 합니다.
3. 글자가 알약 안에서 세로 가운데에 있어야 합니다. 꼬리 쪽으로 내려가면 `DrawPopupText` 의 사각형을 `body` 로 바꾸지 않은 것입니다.
4. 이름이 아주 짧은 앱과 아주 긴 앱을 모두 확인합니다. 짧은 쪽에서 꼬리가 조각으로 줄지 않아야 하고, 긴 쪽에서 알약이 예전처럼 늘어나야 합니다.
5. 독 맨 왼쪽과 맨 오른쪽 아이콘에서, 라벨이 화면 안으로 밀려도 꼬리는 그 아이콘을 가리켜야 합니다.
6. 아이콘 사이를 빠르게 지나갈 때 라벨이 예전처럼 따라와야 합니다.

## 4. 하지 말 것

- `kLabelPadXDip`(16)과 `kLabelHeightDip`(26)을 바꾸지 마십시오. 알약 자체의 치수는 그대로입니다.
- 라벨의 가로 위치 계산과 화면 경계 보정을 바꾸지 마십시오.
- `corner::BuildCallout` 을 고치지 마십시오. 메뉴 꼬리가 같은 함수를 씁니다.
- 라벨에 그림자나 애니메이션을 새로 넣지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
