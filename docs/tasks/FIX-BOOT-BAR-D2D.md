# 작업 지시서 30: 상단바 첫 프레임의 D2D 초기화 비용을 옮긴다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-BOOT-COLD-START-2.md` 1절의 계측으로 원인이 확정되었으므로, 이번에는 실제로 고칩니다. 2026-09-03 13:48:45 재부팅에서 측정한 값을 근거로 삼습니다.

---

## 1. 확정된 사실

계측을 추가하고 재부팅한 뒤 얻은 값입니다.

```
2026-09-03 13:50:59.026 [perf] bar full[n=39 avg=100.4 max=3528.1]ms seg[n=61 avg=1.7 max=30.3]ms
                              compute[n=100 avg=0.6 max=12.7]ms segments=16 overflow=0 cold
2026-09-03 13:50:59.028 [perf] draw create=16.47/1647.0 bind=0.21/1.1 brush=0.00/0.0 begin=2.75/197.3
                              draw=4.37/265.2 end=16.37/1418.3 bpbegin=0.16/1.4 bpend=0.14/2.0
                              other=0.00/0.1 (ms, avg/max)
```

| 항목 | 이전(09:05) | 이번(13:50) |
|---|---|---|
| `other` 최댓값 | 5593.6ms | **0.1ms** |
| `create` 최댓값 | 측정하지 않음 | **1647.0ms** |
| `end` 최댓값 | 3662.5ms | 1418.3ms |
| `bar full` 최댓값 | 9694.4ms | 3528.1ms |

`other`가 0.1ms로 사라졌으므로 **느린 프레임의 정체는 렌더 타깃 생성 구간이 맞습니다.** `ClockRenderer::Draw` 안에 계측되지 않은 구간은 남아 있지 않습니다.

남은 비용은 두 덩어리입니다. `create`가 1647ms, `end`가 1418ms이고, 두 값의 합에 `begin` 197ms와 `draw` 265ms를 더하면 3527ms로 `bar full` 최댓값 3528.1ms와 일치합니다. **한 프레임이 전부를 차지합니다.**

평균값도 같은 결론을 가리킵니다. `create`의 평균 16.47ms에 100프레임을 곱하면 1647ms로 최댓값과 같습니다. `end`도 16.37 × 100 = 1637ms이므로 최댓값 1418ms에 근접합니다. 두 비용 모두 **한 프레임에만 몰려 있는 일회성 비용**입니다.

---

## 2. 왜 `end`까지 느린가

`create`가 느린 이유는 명확합니다. `CreateDCRenderTarget`이 `rt_`가 비어 있을 때만 도는 일회성 경로이기 때문입니다.

`end`가 함께 느린 이유는 Direct2D의 지연 초기화 성질에서 나옵니다. `CreateDCRenderTarget`은 렌더 타깃 객체만 만들고, 실제 래스터라이저 장치는 처음 그리기가 일어날 때 구성됩니다. 그래서 초기화 비용이 `create`와 `end`(`EndDraw`) 양쪽에 갈라져 나타납니다.

**이것은 코드를 읽어 세운 추정이고 아직 측정된 사실이 아닙니다.** 3절의 실험이 이 추정을 검증하는 역할을 겸합니다. 예열을 넣었을 때 `create`와 `end`가 **함께** 내려가면 추정이 맞고, `create`만 내려가고 `end`가 그대로면 `end`는 다른 원인을 갖고 있다는 뜻입니다.

---

## 3. 예열을 넣는다

### 3-1. 선례가 이미 있습니다

`src/popup_surface.cpp` 220~228행이 같은 문제를 같은 방법으로 해결해 두었습니다.

```cpp
  // Build the render target now, while the window is still hidden. Creating it
  // lazily inside the first present stalls the first menu by the full device
  // setup cost. A later Open() only rebuilds the DIB; the DC target stays.
  const ULONGLONG started = GetTickCount64();
  SetWindowPos(hwnd_, nullptr, 0, 0, 8, 8, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
  EnsureLayeredTarget();
  Log(L"popup", L"render target warm=%d %ums", target_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
```

이번 부팅에서도 이 예열이 두 번 돌았고 각각 140ms와 313ms를 기록했습니다.

```
13:49:14.373 [popup] create render target 8x8 ok=1 62ms
13:49:14.449 [popup] render target warm=1 140ms
13:49:14.612 [popup] create render target 8x8 ok=1 109ms
13:49:14.819 [popup] render target warm=1 313ms
```

**같은 방식을 상단바에 적용하십시오.** 새로운 구조를 만들지 말고 이 선례를 따르는 것이 이번 작업의 방침입니다.

### 3-2. `ClockRenderer`에 예열 함수를 만든다

`src/clock_renderer.hpp`의 public 영역에 함수를 하나 추가합니다.

```cpp
  // 첫 그리기 전에 렌더 타깃과 D2D 장치를 미리 구성한다.
  // 그리지 않으면 장치 구성이 EndDraw까지 미뤄지므로, 작은 메모리 DC에
  // 실제로 한 번 그려서 비용을 여기서 치른다.
  void WarmTarget();
```

`src/clock_renderer.cpp`에 구현합니다. 요구 사항은 넷입니다.

