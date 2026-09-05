# 작업 지시서 29: 부팅 구간에 남은 세 가지

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-BOOT-COLD-START.md`와 `FIX-SPY-WINDOW-VISIBLE.md`를 적용한 뒤 2026-09-03 09:05 재부팅에서 측정한 값으로 잡은 후속 작업입니다. 사용자가 보고한 증상(상단바가 비어 보임, 독이 늦게 뜸, 스파이 창이 보임)은 모두 해소되었고, 여기서 다루는 것은 그 측정 과정에서 드러난 남은 문제들입니다.

세 절은 서로 다른 파일을 건드리므로 순서 제약이 없습니다. 절마다 커밋을 나누어 주십시오.

---

## 0. 이번 재부팅에서 확인된 것

먼저 잘 동작한 항목을 적어 둡니다. 아래 작업에서 이 부분을 되돌리면 안 됩니다.

```
09:05:16.985  [tray] intercept priority acquired ms=0
09:05:22.499  [host] shell wait ms=5484 found=1 tray=1 progman=1 notify=1
09:05:35.852  [bar] ready hwnd=0000000000010474 taskbar_hidden=1
09:05:52.075  [dock] warmup items=19 4531ms
09:08:30.247  [tray] intercept roster n=4 tips="오피스키퍼|Everything|Tailscale...|KakaoTalk"
```

1. **셸 준비 판정이 의도대로 동작합니다.** 5.5초를 실제로 기다렸고 `priority acquired`가 한 번만 찍혔습니다.
2. **스파이 창 선점이 유지되고 있습니다.** `EnumWindows`로 z순서를 확인하면 bamti의 `Shell_TrayWnd`(8번)가 explorer의 것(10번)보다 앞에 있습니다. `intercept roster`가 네 개를 받은 것이 그 결과입니다.
3. **독 준비가 창을 건드리지 않습니다.** `shown=0`으로 목록만 만들었고, 이후 표시는 전부 `rebuild skip fingerprint 0ms`로 끝났습니다.

**주의: 선점 여부를 바깥 프로세스에서 `FindWindowW(L"Shell_TrayWnd", ...)`로 판정하지 마십시오.** 창 클래스는 프로세스마다 따로 등록되므로, 다른 프로세스에서 같은 이름을 찾으면 우리 창이 아니라 explorer의 창이 돌아옵니다. 검수 과정에서 이 방법으로 판정했다가 "선점이 깨졌다"는 잘못된 결론을 냈습니다. 올바른 방법은 `EnumWindows`로 클래스 이름을 직접 비교하거나, `intercept roster`의 내용을 보는 것입니다.

---

## 1. 상단바 그리기의 `other` 구간을 특정한다

### 1-1. 측정값

```
[perf] bar full[n=32 avg=310.4 max=9694.4]ms seg[n=68 avg=1.2 max=5.4]ms
       compute[n=100 avg=0.5 max=3.7]ms segments=13 overflow=0 cold
[perf] draw bind=0.21/1.0 brush=0.00/0.0 begin=2.77/220.6 draw=2.51/217.1
       end=38.71/3662.5 bpbegin=0.25/1.7 bpend=0.19/4.6 other=55.50/5593.6 (ms, avg/max)
```

한 프레임이 **9.7초** 걸렸습니다. 그 안에서 `other`가 5.59초, `end`가 3.66초입니다. 둘을 더하면 9.26초이므로 이 한 프레임이 거의 전부를 차지합니다.

평균값도 같은 이야기를 합니다. `other`의 평균 55.50ms에 100프레임을 곱하면 5550ms로 최댓값과 거의 같습니다. 즉 **`other`는 100프레임 중 한 프레임에만 몰려 있고 나머지는 0에 가깝습니다.** 상시 비용이 아니라 일회성 비용입니다.

### 1-2. `other`의 정체

`other`는 `ClockRenderer::Draw` 전체 시간에서 계측된 다섯 구간을 뺀 나머지입니다. 그 함수에서 계측되지 않은 곳은 `src/clock_renderer.cpp` 532~545행뿐입니다.

```cpp
  if (!rt_) {
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(...);
    const HRESULT hr = d2d_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
    icons_.SetRenderTarget(rt_.Get());
    logo_brush_.Reset();
    logo_geom_size_ = 0.0f;
  }
  icons_.SetDark(dark);
