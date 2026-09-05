# 작업 지시서: 상단바를 세그먼트 모델로 재구성

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`PLAN-TRAY-TO-TOPBAR.md`의 2단계를 구현하는 지시서입니다.

이 작업은 **화면에 보이는 결과를 바꾸지 않습니다.** 시작 버튼, 경고 문구, 상태 항목, 시계가 지금과 같은 자리에 같은 모양으로 나와야 합니다. 바뀌는 것은 내부 구조와 다시 그리는 범위입니다. 앞으로 트레이 아이콘과 내장 위젯과 사용량 항목이 이 바에 얹히는데, 지금 구조로는 그것들을 올릴 자리가 없습니다.

예외가 하나 있습니다. 폭이 부족해 표시되지 못한 항목을 알려 주는 갈매기 표시가 새로 생깁니다. 지금은 그런 항목이 아무 흔적 없이 사라집니다.

---

## 0. 완료 조건

- 상태 항목이 화면에 들어가는 평범한 상황에서 바의 모양이 작업 전과 같습니다.
- 시계만 바뀔 때 시계 영역만 다시 그려집니다. 지금은 바 전체를 다시 그립니다.
- 상태 항목이 초당 수십 번 갱신되어도 프레임은 16ms에 한 번만 그려집니다.
- 폭이 부족하면 항목이 갈매기 표시 안으로 접히고, 눌러서 볼 수 있습니다.
- `/W4` 경고 없이 Debug와 Release 모두 빌드됩니다.

---

## 1. 지금 코드의 문제

고칠 대상을 정확히 짚습니다. 아래는 추측이 아니라 코드를 읽고 확인한 사실입니다.

| 문제 | 위치 |
|---|---|
| 레이아웃과 그리기가 한 함수에 섞여 있어, 히트 사각형이 그리기의 부산물로 만들어집니다 | `clock_renderer.cpp` `Draw()` |
| 시계 텍스트 레이아웃을 한 프레임에 **두 번** 만듭니다. 그리기용으로 한 번, 항목 배치의 시작점을 다시 구하려고 또 한 번 만듭니다 | `Draw()` 안의 `MakeLayout(clock, ...)` 두 곳 |
| 상태 항목마다 `IDWriteTextLayout`을 프레임마다 새로 만듭니다. 캐시가 없습니다 | `Draw()`의 `Fitted` 루프 |
| 폭이 부족한 항목을 `continue`로 조용히 버립니다. 사용자는 항목이 있는지조차 모릅니다 | `Draw()`의 `if (left < left_limit) continue;` |
| 상태가 바뀔 때마다 바 전체를 무효화하고, 합치기가 없습니다 | `menu_bar.cpp:482` `kStatusChangedMsg` |
| **`Paint()`가 `ps.rcPaint`를 무시하고 항상 전체 클라이언트로 버퍼를 잡습니다.** 그래서 시계만 무효화해도 결국 바 전체를 다시 그립니다 | `menu_bar.cpp` `Paint()` |

마지막 항목이 특히 중요합니다. `WM_TIMER`에서 `clock_rect_`만 무효화하는 코드가 이미 있는데, `Paint()`가 그 노력을 그대로 버리고 있습니다. **부분 무효화는 무효화하는 쪽과 그리는 쪽이 함께 지켜야 성립합니다.**

---

## 2. 범위 조정: 아이콘 캐시는 3단계로 미룹니다

계획 문서 2-1절은 `icon_cache`를 이 단계에서 만들라고 적었습니다. 그 판단을 바꿉니다.

지금 바에는 비트맵 아이콘을 쓰는 곳이 하나도 없습니다. 상태 항목은 유니코드 글리프만 그리고, 트레이 아이콘과 위젯 아이콘은 3단계와 5단계에 들어옵니다. 소비자가 없는 상태에서 `dock.cpp`의 아이콘 코드를 공용 모듈로 뽑으면, 실제 필요와 어긋난 인터페이스가 나올 가능성이 큽니다.

따라서 **이 단계에서는 `icon_cache`를 만들지 않습니다.** 3단계에서 프로토콜 v2의 `icon` 필드를 그릴 때, 첫 소비자의 요구를 보고 함께 뽑습니다. 계획 문서 2-1절과 7절 커밋 표도 그렇게 고칩니다(6-3절 참고).

