# 작업 지시서 27: 부팅 직후 상단바가 늦게 채워지고 독이 늦게 뜬다

자동 시작을 작업 스케줄러로 옮긴 뒤(`FIX-AUTOSTART-DELAY.md`) bamti가 explorer보다 먼저 뜨게 되었습니다. 기동은 빨라졌지만, 그 대가로 셸이 준비되지 않은 상태에서 초기화를 진행하게 되었습니다. 이 지시서는 그때 생기는 세 가지를 다룹니다.

---

## 1. 측정으로 확정한 사실

2026-09-03 재부팅 세션의 로그입니다. 부팅은 07:49:54였습니다.

```
07:50:32.788  [host] start
07:50:32.793  [tray] intercept prestart elapsed_ms=0 spy=0x20014
07:50:32.794  [tray] intercept priority acquired ms=0
07:50:32.810  [host] shell wait ms=0 found=1
07:50:33.473  [widget] worker start
07:50:35.527  [tray] hidden autohide=1 windows=1
07:50:37.072  [bar] ready hwnd=000000000002012A taskbar_hidden=1
07:50:38.250  [tray] intercept icon_ms=16 png=775
07:50:40.163  [host] menu bar ready taskbar_hidden=1
07:50:40.549  [dock] create pins=19
07:50:48.897  [dock] ready hwnd=000000000004009A items=0
07:50:49.178  [tray] intercept priority acquired ms=0      <- 두 번째 획득
07:51:09.689  [tray] intercept icon_ms=0 png=1916
07:51:37.724  [perf] rebuild items=19 pins=19 shown=1 578ms
07:51:53.154  [perf] bar full[n=35 avg=122.9 max=4067.8]ms seg[n=65 avg=0.9 max=6.6]ms
              compute[n=100 avg=0.3 max=1.5]ms segments=14 overflow=0 cold
07:51:53.154  [perf] draw bind=0.18 brush=0.00 begin=1.18 draw=1.97 end=18.78
              bpbegin=0.15 bpend=0.12 (ms, avg)
```

여기서 세 가지가 드러납니다.

### 1-1. 셸 준비 판정이 너무 이르다

`shell wait ms=0 found=1`은 `WaitForShell`이 기다리지 않고 통과했다는 뜻입니다. 그런데 **16초 뒤인 07:50:49에 트레이 우선순위를 다시 잡았습니다.**

`intercept priority acquired`가 두 번 찍히는 것은 그 사이에 우선순위를 잃었다는 뜻이고, 이는 explorer가 초기화를 진행하면서 `Shell_TrayWnd`를 다시 만들었기 때문입니다. 즉 **우리가 07:50:32에 발견한 `Shell_TrayWnd`는 explorer가 완성하기 전의 창이었습니다.**

`src/host.cpp`의 `WaitForShell`은 판정 조건이 하나뿐입니다.

```cpp
    if (FindWindowW(L"Shell_TrayWnd", nullptr) != nullptr) {
      found = 1;
      break;
    }
```

이 조건만으로는 셸이 준비되었는지 가릴 수 없다는 것이 위 로그로 확인되었습니다.

이것이 중요한 이유는 `RESEARCH-TRAY-REREGISTER.md` 4절이 지적한 문제와 같기 때문입니다. 우선순위를 잃은 동안 앱이 트레이 아이콘을 등록하면 그 등록은 우리를 건너뛰고 explorer로 갑니다. 07:50:32부터 07:50:49까지 17초 동안 그 위험에 노출되어 있었습니다.

### 1-2. 상단바 그리기 한 프레임이 최대 4초 걸렸다

```
bar full[n=35 avg=122.9 max=4067.8]ms
```

평상시 같은 지표는 `avg=1.5 max=2.3`입니다. 그런데 세부 항목을 더해도 설명이 되지 않습니다.

| 항목 | 평균 |
|---|---|
| bind | 0.18 ms |
| brush | 0.00 ms |
| begin | 1.18 ms |
| draw | 1.97 ms |
| end | 18.78 ms |
| bpbegin | 0.15 ms |
| bpend | 0.12 ms |
| **합계** | **약 22.4 ms** |
| **full 평균** | **122.9 ms** |

