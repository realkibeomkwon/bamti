# 작업 지시서: 앞 지시서가 만든 회귀를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-WORKAREA-SETTLE.md`(커밋 `4e639a6`)의 후속입니다. 구현은 지시대로 되어 있으므로 **잘못은 지시한 쪽에 있습니다.** 검수에서 결함 세 개를 확인했고, 그 결과 작업 영역이 자리 잡기까지 걸리는 시간이 아홉 초에서 **아흔세 초로 늘었습니다.** 건드리는 파일은 `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 입니다.

---

## 1. 측정으로 확인한 회귀

`~/.bamti/bamti.log` 의 실측입니다.

```
08:06:18.491 [bar] workarea set want=48 pre_mi=0 pre_spi=0 ok=1 err=0 post_mi=0 post_spi=0 applied=0
08:06:18.492 [bar] workarea top=0 want=48 forced=0 moved=0 retry=1
08:06:18.686 [bar] workarea at=appbar-poschanged mi=0 spi=0
08:06:18.830 [bar] workarea at=settingchange mi=48 spi=48
08:06:20.376 [bar] workarea set want=48 pre_mi=0 pre_spi=0 ok=1 err=0 post_mi=0 post_spi=0 applied=0
```

`applied=0` 인 회차가 **쉰다섯 번** 반복되었고, `08:06:18.491` 부터 `08:07:45.117` 의 `applied=1` 까지 여든일곱 초가 걸렸습니다. `workarea retry giveup` 은 한 번도 찍히지 않았습니다.

### 결함 1: 모니터 정보 갱신이 표식 판정에 묶였다

앞 지시서가 `if (ok != FALSE)` 를 `if (applied)` 로 바꾸라고 하면서, **그 블록 안에 있던 `GetMonitorInfoW(monitor, &info)` 까지 함께 조건 안에 남겼습니다.** 이것이 회귀의 직접 원인입니다.

예전에는 `ok` 가 늘 참이었으므로 `info` 가 **항상** 갱신되었습니다. 지금은 `applied` 가 늘 거짓이므로 **한 번도** 갱신되지 않습니다. 그래서 아래의 `if (info.rcWork.top >= want)` 판정이 `Layout()` 직후에 읽은 옛 값 0 만 보게 되었습니다.

로그가 이것을 그대로 보여 줍니다. `08:06:18.830` 에 작업 영역이 실제로 48 이 되었는데도, 코드는 계속 0 이라고 판정하고 재시도를 예약했습니다.

**표식을 남기는 판정과 상태를 다시 읽는 일은 서로 다른 목적입니다.** 하나의 조건문에 묶은 것이 잘못이었습니다.

### 결함 2: 통하지 않는 호출이 스레드를 오래 막는다

`SystemParametersInfoW(SPI_SETWORKAREA, ...)` 에 `SPIF_SENDCHANGE` 를 붙이면 모든 최상위 창에 `WM_SETTINGCHANGE` 를 보내고 답을 기다립니다. 이 기기에서는 한 번에 **0.6초에서 1.6초**가 걸립니다.

증거는 타이머가 밀린 것입니다. `08:06:18.492` 에 300밀리초 재시도를 예약했는데 다음 기록이 `08:06:20.376` 입니다. **1.88초 동안 `WM_TIMER` 가 처리되지 못했습니다.** 그동안 상단바는 아무 입력에도 응답하지 못합니다.

그런데 이 호출은 이 기기에서 아무 효과가 없습니다. `applied=0` 이 쉰다섯 번 연속으로 나왔습니다. **효과가 없다는 것이 확인된 뒤에도 계속 부른 것이 잘못입니다.** 앞 지시서가 "호출 자체를 지우지 마십시오"라고만 적고 비용을 따지지 않았습니다.

### 결함 3: 재시도 횟수 제한이 무력화되었다

`if (!from_retry) { work_area_retry_ = 0; }` 때문입니다. `WM_SETTINGCHANGE` 와 앱바 알림이 끊임없이 들어와 `ReserveWorkArea()` 를 부르므로, 횟수가 거의 매번 0 으로 돌아갔습니다. 로그의 `retry=` 가 계속 1 에 머무르고 `giveup` 이 한 번도 찍히지 않은 까닭입니다. **열 번 제한은 사실상 없는 것이나 마찬가지였습니다.**