참고로 3단계에서 추출할 대상은 `dock.cpp`의 `WicFactory()`, `BitmapFromIcon()`, 그리고 `RenderLayered()` 안의 `d2d_icons_` 캐시입니다. 지금은 건드리지 마십시오.

---

## 3. 새 구조

### 3-1. 새 파일

```
src/bar_layout.hpp / .cpp
```

`clock_renderer.{hpp,cpp}`는 남기되 역할을 줄입니다. 레이아웃 계산은 `BarLayout`이 하고, `ClockRenderer`는 계산된 결과를 받아 그리기만 합니다. 파일 이름을 바꾸지는 마십시오. 이름 변경은 이 작업의 목적이 아니며 diff만 키웁니다.

### 3-2. 자료 구조

```cpp
// src/bar_layout.hpp
namespace bamti {

enum class SegmentKind { kStart, kWarning, kStatus, kOverflow, kClock };

struct BarSegment {
  SegmentKind kind = SegmentKind::kStatus;
  std::string id;          // kStatus에서만 의미가 있다
  std::wstring text;       // 그릴 문자열. kStart는 비어 있다
  std::wstring tooltip;
  uint32_t accent = 0;
  RECT rect{};             // 클라이언트 좌표, 픽셀
};

struct BarLayoutResult {
  std::vector<BarSegment> segments;   // 왼쪽에서 오른쪽 순서로 정렬해 둔다
  std::vector<StatusItem> overflow;   // 폭이 부족해 접힌 항목
  UINT dpi = 96;
  RECT client{};
};

}  // namespace bamti
```

세그먼트는 **왼쪽에서 오른쪽 순서**로 담습니다. 계산은 지금처럼 오른쪽에서 왼쪽으로 하더라도, 결과 배열은 정렬해 두어야 이후 비교와 순회가 단순해집니다.

`BarSegment`가 `const StatusItem*`를 들고 있지 않게 하십시오. 계획 문서 초안에는 포인터가 있었지만, 스냅숏 수명과 얽히면 위험합니다. 필요한 값만 복사합니다. 항목 원본이 필요한 곳은 `id`로 다시 찾습니다.

### 3-3. BarLayout 클래스

측정에는 DirectWrite가 필요하므로 자유 함수가 아니라 클래스로 둡니다. 텍스트 레이아웃을 캐시해야 하기 때문이기도 합니다.

```cpp
class BarLayout {
 public:
  bool Initialize();                 // IDWriteFactory와 IDWriteTextFormat 생성
  void SetDpi(UINT dpi);             // 값이 바뀌면 포맷과 캐시를 버린다

  // 레이아웃을 계산한다. 그리기를 하지 않으며 렌더 타깃을 만지지 않는다.
  const BarLayoutResult& Compute(const RECT& client, const std::wstring& clock_text,
                                 const std::wstring& warning_text,
                                 const std::vector<StatusItem>& items);

  // 렌더러가 같은 프레임에서 재사용할 텍스트 레이아웃을 돌려준다.
  IDWriteTextLayout* LayoutFor(const std::wstring& text);

  const BarLayoutResult& last() const { return last_; }

 private:
  BarLayoutResult last_;
  // 텍스트 -> (IDWriteTextLayout, DWRITE_TEXT_METRICS) 캐시. 상한 64개, LRU로 버린다.
};
```

`ClockRenderer::MakeLayout`과 `EnsureTextFormat`은 `BarLayout`으로 옮깁니다. 글꼴 계열(`Segoe UI Variable` 다음에 `Segoe UI`), 크기(`kFontSizeDip = 13.0f`), 표 형태 숫자(`DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES`) 설정을 그대로 가져가십시오. 이 설정이 바뀌면 글자 폭이 달라져서 "모양이 같아야 한다"는 완료 조건이 깨집니다.

캐시는 문자열을 키로 씁니다. 시계 문자열은 1초마다 달라지므로 캐시에 남지 않고 흘러가지만, 그래도 초당 하나이므로 부담이 없습니다. 이득은 잘 바뀌지 않는 상태 항목 쪽에서 나옵니다. `SetDpi`에서 캐시를 통째로 비우십시오. DPI가 바뀌면 저장된 레이아웃의 폭이 전부 틀립니다.

### 3-4. 배치 규칙

지금 `Draw()`가 하는 계산을 그대로 옮기되, 두 번 계산하던 것을 한 번으로 줄입니다.