**세부 항목의 합과 `full` 사이에 약 100ms의 설명되지 않는 구간이 있습니다.** 게다가 세부 항목에는 최댓값이 기록되지 않아서, 4067.8ms짜리 프레임이 어느 단계에서 걸렸는지 알 수 없습니다. 지금 계측으로는 원인을 좁힐 수 없습니다.

### 1-3. 독은 처음 나타날 때 비로소 목록을 만든다

`[dock] ready ... items=0`은 결함이 아닙니다. `Dock::Create`(1229행)가 `Rebuild()`를 부르지만, 그 시점에는 독이 표시 상태가 아니어서 `Rebuild()`가 조기 반환하도록 되어 있습니다.

```cpp
void Dock::Rebuild() {
  WatchdogStage(L"dock.rebuild");
  ResetPinCmpLog();
  if (!shown_) {
    pending_rebuild_ = true;
    return;
  }
```

밀린 작업은 `ShowPill()`(2140행)에서 처리합니다. 그리고 **항목이 준비된 뒤에 창을 표시하므로, 빈 독이 화면에 보이는 일은 없습니다.**

```cpp
  if (!shown_) {
    shown_ = true;
    if (pending_rebuild_) {
      Rebuild();
    }
    if (!shown_ || items_.empty()) {
      shown_ = false;
      UpdateIdleTimer();
      return;
    }
    Layout();
    ShowWindow(hwnd_, SW_SHOWNA);
```

문제는 표시 지연입니다. 사용자가 화면 아래로 마우스를 내린 그 순간에 `Rebuild()`가 처음 돌고, 부팅 직후에는 그것이 오래 걸립니다. 같은 세션에서 잰 값이 **578ms**와 **1625ms**였고, 시스템이 안정된 뒤의 값은 31~110ms입니다. 그동안 독이 나타나지 않으므로 반응이 없는 것처럼 느껴집니다.

---

## 2. 셸 준비 판정을 강화한다

`src/host.cpp`의 `WaitForShell`을 고칩니다. 창 하나가 아니라 **세 조건을 모두 만족할 때** 준비된 것으로 봅니다.

```cpp
// 셸이 준비되었는지 가린다. Shell_TrayWnd만 보면 explorer가 초기화 도중에 만드는
// 임시 창에 속는다. 2026-09-03 재부팅에서 그 창을 잡고 통과한 뒤 16초 만에 트레이
// 우선순위를 다시 잡아야 했다.
bool ShellReady() {
  const HWND tray = FindWindowW(L"Shell_TrayWnd", nullptr);
  if (tray == nullptr) {
    return false;
  }
  // 데스크톱(Progman)이 떠야 explorer가 셸 역할을 시작한 것이다.
  if (GetShellWindow() == nullptr) {
    return false;
  }
  // 알림 영역까지 만들어졌는지 본다. 트레이 가로채기가 상대할 대상이 이것이다.
  return FindWindowExW(tray, nullptr, L"TrayNotifyWnd", nullptr) != nullptr;
}
```

`WaitForShell`은 이 함수를 폴링하도록 바꾸고, 로그에 어느 조건이 만족되었는지 함께 남기십시오.

```
[host] shell wait ms=8400 found=1 tray=1 progman=1 notify=1
```

대기 상한 30초는 그대로 둡니다. 시간이 다 되어도 그냥 진행해야 합니다. 셸이 끝내 준비되지 않는 환경에서 bamti가 아예 뜨지 않는 것이 더 나쁩니다.

**`PrestartInterceptTrayBackend()` 호출은 지금 자리를 유지하십시오.** 그 함수는 셸보다 먼저 자리를 잡는 것이 목적이므로 대기보다 앞에 있어야 합니다.

### 검증

재부팅한 뒤 로그에서 다음을 확인합니다.

1. `shell wait ms=` 값이 0보다 커야 합니다. 셸을 실제로 기다렸다는 뜻입니다.
2. **`intercept priority acquired`가 한 번만 찍혀야 합니다.** 두 번 찍히면 여전히 이른 시점에 통과한 것이므로 판정 조건을 다시 보아야 합니다. 이 항목이 이번 절의 핵심 판정 기준입니다.
3. `[host] start`와 `[bar] ready`의 시차가 30초를 넘지 않아야 합니다.