```

`CreateDCRenderTarget`은 `rt_`가 비어 있을 때만 부릅니다. 첫 그리기와 `DropTarget()` 이후에만 도는 일회성 경로이고, 이는 1-1절에서 확인한 "한 프레임에만 몰려 있다"는 성질과 정확히 맞습니다.

**다만 이것은 코드를 읽어 세운 추정이고 측정된 사실이 아닙니다.** 앞서 아이콘 디코딩을 원인으로 지목했다가 `draw` 최댓값이 217ms에 그쳐 빗나간 적이 있습니다. 같은 실수를 반복하지 않으려면 계측으로 확정해야 합니다.

### 1-3. 계측을 추가한다

`src/clock_renderer.hpp` 14행의 `DrawTimings`에 필드를 더합니다.

```cpp
struct DrawTimings {
  double create_ms = 0;   // 렌더 타깃 생성. rt_가 비어 있던 프레임에만 0이 아니다.
  double bind_ms = 0;
  double brush_ms = 0;
  double begin_ms = 0;
  double draw_ms = 0;
  double end_ms = 0;
};
```

`src/clock_renderer.cpp` 532~545행을 `QueryPerformanceCounter`로 감싸서 `timings->create_ms`에 넣으십시오. **`if (!rt_)` 블록 바깥의 `icons_.SetDark(dark)`까지 포함해서 재야 합니다.** 그래야 `other`가 실제로 0에 수렴하는지 확인할 수 있습니다.

`src/menu_bar.cpp`의 `NotePerf`와 로그 줄에 `create`를 다른 항목과 같은 `avg/max` 형식으로 추가합니다.

### 1-4. `other` 계산식을 바로잡는다

`src/menu_bar.cpp` 1107~1108행의 식에 오류가 있습니다.

```cpp
  const double other_ms =
      draw_ms - (draw.bind_ms + draw.brush_ms + draw.begin_ms + draw.draw_ms + draw.end_ms + bpbegin_ms + bpend_ms);
```

`draw_ms`는 `clock_.Draw()` 호출 하나의 소요 시간입니다(`Paint()` 1188~1192행). 그런데 `bpbegin_ms`와 `bpend_ms`는 `BeginBufferedPaint`와 `EndBufferedPaint`를 잰 값이고, **둘 다 `clock_.Draw()` 바깥에서 측정됩니다**(1181행과 1195행). 포함되지 않은 시간을 빼고 있으므로 `other`가 실제보다 작게 나옵니다.

두 항을 빼십시오.

```cpp
  // draw_ms는 clock_.Draw() 하나의 시간이다. bpbegin/bpend는 그 바깥에서 재므로 빼지 않는다.
  const double other_ms = draw_ms - (draw.create_ms + draw.bind_ms + draw.brush_ms + draw.begin_ms +
                                     draw.draw_ms + draw.end_ms);