"새로운 계기이므로 기회를 되살린다"는 발상 자체가 틀렸습니다. 우리가 유발한 알림과 바깥에서 온 알림을 구분할 수 없기 때문입니다.

## 2. 고칠 것

### 2.1 헤더

`from_retry` 인자는 더 쓰지 않으므로 없앱니다. `src/menu_bar.hpp` 를 이렇게 되돌리고 멤버를 둘 더하십시오.

```cpp
  void ReserveWorkArea();
```

```cpp
  bool work_area_forced_ = false;
  bool reserving_work_area_ = false;
  unsigned work_area_retry_ = 0;
  unsigned work_area_spi_fail_ = 0;
  LONG work_area_want_ = 0;
```

### 2.2 상수

`kWorkAreaRetryMax` 옆에 더하십시오.

```cpp
constexpr unsigned kWorkAreaSpiGiveUp = 3;
```

### 2.3 본체

`MenuBar::ReserveWorkArea()` 를 다음으로 바꾸십시오. 앞부분의 가드와 `Layout()` 호출은 그대로입니다.

```cpp
void MenuBar::ReserveWorkArea() {
  if (hwnd_ == nullptr || fullscreen_occluded_ || !appbar_registered_ || reserving_work_area_) {
    return;
  }
  reserving_work_area_ = true;
  Layout();

  const HMONITOR monitor = MonitorFromWindow(hwnd_, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  int moved = 0;
  if (GetMonitorInfoW(monitor, &info)) {
    const LONG want = info.rcMonitor.top + BarHeightPx();
    if (want != work_area_want_) {
      // 해상도나 DPI가 바뀌었다. 앞서 배운 실패는 더 이상 근거가 되지 못한다.
      work_area_want_ = want;
      work_area_retry_ = 0;
      work_area_spi_fail_ = 0;
    }
    if (info.rcWork.top < want && work_area_spi_fail_ < kWorkAreaSpiGiveUp) {
      LONG pre_mi = -1;
      LONG pre_spi = -1;
      ReadWorkAreaTop(hwnd_, &pre_mi, &pre_spi);
      RECT work = info.rcWork;
      work.top = want;  // SPI_SETWORKAREA is often ignored here; keep left/right/bottom.
      SetLastError(0);
      const BOOL ok = SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, SPIF_SENDCHANGE);
      const DWORD err = ok != FALSE ? 0 : GetLastError();
      LONG post_mi = -1;
      LONG post_spi = -1;
      ReadWorkAreaTop(hwnd_, &post_mi, &post_spi);
      // 반환값은 적용을 뜻하지 않는다. 실제로 잡혔을 때에만 되돌릴 책임을 진다.
      const bool applied = post_spi >= want;
      Log(L"bar", L"workarea set want=%ld pre_mi=%ld pre_spi=%ld ok=%d err=%lu post_mi=%ld post_spi=%ld applied=%d",
          want, pre_mi, pre_spi, ok != FALSE ? 1 : 0, err, post_mi, post_spi, applied ? 1 : 0);
      if (applied) {
        work_area_spi_fail_ = 0;
        if (!work_area_forced_) {
          work_area_forced_ = true;
          WriteWorkAreaGuard();
        }
      } else if (++work_area_spi_fail_ == kWorkAreaSpiGiveUp) {
        Log(L"bar", L"workarea spi giveup n=%u", work_area_spi_fail_);
      }
      // 표식을 남겼는지와 무관하게 다시 읽는다. 이 호출이 셸을 깨워 잡혔을 수 있다.
      GetMonitorInfoW(monitor, &info);
    }
    if (info.rcWork.top >= want) {
      moved = RemaximizeOverlapping(hwnd_, monitor, info.rcWork);
      work_area_retry_ = 0;
    } else if (work_area_retry_ < kWorkAreaRetryMax) {
      ++work_area_retry_;
      SetTimer(hwnd_, kWorkAreaRecheckTimerId, kWorkAreaRetryMs, nullptr);
    } else {
      Log(L"bar", L"workarea retry giveup n=%u top=%ld want=%ld", work_area_retry_, info.rcWork.top, want);
    }
    Log(L"bar", L"workarea top=%ld want=%ld forced=%d moved=%d retry=%u spi_fail=%u", info.rcWork.top, want,
        work_area_forced_ ? 1 : 0, moved, work_area_retry_, work_area_spi_fail_);
  }
  reserving_work_area_ = false;
}
```