1. 시작 버튼을 왼쪽에 둡니다. 사각형은 지금과 같이 `kStartPadLeftDip`에서 시작해 폭 `kStartHitWidthDip`입니다.
2. 경고 문구(`taskbar_.warning()`)가 있으면 시작 버튼 오른쪽에 둡니다. 그 오른쪽 끝이 `left_limit`이 됩니다.
3. 시계를 오른쪽 끝에서 `kPadRightDip`만큼 안쪽에 둡니다. **시계 레이아웃은 한 번만 만들고 그 폭을 재사용합니다.**
4. 상태 항목을 시계 왼쪽에서부터 `priority` 내림차순으로, 같으면 `id` 오름차순으로 채웁니다. 정렬 기준은 지금 코드와 같습니다.
5. 어떤 항목의 왼쪽 끝이 `left_limit`보다 왼쪽으로 나가면, **그 항목과 그 뒤의 모든 항목을 `overflow`로 보냅니다.** 지금 코드처럼 그 항목만 건너뛰고 다음 항목을 시도하지 마십시오. 우선순위가 높은 항목이 밀려났는데 낮은 항목이 그 자리를 차지하면 순서가 뒤집혀 보입니다.
6. `overflow`가 비어 있지 않으면 갈매기 세그먼트를 하나 둡니다. 갈매기는 항목들보다 왼쪽에 놓습니다. 갈매기 자리를 확보하려고 항목 하나가 더 밀려날 수 있으므로, 5번과 6번을 한 번 더 반복해 수렴시킵니다. 반복은 최대 두 번이면 충분합니다.
7. 왼쪽 절반은 앞으로 앱 메뉴가 들어갈 자리이므로 상태 항목을 놓지 않습니다. 이는 `left_limit` 규칙으로 이미 지켜집니다.

갈매기 글리프는 `U+F0B0`(Segoe Fluent Icons의 ChevronLeft) 대신 일반 글꼴로 그릴 수 있는 문자를 쓰십시오. 상단바 글꼴이 Segoe UI 계열이므로 아이콘 글꼴을 새로 들이면 폴백 문제가 생깁니다. `···` 세 점 또는 `‹`로 충분합니다. 어느 쪽을 골랐는지 커밋 메시지에 적어 주십시오.

### 3-5. 렌더러

`ClockRenderer::Draw()`의 서명을 다음으로 바꿉니다.

```cpp
bool Draw(HDC hdc, const RECT& client, const RECT& dirty, bool dark,
          const BarLayoutResult& layout, BarLayout* text,
          bool start_hot, bool start_pressed);
```

- 인자로 받은 `layout`의 세그먼트를 순회하며 그립니다. 위치를 다시 계산하지 마십시오.
- `dirty`와 겹치지 않는 세그먼트는 건너뜁니다.
- 텍스트 레이아웃은 `text->LayoutFor(segment.text)`로 얻습니다.
- 히트 사각형을 만들지 않습니다. 그 정보는 이미 `layout`에 있습니다.

`MenuBar`의 `hits_`, `start_rect_`, `clock_rect_` 멤버는 지웁니다. `HitTest`, `HitStart`는 보관 중인 `BarLayoutResult`를 조회하도록 고칩니다.

---

## 4. 부분 무효화

### 4-1. Paint()가 rcPaint를 존중하게 만듭니다

이 절이 이 작업에서 가장 실수하기 쉬운 곳입니다. 좌표계를 두 번 옮기기 때문입니다.

```cpp
void MenuBar::Paint() {
  PAINTSTRUCT ps{};
  const HDC hdc = BeginPaint(hwnd_, &ps);
  RECT client{};
  GetClientRect(hwnd_, &client);

  RECT dirty = ps.rcPaint;
  if (IsRectEmpty(&dirty) == FALSE) {
    BP_PAINTPARAMS params{};
    params.cbSize = sizeof(params);
    params.dwFlags = BPPF_ERASE;
    HDC buffer_dc = nullptr;
    const HPAINTBUFFER buffer = BeginBufferedPaint(hdc, &dirty, BPBF_TOPDOWNDIB, &params, &buffer_dc);
    if (buffer != nullptr && buffer_dc != nullptr) {
      BufferedPaintClear(buffer, &dirty);
      clock_.Draw(buffer_dc, client, dirty, dark_, layout_.last(), &layout_,
                  start_hot_ || start_menu_.visible(), start_pressed_ || start_menu_.visible());
      EndBufferedPaint(buffer, TRUE);
    }
  }
  EndPaint(hwnd_, &ps);
}
```