```

지금 값으로는 0.44ms 차이라 결론이 바뀌지는 않지만, 계측값은 정확해야 다음 판단에 쓸 수 있습니다.

### 1-5. 이번 절에서 성능을 고치지 마십시오

목적은 원인 확정입니다. `create_ms`가 5.6초로 확인되면 그 사실만 보고하고 멈추십시오. 무엇을 어떻게 고칠지는 그 값을 보고 정합니다. `end`(EndDraw)의 3.66초도 마찬가지입니다. 둘 다 부팅 직후 디스크와 CPU 경합이 심한 구간의 값이라, 우리 알고리즘을 고쳐서 줄어드는 성질인지부터 판단해야 합니다.

### 1-6. 검증

재부팅한 뒤 첫 `[perf] draw` 줄을 완료 보고에 그대로 옮겨 적으십시오. 판정 기준은 둘입니다.

1. `create`의 최댓값이 크게 나오고, **`other`의 최댓값이 0에 가까워야** 합니다. 그러면 1-2절의 추정이 확정됩니다.
2. `other`가 여전히 크면 추정이 틀린 것입니다. 그때는 `ClockRenderer::Draw` 안에 계측되지 않은 다른 구간이 있다는 뜻이므로, 그 사실을 보고하고 멈추십시오.

---

## 2. 부팅 중 z-order 상실 구간을 줄인다

### 2-1. 무엇이 문제인가

이번 부팅에서 z-order를 **11번** 잃었습니다. 직전 부팅(08:25)의 6번보다 늘었습니다.

```
09:05:25.997  [tray] intercept z-order lost first=0x100DE spy=0x50038 count=4
09:05:26.662  [tray] intercept z-order restored
09:05:27.701  [tray] intercept z-order lost first=0x100DE spy=0x50038 count=5
09:05:29.065  [tray] intercept z-order restored
```

잃은 시각과 되찾은 시각 사이가 665ms와 1364ms입니다. **그 구간 동안 `Shell_TrayWnd`를 찾으면 explorer의 창이 먼저 나옵니다.** 그때 앱이 트레이 아이콘을 등록하면 그 등록은 우리를 건너뛰고 explorer로 갑니다.

실제로 손실이 관측됩니다. 09:08:30의 로스터를 `RESEARCH-TRAY-REREGISTER.md` 6-1절의 방법으로 대조하면 이렇습니다.

| 구분 | 내용 |
|---|---|
| UIA가 본 서드파티 | KakaoTalk, Tailscale, **Bluetooth 장치** |
| 가로채기가 받은 것 | 오피스키퍼, Everything, Tailscale, KakaoTalk |
| **손실** | **Bluetooth 장치 한 개** |

같은 컴퓨터의 08:28 세션에서는 `intercept roster n=7`에 Bluetooth 장치가 들어 있었으므로, 이 아이콘은 원래 잡히는 것입니다. 이번에 놓친 것입니다.

사용자가 "트레이 아이콘은 어쩔 수 없게 늦게 나타난다"고 말한 현상도 같은 뿌리입니다.

### 2-2. 왜 상실 구간이 생기는가

`src/tray_intercept.cpp`의 `KeepPriority`(687행)는 **상실을 감지한 뒤에야** 우선순위를 되찾습니다.

```cpp
  void KeepPriority(HWND spy) {
    const HWND first = FindWindowW(kSpyClass, nullptr);
    if (first == spy) {
      return;                                  // 앞서 있으면 아무 일도 하지 않는다
    }
    ++z_loss_;
    Log(L"tray", L"intercept z-order lost ...");
    SetWindowPos(spy, HWND_TOPMOST, ...);
    ...
  }