바뀐 곳을 짚어 둡니다.

- `work_area_retry_` 를 0 으로 되돌리는 자리는 **작업 영역을 실제로 잡았을 때와 `want` 가 바뀌었을 때 둘뿐입니다.** 이것으로 열 번 제한이 되살아납니다.
- `GetMonitorInfoW(monitor, &info)` 가 `applied` 조건 **밖**으로 나왔습니다. 결함 1의 수정입니다.
- `work_area_spi_fail_` 이 세 번 차면 그 뒤로는 `SPI_SETWORKAREA` 를 부르지 않습니다. 재시도는 계속하지만 그 내용은 `Layout()` 의 `ABM_SETPOS` 뿐이므로 값싸고, 측정에 따르면 실제로 잡아 주는 것도 그쪽입니다.

### 2.4 타이머 처리부

인자가 없어졌으므로 호출을 고치십시오.

```cpp
        Log(L"bar", L"workarea retry n=%u mi=%ld spi=%ld", work_area_retry_, mi_top, spi_top);
        ReserveWorkArea();
```

## 3. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

2. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
3. 새 빌드를 실행하고 시작 직후 삼십 초 동안의 `workarea` 로그를 시간 순서 그대로 전부 보고하십시오.
4. 다음 다섯 가지를 판정해서 각각 참인지 거짓인지 적으십시오.

| 확인할 것 | 판정 방법 |
| --- | --- |
| `applied=0` 인 회차가 세 번을 넘지 않는다 | `applied=0` 을 세십시오. `workarea spi giveup` 이 찍히면 그 시각도 적으십시오 |
| 자리를 잡기까지 걸린 시간이 아홉 초보다 짧다 | 첫 `workarea` 줄과 `top` 이 `want` 에 닿은 첫 줄의 시각 차이를 재십시오 |
| 재시도 타이머가 실제로 발화한다 | `workarea retry n=` 의 `n` 이 1 이상으로 오르는지 보십시오 |
| 작업 영역이 상단바 아래로 잡힌다 | `[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea` 의 `Y` 를 재십시오 |
| 정상 종료하면 작업 영역이 돌아온다 | 종료 뒤 같은 방법으로 `Y` 를 재고, `~/.bamti/workarea.guard` 가 없는지 보십시오 |

5. 두 번째 항목이 거짓이면 **고쳐졌다고 보고하지 마십시오.** 로그를 그대로 보고하고 판정을 미루십시오.
6. 창을 최대화했을 때 상단바에 가리지 않는지 **사용자에게 확인을 부탁하십시오.**

## 4. 하지 말 것

- **`work_area_retry_` 를 다른 자리에서 0 으로 되돌리지 마십시오.** 결함 3이 그대로 되살아납니다.
- `GetMonitorInfoW(monitor, &info)` 를 다시 조건문 안으로 넣지 마십시오. 결함 1이 그대로 되살아납니다.
- `SPIF_SENDCHANGE` 를 떼거나 다른 플래그로 바꾸지 마십시오. 셸이 변경을 모르게 되며, 이 기기에서 적용되지 않는 원인은 플래그가 아닙니다.
- `MenuBar::Create()` 의 시작 순서를 바꾸지 마십시오.
- 탐침 로그(`workarea at=`, `workarea set`, `LogWorkAreaPoint`, `ReadWorkAreaTop`)를 지우지 마십시오. 이번 판정의 근거입니다.
- 키보드 관련 코드는 건드리지 마십시오. 검증까지 끝났습니다.
- 레지스트리에 쓰지 마십시오.
- `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