---

## 3. 상단바 그리기 계측을 보강한다

1-2절에서 확인했듯이 지금 계측으로는 4초짜리 프레임의 정체를 알 수 없습니다. 원인을 고치기 전에 원인을 볼 수 있게 만들어야 합니다.

`src/menu_bar.cpp`의 1120~1145행 부근에 있는 통계 집계를 다음과 같이 넓히십시오.

### 3-1. 세부 항목마다 최댓값을 기록한다

지금은 평균만 누적하고 있습니다. 각 단계에 최댓값을 함께 들고 있다가 같은 줄에 적으십시오.

```
[perf] draw bind=0.18/2.1 brush=0.00/0.0 begin=1.18/48.3 draw=1.97/12.0
       end=18.78/3980.2 bpbegin=0.15/1.2 bpend=0.12/0.9 (ms, avg/max)
```

이 한 줄이면 4초가 어느 단계에서 나왔는지 곧바로 드러납니다.

### 3-2. 설명되지 않는 구간을 드러낸다

`full`을 재는 구간 안에서 세부 항목이 덮지 못하는 부분이 100ms 있습니다. 그 구간이 무엇인지 계측하십시오. 방법은 두 가지 중 하나를 고르면 됩니다.

- `full` 측정 시작부터 첫 세부 항목 시작까지를 `pre`로, 마지막 세부 항목 종료부터 `full` 측정 종료까지를 `post`로 새로 재는 방법.
- 세부 항목의 합을 그때그때 계산해 두고 `full`에서 뺀 값을 `other`로 기록하는 방법.

**후자를 권합니다.** 계측 지점을 새로 넣지 않아도 되고, 값이 크게 나오는 프레임이 있으면 그때 앞의 방법으로 좁히면 됩니다.

```
[perf] draw ... other=100.5/3900.1 (ms, avg/max)
```

### 3-3. 아이콘 디코딩이 그리기 경로에 있는지 확인한다

부팅 세션에서 `[icon] decode`와 `[icon] mono` 로그가 07:50:52와 07:51:09에 찍혔습니다. 이 작업이 그리기 스레드에서 동기적으로 일어난다면 그것이 100ms 구간의 정체일 수 있습니다.

`src/icon_cache.cpp`의 디코딩 경로가 어느 스레드에서 불리는지 확인하고, **결과를 완료 보고에 적어 주십시오.** 그리기 스레드에서 부르고 있다면 그 사실만 보고하고 이번 지시서에서 고치지는 마십시오. 옮기는 작업은 범위가 크므로 별도 지시서로 다룹니다.

### 검증

재부팅한 뒤 첫 `[perf] draw` 줄을 그대로 완료 보고에 옮겨 적으십시오. 판정은 그 값으로 합니다. 이번 절의 목적은 성능 개선이 아니라 **원인을 볼 수 있게 만드는 것**입니다.

---

## 4. 독이 처음 나타날 때의 지연을 없앤다

### 4-1. 무엇을 고치는가

1-3절에서 정리했듯 독은 빈 채로 뜨지 않습니다. 대신 사용자가 마우스를 내린 순간에 목록을 만들기 시작하므로, 부팅 직후에는 578~1625ms 동안 아무 반응이 없습니다.

밀린 `pending_rebuild_`를 **독이 표시되기 전, 시스템이 한가할 때 미리 처리해 두면** 이 지연이 사라집니다.

### 4-2. 방법

`Dock::Create`가 끝난 뒤 한 번만 도는 준비 타이머를 두십시오. 기존 타이머들과 같은 방식으로 `src/dock.cpp`에 상수를 더하면 됩니다.

```cpp
constexpr UINT_PTR kWarmupTimerId = 7;   // 비어 있는 id를 쓰십시오
constexpr UINT kWarmupDelayMs = 3000;
```