렌더러 안에서는 다음을 지킵니다.

```cpp
// 버퍼 DC는 원본 DC와 같은 좌표계를 쓴다. 따라서 dirty로 바인딩한다.
hr = rt_->BindDC(hdc, &dirty);

// D2D는 바인딩한 사각형의 왼쪽 위를 원점으로 삼는다.
// 세그먼트 좌표는 클라이언트 기준이므로, 그 차이만큼 평행이동해 좌표계를 되돌린다.
const float px = static_cast<float>(dpi_) / 96.0f;
rt_->SetTransform(D2D1::Matrix3x2F::Translation(
    -static_cast<float>(dirty.left - client.left) / px,
    -static_cast<float>(dirty.top - client.top) / px));
```

이렇게 하면 그리기 코드는 항상 클라이언트 좌표로 말하고, `dirty` 바깥은 D2D가 잘라 냅니다. 세그먼트별로 좌표를 빼는 계산을 곳곳에 흩지 마십시오.

`EndDraw()` 전에 `SetTransform`을 항등 행렬로 되돌릴 필요는 없습니다. 다음 프레임에서 다시 설정하기 때문입니다. 다만 `D2DERR_RECREATE_TARGET`으로 타깃을 버릴 때는 지금처럼 `rt_.Reset()`을 유지하십시오.

**검증 방법**: 시계만 무효화된 프레임에서 시작 버튼과 상태 항목이 지워지지 않고 남아 있어야 합니다. 좌표 변환이 틀리면 시계가 엉뚱한 위치에 그려지거나 화면이 밀려 보입니다.

### 4-2. 무엇이 바뀌었는지 비교합니다

`MenuBar`가 직전 `BarLayoutResult`를 보관하고, 새로 계산한 결과와 비교해 무효화 범위를 정합니다.

```cpp
void MenuBar::RefreshLayout() {
  BarLayoutResult before = layout_.last();          // 복사
  RECT client{};
  GetClientRect(hwnd_, &client);
  const BarLayoutResult& after =
      layout_.Compute(client, clock_.CurrentTimeText(), taskbar_.warning(), status_.Snapshot());

  if (before.segments.size() != after.segments.size() || before.dpi != after.dpi ||
      !EqualRect(&before.client, &after.client)) {
    InvalidateRect(hwnd_, nullptr, FALSE);
    return;
  }
  for (size_t i = 0; i < after.segments.size(); ++i) {
    const BarSegment& a = before.segments[i];
    const BarSegment& b = after.segments[i];
    if (a.kind != b.kind || a.id != b.id || !EqualRect(&a.rect, &b.rect)) {
      InvalidateRect(hwnd_, nullptr, FALSE);   // 배치가 흔들렸으면 전체를 다시 그린다
      return;
    }
    if (a.text != b.text || a.accent != b.accent) {
      InvalidateRect(hwnd_, &b.rect, FALSE);
    }
  }
}
```

판단 기준은 단순합니다. **자리가 바뀌면 전체를 그리고, 내용만 바뀌면 그 자리만 그립니다.** 자리가 바뀐 경우를 정교하게 나누려 들지 마십시오. 흔한 경우가 아니고, 잘못 계산하면 화면에 찌꺼기가 남습니다.

`WM_TIMER`의 시계 갱신도 이 함수를 거치게 통일합니다. 시계 문자열이 그대로면 `Compute` 결과의 텍스트도 그대로이므로 아무것도 무효화되지 않습니다. 지금의 `last_clock_text_` 비교는 지웁니다.

### 4-3. 다시 그리기를 합칩니다

`kStatusChangedMsg`가 올 때마다 계산하면, 공급자 다섯이 동시에 값을 밀어 넣을 때 계산도 다섯 번 합니다.

```cpp
constexpr UINT_PTR kRepaintTimerId = 2;
constexpr UINT kRepaintCoalesceMs = 16;

case kStatusChangedMsg:
  NotePostedStorm(L"status", g_status_msg_count, g_status_msg_window);
  if (!repaint_armed_) {
    repaint_armed_ = true;
    SetTimer(hwnd_, kRepaintTimerId, kRepaintCoalesceMs, nullptr);
  }
  return 0;

case WM_TIMER:
  if (wparam == kRepaintTimerId) {
    KillTimer(hwnd_, kRepaintTimerId);
    repaint_armed_ = false;
    RefreshLayout();
    return 0;
  }
  ...
```