1. `d2d_`가 없으면 아무 일도 하지 말고 돌아갑니다.
2. `rt_`가 이미 있으면 아무 일도 하지 말고 돌아갑니다.
3. `Draw`의 535~545행과 **똑같은 속성으로** `CreateDCRenderTarget`을 부릅니다. 속성이 다르면 첫 그리기에서 다시 만들게 되어 예열이 무의미해집니다. 이 중복을 없애려면 그 블록을 private 헬퍼로 빼고 `Draw`와 `WarmTarget`이 함께 부르는 편이 낫습니다.
4. `CreateCompatibleDC(nullptr)`와 `CreateCompatibleBitmap`으로 8×8 메모리 DC를 만들어 `BindDC` → `BeginDraw` → 아주 작은 그리기 하나 → `EndDraw`를 수행합니다. 그린 결과는 버립니다. GDI 객체는 반드시 원래 것으로 되돌린 뒤(`SelectObject`) 해제하십시오.

그리기 하나는 `CreateSolidColorBrush` 한 번과 `FillRectangle` 한 번이면 충분합니다. 로고나 아이콘까지 그리게 만들지 마십시오. 목적은 장치 구성이지 캐시 채우기가 아닙니다.

로그는 `popup`의 형식을 그대로 따릅니다.

```cpp
  Log(L"bar", L"render target warm=%d %ums", rt_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
```

### 3-3. 호출 위치가 중요합니다

**`ClockRenderer::Initialize` 안에서 부르지 마십시오.** `SetDpi`(315행)가 DPI가 바뀌면 `DropTarget()`을 부르고, `DropTarget()`은 `rt_.Reset()`으로 렌더 타깃을 버립니다.

```cpp
void ClockRenderer::SetDpi(UINT dpi) {
  if (dpi_ == dpi) {
    return;
  }
  dpi_ = dpi == 0 ? 96 : dpi;
  DropTarget();
}
```

`dpi_`의 초깃값은 96이므로, 사용자의 화면 배율이 100%가 아니면 `Initialize`에서 예열한 렌더 타깃이 곧바로 버려집니다.

따라서 `src/menu_bar.cpp` 294행 **바로 뒤**에서 부르십시오.

```cpp
  layout_.SetDpi(Dpi());
  clock_.SetDpi(Dpi());
  clock_.WarmTarget();   // SetDpi가 렌더 타깃을 버리므로 반드시 그 뒤에서 부른다
  ApplyBackdrop();
```

이 지점은 `CreateWindowExW` 다음이고 `RegisterAppBar()` 앞이므로, 상단바가 화면에 자리를 잡기 전입니다.

### 3-4. `DrawTimings`는 그대로 둡니다

`create_ms`와 `end_ms` 계측은 판정에 그대로 쓰이므로 **지우지 마십시오.** 이번 수정이 효과가 있었는지를 그 값으로 판정합니다.

---

## 4. 판정 기준

재부팅한 뒤 `[bar] render target warm` 줄과 첫 `[perf] draw` 줄을 완료 보고에 그대로 옮겨 적으십시오. 판정은 셋입니다.

1. **`create`의 최댓값이 1647.0ms에서 크게 줄어야 합니다.** 예열이 렌더 타깃을 미리 만들었으므로 첫 그리기에서는 `if (!rt_)` 블록을 건너뜁니다.
2. **`end`의 최댓값도 1418.3ms에서 함께 줄어야 합니다.** 줄어들면 2절의 추정이 확정됩니다. `create`만 줄고 `end`가 그대로면 추정이 틀린 것이므로, **그 사실을 보고하고 멈추십시오.** `end`를 추가로 파고들지 마십시오.
3. **`bar full`의 최댓값이 3528.1ms에서 줄어야 합니다.** 이것이 사용자가 체감하는 값입니다.

`[bar] render target warm` 자체는 수백 밀리초에서 3초까지 나올 수 있습니다. **그 값이 크다는 것은 실패가 아닙니다.** 비용을 첫 그리기에서 창 생성 이전으로 옮기는 것이 이번 작업의 목적이기 때문입니다.

세 값을 09:05 부팅, 13:49 부팅, 이번 부팅의 순서로 표에 정리해서 보고해 주십시오.

---

## 5. 하지 말아야 할 것

- **`D2D1_FACTORY_TYPE_SINGLE_THREADED`를 바꾸지 마십시오.** `ClockRenderer::Initialize`가 이 형식으로 팩토리를 만들고 있으므로 렌더 타깃은 만든 스레드에서만 쓸 수 있습니다. 예열을 별도 스레드로 옮기는 방식은 이번 작업의 범위 밖입니다.
- **예열을 백그라운드 스레드에서 돌리지 마십시오.** 위와 같은 이유입니다.
- `DropTarget()`의 동작을 바꾸지 마십시오. DPI가 바뀌면 렌더 타깃을 버리는 것이 맞습니다.
- `WM_DPICHANGED` 경로(436행)에서 예열을 다시 부르지 마십시오. 부팅 구간이 아니고 사용자가 배율을 바꾸는 드문 상황이므로, 다음 그리기에서 만들어지는 지금 동작으로 충분합니다.
- 그리기 알고리즘 자체를 손대지 마십시오. `begin` 197ms와 `draw` 265ms는 이번 작업의 대상이 아닙니다.
- 빌드는 `D:\repos\bamti\build` 트리에 Release 구성으로 하십시오. 실행 중이면 링크가 실패하므로, 그때는 컴파일만 확인하고 그 사실을 보고해 주십시오.
- 검증 절차에 레지스트리 쓰기 명령을 넣지 마십시오. 조회만 하십시오.
- 재부팅이 필요한 검증은 수행하지 마십시오. 사용자에게 부탁할 항목입니다.