```

폴링 주기는 평소 1000ms이고, 상실을 감지하면 5초 동안 100ms로 빨라집니다(35~37행). 그래도 **감지에서 되찾기까지 최소 한 주기가 비고**, 위 로그에서는 그것이 665ms 이상이었습니다.

부팅 직후는 explorer가 셸을 초기화하면서 자기 창을 계속 다시 만드는 구간이라 상실이 잦습니다. 감지 후 대응이 아니라 **선제적으로 붙잡고 있어야** 하는 구간입니다.

### 2-3. 수정 내용

기동 후 일정 시간 동안은 상실 여부와 관계없이 매 틱마다 최상위를 다시 주장하십시오.

```cpp
// 부팅 직후에는 explorer가 셸을 초기화하면서 Shell_TrayWnd를 여러 번 다시 만든다.
// 상실을 감지한 뒤에 되찾으면 그 사이에 등록한 앱을 놓치므로, 이 구간에서는
// 매 틱마다 선제적으로 최상위를 주장한다.
constexpr ULONGLONG kBootHoldMs = 90000;
constexpr UINT kPrioTimerBootMs = 100;
```

`KeepPriority`를 다음 규칙으로 고칩니다.

1. 기동 시각을 멤버로 기억해 두고, `GetTickCount64() - started_at_ < kBootHoldMs`인 동안을 "부팅 구간"으로 봅니다.
2. 부팅 구간에서는 `first == spy`여도 `SetWindowPos(spy, HWND_TOPMOST, ...)`를 부릅니다. 이미 최상위인 창에 같은 호출을 하는 것은 비용이 거의 없습니다.
3. **로그는 지금처럼 실제로 상실했을 때만 남기십시오.** 매 틱마다 로그를 남기면 부팅 구간에서만 900줄이 쌓입니다.
4. 부팅 구간의 타이머 주기는 `kPrioTimerBootMs`(100ms)로 둡니다. 구간이 끝나면 기존 규칙(평소 1000ms, 상실 뒤 5초 동안 100ms)으로 돌아갑니다.

`EnterFast`와 `SetPrioPeriod`가 이미 주기를 바꾸는 경로를 갖고 있으므로, 부팅 구간 판정을 그 경로에 얹으면 됩니다. **새 타이머를 만들지 마십시오.**

### 2-4. 90초라는 값의 근거

이번 부팅의 마지막 상실이 09:06:17로 기동 후 60초 지점이었습니다. 여유를 두어 90초로 잡았습니다.

부팅 구간이 끝나기 전에 상실이 더 일어나는지는 로그의 `z-order lost` 마지막 시각으로 판정합니다. 그 시각이 90초에 가까우면 값을 늘려야 합니다.

### 2-5. 비용을 확인한다

100ms 주기로 90초면 900회입니다. `FindWindowW`와 `SetWindowPos`를 900번 부르는 비용이 문제가 되지 않는지 확인하십시오. 유휴 CPU 예산은 0.2%이고 직전 실측이 0.0249%였습니다(`bamti-topbar-project-status` 참고).

부팅 구간이 끝난 뒤 유휴 CPU를 `Win32_PerfRawData_PerfProc_Process`로 측정해서, **평소 값이 부팅 구간의 영향을 받지 않는지** 확인하고 보고해 주십시오. 부팅 구간 안의 값은 어차피 시스템 전체가 바쁘므로 따로 재지 않아도 됩니다.

### 2-6. 검증

재부팅한 뒤 확인합니다.

1. **`z-order lost`의 횟수가 11회보다 줄어야 합니다.** 이번 절의 판정 기준입니다.
2. 기동 3분 뒤의 `intercept roster`와 `uia roster`를 대조해 **서드파티 손실이 0이어야** 합니다. 판정 방법은 `RESEARCH-TRAY-REREGISTER.md` 6-1절에 있습니다. 특히 Bluetooth 장치가 `intercept roster`에 들어왔는지 보십시오.
3. `z-order lost`의 마지막 시각이 기동 후 90초 안쪽인지 확인하십시오.
4. 트레이 아이콘을 좌클릭과 우클릭해 각 앱의 메뉴가 뜨는지 확인하십시오.

---

## 3. 독 준비가 UI를 막는 시간을 계측한다

### 3-1. 무엇이 문제인가

```
09:05:52.075  [dock] warmup items=19 4531ms
```

`FIX-BOOT-COLD-START.md` 4절로 넣은 준비 타이머가 **UI 스레드를 4.5초 동안 막습니다.** 직전 부팅에서는 18.4초였습니다.

의도한 효과는 났습니다. 준비 덕분에 이후 표시가 전부 `rebuild skip fingerprint 0ms`로 끝나고 독이 즉시 뜹니다. 총 비용이 늘어난 것도 아닙니다. 이전에는 같은 비용을 사용자가 마우스를 내린 시점에 치렀습니다.

문제는 그 비용을 **부팅 직후 가장 바쁜 구간으로 옮겨 놓았다**는 점입니다. 상단바와 독이 같은 스레드에서 돌기 때문에, 그동안 상단바가 메시지를 처리하지 못합니다. 1절의 9.7초 프레임과 겹치면 체감이 나빠집니다.

### 3-2. 계측을 먼저 넣는다

4.5초가 어디서 나오는지 모르는 상태입니다. `Dock::Rebuild`(`src/dock.cpp` 1667행)의 단계별 시간을 재십시오.

| 구간 | 위치 |
|---|---|
| 지문 계산 | `TaskWindowFingerprint()` — 이미 `rebuild fingerprint %ums`로 남고 있습니다 |
| 목록 수집 | `CollectDockApps(pins_)` (1693행) |
| 아이콘 적재 | `EnsureIcons()` (1720행과 1733행, 두 번 불립니다) |

준비 경로에서만 이 값을 남기면 충분합니다. 평상시 `Rebuild`마다 남길 필요는 없습니다.

```
[dock] warmup items=19 4531ms collect=... icons=... (ms)
```

`CollectDockApps`가 창 하나당 COM 프로퍼티 스토어를 네 번 여는 문제는 `PLAN.md` 0절에 이미 적혀 있습니다. 그것이 지배적인지, 아니면 아이콘 적재가 지배적인지를 이 계측으로 가립니다.

### 3-3. 이번 절에서도 성능을 고치지 마십시오

계측만 넣으십시오. 어느 쪽을 고칠지는 값을 보고 정합니다.

특히 **`EnsureIcons()`를 준비 경로에서 빼는 방식은 쓰지 마십시오.** 그 뒤의 필터가 아이콘 적재 결과로 항목을 걸러 내고 있어서(1722~1730행), 아이콘 없이 넘어가면 실행 중인 비고정 앱이 목록에서 빠집니다.

```cpp
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == DockItemKind::kSpotlight || items_[i].pinned ||
        (i < icons_.size() && icons_[i] != nullptr)) {
```

### 3-4. 검증

재부팅한 뒤 `[dock] warmup` 줄을 완료 보고에 그대로 옮겨 적으십시오. `collect`와 `icons`의 합이 전체 시간에 가까운지도 함께 확인해 주십시오. 크게 벌어지면 계측되지 않은 구간이 또 있다는 뜻입니다.

---

## 4. 주의 사항

- 세 절 모두 **계측이 목적인 부분과 수정이 목적인 부분이 섞여 있습니다.** 1절과 3절은 계측만, 2절은 실제 수정입니다. 1절과 3절에서 성능 개선을 시도하지 마십시오.
- 0절에 적은 세 가지가 깨지지 않아야 합니다. 특히 스파이 창의 창 스타일(`WS_POPUP`, 0 크기)을 되돌리지 마십시오. 그것이 바탕 화면에 창이 보이던 문제의 해법입니다.
- 2절에서 클래스 이름을 바꾸거나 메시지 전용 창으로 만드는 방식은 쓰지 마십시오. 선점이 완전히 깨집니다.
- 자동 시작을 되돌려 기동을 늦추는 방식으로 z-order 경쟁을 피하려 하지 마십시오. 빨라진 기동은 원하는 결과입니다.
- 빌드는 `D:\repos\bamti\build` 트리에 Release 구성으로 하십시오. 자동 시작 작업이 그 경로의 실행 파일을 가리킵니다. 실행 중이면 링크가 실패하므로, 그때는 컴파일만 확인하고 그 사실을 보고해 주십시오.
- 검증 절차에 레지스트리 쓰기 명령을 넣지 마십시오. 조회만 하십시오.
- 재부팅이 필요한 검증은 수행하지 마십시오. 사용자에게 부탁할 항목입니다.