`WM_DESTROY`에서 이 타이머도 `KillTimer` 하십시오. 지금 `kClockTimerId`를 정리하는 두 곳에 나란히 넣으면 됩니다.

`WM_DPICHANGED`, `WM_DISPLAYCHANGE`, `WM_SETTINGCHANGE`에서는 합치지 말고 즉시 `layout_.SetDpi(...)`와 전체 무효화를 수행합니다. 드문 사건이고 즉시 반영되어야 합니다.

---

## 5. 오버플로 팝업

갈매기가 눌리지 않으면 사용자는 접힌 항목에 닿을 수 없습니다. 그래서 이 단계에서 최소한의 팝업까지 만듭니다.

`src/status_panel.cpp` 같은 새 파일을 만들지 말고, 지금 `menu_bar.cpp`의 익명 이름공간에 `StatusPanelContent`가 있는 자리 옆에 `OverflowContent : public PopupContent`를 둡니다. 파일 분리는 3단계에서 함께 합니다.

```cpp
class OverflowContent : public PopupContent {
 public:
  void Reset(MenuBar* owner, std::vector<StatusItem> items);
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;
  int RowCount() const override;
};
```

- 행 하나는 항목 하나입니다. 왼쪽에 `icon_glyph`와 `text`를 이어 붙여 그리고, 오른쪽 여백은 두지 않습니다.
- `Invoke(index)`는 그 항목의 패널을 엽니다. `MenuBar::OpenStatusPanel`을 재사용하십시오. `PopupSurface::Dismiss`가 팝업을 먼저 닫은 뒤 `Invoke`를 부르므로, 그 안에서 다른 팝업을 여는 것은 안전합니다.
- 해당 항목에 패널이 없으면 `SendClick(id, "left")`만 보내고 아무 창도 열지 않습니다. 지금 바에서 항목을 클릭했을 때와 같은 동작입니다.
- 갈매기는 `Anchor::BelowAt`으로 엽니다. 앵커는 갈매기 세그먼트 사각형의 왼쪽 아래입니다.

`status_popup_`을 그대로 재사용하십시오. `PopupSurface`를 새로 하나 더 만들지 마십시오. 오버플로 팝업과 상태 패널이 동시에 떠 있을 이유가 없습니다.

---

## 6. 성능 측정과 문서 갱신

### 6-1. 측정값을 로그로 남깁니다

계획 문서 5절의 예산을 지켰는지 확인하려면 숫자가 필요합니다. `Log(L"perf", ...)`로 다음을 남기되, **매 프레임 남기지 말고 100프레임마다 한 번** 남깁니다. 로그가 그리기보다 비싸지면 안 됩니다.

```
perf bar full=0.9ms seg=0.2ms compute=0.1ms segments=7 overflow=0
```

- `full`: `dirty`가 클라이언트 전체였던 프레임의 `Draw` 소요 시간
- `seg`: 부분 무효화 프레임의 `Draw` 소요 시간
- `compute`: `BarLayout::Compute` 소요 시간

시간은 `QueryPerformanceCounter`로 재십시오. 이 저장소에 이미 정밀 시계를 쓰는 선례가 있습니다(`0512eae` 커밋).

목표는 계획 문서 5절과 같습니다. 전체 다시 그리기 2ms 미만, 세그먼트 하나 0.3ms 미만입니다. 넘으면 그 사실을 보고하고 원인을 적어 주십시오. 목표를 맞추려고 측정을 손보지 마십시오.

### 6-2. 눈으로 확인합니다

작업 전 상태의 상단바를 스크린숏으로 남기고, 작업 후와 비교하십시오. 다음 네 조합을 각각 확인합니다.

- 다크 테마 / 라이트 테마
- 100% DPI / 150% DPI 이상

글자 위치가 1픽셀이라도 다르면 3-3절의 글꼴 설정이 옮겨지지 않은 것입니다.

### 6-3. 계획 문서를 고칩니다

- 2-1절의 새 파일 목록에서 `icon_cache`를 빼고, 3단계로 옮긴다는 문장을 넣습니다. 사유는 이 지시서 2절과 같습니다.
- 3단계 설명에 `icon_cache` 추출을 추가합니다. 추출 대상은 `dock.cpp`의 `WicFactory()`, `BitmapFromIcon()`, `d2d_icons_` 캐시입니다.
- 7절 커밋 표의 3번 줄(아이콘 캐시 추출)을 4번 뒤로 옮깁니다.