`WM_TIMER` 처리에서 이 id를 받으면 타이머를 죽이고, `pending_rebuild_`가 참일 때만 목록을 구성합니다. **이때 독을 화면에 표시하면 안 됩니다.** 목록만 만들어 두고 창은 그대로 숨겨 두어야 합니다.

`Rebuild()`가 `!shown_`에서 조기 반환하는 구조이므로, 준비 경로에서는 그 관문을 통과할 수 있어야 합니다. `shown_`을 임의로 켜는 방식은 **쓰지 마십시오.** `ShowPill()`과 상태가 어긋나 창이 잘못 표시될 수 있습니다. 대신 준비 중임을 나타내는 별도 플래그를 두고, `Rebuild()`의 관문을 그 플래그도 함께 보도록 넓히는 편이 안전합니다.

```cpp
  if (!shown_ && !warming_up_) {
    pending_rebuild_ = true;
    return;
  }
```

준비가 끝나면 `warming_up_`을 반드시 되돌리고, `pending_rebuild_`는 그대로 두어야 합니다. 실제 표시 시점에 창 목록이 달라져 있을 수 있으므로, `ShowPill()`이 지금처럼 한 번 더 판단할 기회를 남겨 두어야 합니다. 지문 비교(`last_window_fp_`)가 있으므로 변화가 없으면 두 번째 호출은 `rebuild skip fingerprint`로 즉시 끝납니다.

로그를 남기십시오.

```
[dock] warmup items=19 812ms
```

### 4-3. 왜 3초인가

기동 직후에는 상단바 준비와 트레이 아이콘 수신이 몰려 있습니다. 그 위에 독 목록 구성까지 겹치면 1-2절의 느린 프레임을 더 악화시킵니다. 3초는 그 고비를 넘긴 뒤이면서, 사용자가 마우스를 화면 아래로 내리기 전일 가능성이 높은 시점입니다.

이 값이 적절한지는 4-4절의 측정으로 판정합니다.

### 4-4. 검증

재부팅한 뒤 확인합니다.

1. 로그에 `[dock] warmup items=` 줄이 있고 항목 수가 0이 아니어야 합니다.
2. 그 뒤 처음으로 독을 띄웠을 때, `[perf] rebuild`가 새로 돌지 않거나 `rebuild skip fingerprint`로 끝나야 합니다. 준비가 실제로 효과를 냈다는 뜻입니다.
3. **독이 즉시 나타나야 합니다.** 부팅 후 1분 안에 마우스를 화면 아래로 내려서 확인하십시오.
4. 준비 타이머가 도는 동안 상단바가 멈추지 않아야 합니다. 같은 스레드에서 돌기 때문에 확인이 필요합니다. `[perf] bar full`의 `max` 값이 준비 이전보다 나빠지면 3초라는 값을 다시 잡아야 합니다.
5. 준비가 한 번만 돌아야 합니다. 타이머를 죽이지 않으면 3초마다 반복됩니다.

---

## 5. 주의 사항

- 세 절은 서로 다른 파일을 건드립니다. 2절은 `host.cpp`, 3절은 `menu_bar.cpp`, 4절은 `dock.cpp`입니다. 절마다 커밋을 나누어 주십시오.
- **3절에서 성능을 고치려 하지 마십시오.** 이번에 넣는 것은 계측뿐입니다. 무엇을 고쳐야 할지는 그 계측 결과를 보고 정합니다.
- 4절에서 `shown_`을 준비 목적으로 켜지 마십시오. `ShowPill()`과 `HidePill()`이 그 변수로 창의 표시 상태를 관리하고 있어서, 임의로 켜면 창이 보이지 않는데 보이는 것으로 판단하는 상태가 만들어집니다.
- 독의 항목 구성 자체를 빠르게 만드는 작업(`CollectDockApps()`가 창 하나당 COM 프로퍼티 스토어를 네 번 여는 문제, `PLAN.md` 0절)은 이번 범위가 아닙니다. 이번에는 그 비용을 사용자가 기다리지 않는 시점으로 옮기기만 합니다.
- 자동 시작을 되돌리는 방식으로 해결하려 하지 마십시오. 기동이 빨라진 것 자체는 원하는 결과입니다.