---

## 7. 하지 말아야 할 것

1. **모양을 바꾸지 않습니다.** 간격 상수, 글꼴, 색을 이 기회에 다듬지 마십시오. 그것은 별도 작업입니다.
2. **파일 이름을 바꾸지 않습니다.** `clock_renderer`가 이제 시계만 그리지 않는다는 것은 압니다. 이름 정리는 3단계에서 파일을 나눌 때 함께 합니다.
3. **아이콘 캐시를 만들지 않습니다.** 2절의 이유입니다.
4. **`PopupSurface`를 새로 만들지 않습니다.**
5. **세그먼트에 포인터를 담지 않습니다.** 3-2절의 이유입니다.
6. **자리가 바뀐 프레임을 부분 무효화로 처리하지 않습니다.** 4-2절의 이유입니다.
7. **`BarLayout::Compute` 안에서 렌더 타깃을 만지지 않습니다.** 계산은 그리기와 완전히 분리되어야 합니다. 그래야 나중에 그리기 전에 히트 테스트를 할 수 있습니다.

---

## 8. 검증

**빌드**
- [ ] `/W4` 경고 없이 Debug와 Release가 빌드됩니다.

**모양**
- [ ] 다크와 라이트, 100%와 150% DPI에서 작업 전과 같은 모양입니다.
- [ ] 시작 버튼 호버와 눌림 표시가 그대로 동작합니다.
- [ ] 태스크바 숨김에 실패했을 때의 경고 문구가 같은 자리에 나옵니다.
- [ ] 상태 항목 툴팁이 그대로 뜹니다.

**부분 무효화**
- [ ] 시계만 바뀐 프레임에서 `ps.rcPaint`가 시계 영역으로 한정됩니다. 로그로 확인합니다.
- [ ] 그 프레임에서 시작 버튼과 상태 항목이 지워지지 않습니다.
- [ ] 상태 항목 하나만 바뀌면 그 항목의 사각형만 무효화됩니다.
- [ ] 항목이 추가되거나 사라져 자리가 밀리면 바 전체가 다시 그려집니다.

**합치기**
- [ ] `examples/status_push.py`로 초당 200회 갱신을 보내도 그리기가 초당 62회를 넘지 않습니다.
- [ ] 그 상태에서 CPU 사용률이 1%를 넘지 않습니다.

**오버플로**
- [ ] 항목을 많이 띄워 폭을 넘기면 갈매기가 나타나고, 접힌 항목이 우선순위 순서대로 팝업에 나옵니다.
- [ ] 팝업에서 항목을 고르면 그 항목의 패널이 열립니다.
- [ ] 폭이 다시 넉넉해지면 갈매기가 사라집니다.

**회귀**
- [ ] `Win+Space` Spotlight, 시작 메뉴, 상태 패널이 그대로 열립니다.
- [ ] 전체화면 게임에서 바가 즉시 사라지고 즉시 복귀합니다.
- [ ] 모니터 DPI를 바꿔도 레이아웃이 복구됩니다.
- [ ] 100회 여닫은 뒤 GDI 객체 수와 USER 객체 수가 늘지 않습니다.

---

## 9. 커밋

| 순서 | 작업 | 커밋 메시지 |
|---|---|---|
| 1 | `BarLayout` 신설, `Draw()`에서 레이아웃 분리 | `refactor: 상단바 레이아웃을 세그먼트 모델로 분리한다` |
| 2 | `Paint()`의 `rcPaint` 존중, 차이 비교 무효화, 합치기 타이머 | `perf: 상단바를 바뀐 세그먼트만 다시 그린다` |
| 3 | 오버플로 세그먼트와 팝업 | `feat: 폭이 부족한 상태 항목을 오버플로 팝업에 모은다` |
| 4 | 계획 문서 갱신 | `docs: 아이콘 캐시 추출을 3단계로 옮긴다` |

1번과 2번 사이에 반드시 빌드하고 눈으로 확인하십시오. 1번만 끝난 상태에서도 바가 지금과 똑같이 보여야 합니다. 그 확인 없이 2번으로 넘어가면, 모양이 깨졌을 때 원인이 레이아웃 분리인지 좌표 변환인지 가릴 수 없습니다.
