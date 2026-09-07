# 작업 지시서: 통하지 않는 강제를 실측으로 판정하고, 작업 영역이 빨리 자리 잡게 만든다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-WORKAREA-PROBE.md` 의 탐침이 원인을 확정했습니다(커밋 `6ac938a`). 그 측정 결과를 근거로 두 가지를 고칩니다. 건드리는 파일은 `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 입니다.

---

## 1. 측정으로 확정된 사실

`~/.bamti/bamti.log` 의 실측입니다. 이 지시서의 모든 판단은 여기에서 나옵니다.

```
07:42:11.104 [bar] workarea set want=48 pre_mi=0 pre_spi=0 ok=1 err=0 post_mi=0 post_spi=0
07:42:15.998 [bar] workarea set want=48 pre_mi=0 pre_spi=0 ok=1 err=0 post_mi=0 post_spi=0
07:42:16.012 [bar] workarea at=settingchange mi=0  spi=0
07:42:16.173 [bar] workarea at=layout-post   mi=48 spi=48
07:42:20.073 [bar] workarea recheck mi=48 spi=48
```

읽어야 할 것이 셋입니다.

1. **`SystemParametersInfoW(SPI_SETWORKAREA, ...)` 는 `TRUE` 를 돌려주고도 적용되지 않습니다.** `ok=1` 인데 호출 직후에 `GetMonitorInfoW` 와 `SPI_GETWORKAREA` 로 **둘 다** 읽어도 옛 값이었습니다. 읽는 경로가 하나뿐이었다면 "모니터 정보만 낡았다"는 가설과 구분되지 않았을 텐데, 둘 다 0 이므로 그 가설은 배제됩니다.
2. **작업 영역을 실제로 잡는 것은 `MenuBar::Layout()` 의 `ABM_SETPOS` 입니다.** `settingchange` 에서 0 이던 값이 `layout-post` 에서 48 이 되었습니다. `ABM_SETPOS` 는 그 호출 안에서 동기적으로 `WM_SETTINGCHANGE` 를 이 스레드로 보냅니다. 그러므로 `ReserveWorkArea()` 안의 주석 `ABM_SETPOS is often ignored` 는 **사실과 반대입니다.** 무시당하는 쪽은 `SPI_SETWORKAREA` 입니다.
3. **자리를 잡기까지 아홉 초가 걸렸습니다.** `07:42:11` 에 0 으로 떨어진 뒤 `07:42:20` 에야 48 로 안정되었습니다. 그동안 되찾아 준 것은 우연히 도착한 설정 변경 알림과 앱바 알림이 부른 `Layout()` 이었습니다. 스스로 다시 잡으려는 시도는 코드에 없습니다.

## 2. 1부: 표식은 반환값이 아니라 실측으로 남긴다

### 무엇이 잘못되었는가

`work_area_forced_` 와 `~/.bamti/workarea.guard` 는 "우리가 작업 영역을 강제했으니 비정상 종료 뒤 다음 실행이 되돌려야 한다"는 뜻입니다. 그런데 지금은 `ok != FALSE` 만 보고 표식을 남깁니다. 위 측정대로 `ok=1` 이면서 아무것도 적용되지 않는 회차가 있으므로, **강제하지도 않은 것을 강제했다고 기록하고 있습니다.** 다음 실행이 `ReleaseLeftoverWorkArea()` 로 작업 영역을 헛되이 초기화하며, 앱바가 곧 다시 잡아 주기 때문에 여태 증상으로 드러나지 않았을 뿐입니다.

### 고칠 것

`MenuBar::ReserveWorkArea()` 의 강제 구간에서 판정 기준을 바꾸십시오. 이미 읽고 있는 `post_spi` 를 그대로 쓰면 됩니다.

```cpp
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
        if (!work_area_forced_) {
          work_area_forced_ = true;
          WriteWorkAreaGuard();
        }
        GetMonitorInfoW(monitor, &info);
      }
```

`ReadWorkAreaTop` 은 읽기에 실패하면 `-1` 을 넣습니다. `-1 >= want` 는 거짓이므로 실패한 경우에도 표식을 남기지 않습니다.

같은 자리의 주석도 사실에 맞게 고치십시오.

```cpp
      work.top = want;  // SPI_SETWORKAREA is often ignored here; keep left/right/bottom.
```

`UnregisterAppBar()` 와 `ReleaseLeftoverWorkArea()` 는 건드리지 마십시오. 적용된 적이 없으면 `work_area_forced_` 가 거짓으로 남아 초기화를 부르지 않으니, 그것이 옳은 동작입니다.

## 3. 2부: 못 잡았으면 스스로 다시 잡는다

### 무엇을 만들 것인가

지금은 `kWorkAreaRecheckTimerId` 가 300밀리초 뒤에 값을 **찍기만** 합니다. 이 타이머를 **재시도**로 바꾸십시오. 작업 영역이 아직 `want` 에 못 미치면 `ReserveWorkArea()` 를 다시 부르고, 그 안의 `Layout()` 이 `ABM_SETPOS` 를 다시 보내게 하는 것입니다. 1절에서 확인했듯이 실제로 잡아 주는 것이 `ABM_SETPOS` 이므로, 통하는 수단을 우리가 직접 다시 부르는 셈입니다.

### 무한 반복을 막을 장치

`Layout()` 의 `ABM_SETPOS` 는 `WM_SETTINGCHANGE` 를 부르고 그 처리부는 다시 `ReserveWorkArea()` 를 부릅니다. 이 순환은 이미 있으며 `reserving_work_area_` 가 재진입만 막고 있습니다. 타이머 재시도는 다른 차례에 실행되므로 그 가드로 막히지 않습니다. **반드시 횟수 제한을 함께 넣으십시오.**

정책은 이렇습니다.

- 간격은 300밀리초로 두십시오. 더 짧게 하면 셸의 재계산 순환을 자극합니다.
- 연속 재시도는 열 번까지입니다. 삼 초 동안 못 잡으면 포기하고 로그를 한 줄 남깁니다.
- 작업 영역이 `want` 에 닿으면 횟수를 0 으로 되돌립니다.
- **타이머가 아닌 경로로 `ReserveWorkArea()` 가 불렸을 때에도 횟수를 0 으로 되돌립니다.** 설정 변경이나 앱바 알림이나 해상도 변경은 새로운 계기이므로 다시 열 번의 기회를 주는 것이 맞습니다.

### 헤더

`src/menu_bar.hpp` 에서 선언을 바꾸고 멤버를 하나 더하십시오. 기본 인자를 주므로 기존 호출부는 고치지 않아도 됩니다.

```cpp
  void ReserveWorkArea(bool from_retry = false);
```

```cpp
  bool work_area_forced_ = false;
  bool reserving_work_area_ = false;
  unsigned work_area_retry_ = 0;
```

### 상수

`kWorkAreaRecheckTimerId` 옆에 두십시오.

```cpp
constexpr UINT kWorkAreaRetryMs = 300;
constexpr unsigned kWorkAreaRetryMax = 10;
```

### 본체

`MenuBar::ReserveWorkArea()` 의 서명과 앞뒤를 다음처럼 바꾸십시오. 가운데의 강제 구간은 2절에서 고친 그대로 두고, `SetTimer` 를 부르던 자리를 걷어내는 것이 요점입니다.

```cpp
void MenuBar::ReserveWorkArea(bool from_retry) {
  if (hwnd_ == nullptr || fullscreen_occluded_ || !appbar_registered_ || reserving_work_area_) {
    return;
  }
  if (!from_retry) {
    work_area_retry_ = 0;  // 새로운 계기이므로 재시도 기회를 되살린다.
  }
  reserving_work_area_ = true;
  Layout();

  ... 가운데는 그대로 ...

    if (info.rcWork.top >= want) {
      moved = RemaximizeOverlapping(hwnd_, monitor, info.rcWork);
      work_area_retry_ = 0;
    } else if (work_area_retry_ < kWorkAreaRetryMax) {
      ++work_area_retry_;
      SetTimer(hwnd_, kWorkAreaRecheckTimerId, kWorkAreaRetryMs, nullptr);
    } else {
      Log(L"bar", L"workarea retry giveup n=%u top=%ld want=%ld", work_area_retry_, info.rcWork.top, want);
    }
    Log(L"bar", L"workarea top=%ld want=%ld forced=%d moved=%d retry=%u", info.rcWork.top, want,
        work_area_forced_ ? 1 : 0, moved, work_area_retry_);
  }
  reserving_work_area_ = false;
}
```

강제 구간 안에 있던 `SetTimer(hwnd_, kWorkAreaRecheckTimerId, 300, nullptr);` 한 줄은 **지우십시오.** 예약 여부는 위의 한 자리에서만 결정해야 합니다.

### 타이머 처리부

`kWorkAreaRecheckTimerId` 갈래를 다음으로 바꾸십시오.

```cpp
      if (wparam == kWorkAreaRecheckTimerId) {
        KillTimer(hwnd_, kWorkAreaRecheckTimerId);
        LONG mi_top = -1;
        LONG spi_top = -1;
        ReadWorkAreaTop(hwnd_, &mi_top, &spi_top);
        Log(L"bar", L"workarea retry n=%u mi=%ld spi=%ld", work_area_retry_, mi_top, spi_top);
        ReserveWorkArea(true);
        return 0;
      }
```

`WM_DESTROY` 와 `WM_ENDSESSION` 의 `KillTimer(hwnd_, kWorkAreaRecheckTimerId);` 는 그대로 두십시오.

## 4. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

2. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
3. 새 빌드를 실행하고 **시작 직후 십오 초 동안의 로그**에서 `workarea` 가 들어간 줄을 시간 순서 그대로 전부 보고하십시오.
4. 다음 네 가지를 로그와 실측으로 확인해서 각각 참인지 거짓인지 적으십시오.

| 확인할 것 | 판정 방법 |
| --- | --- |
| `applied=0` 인 회차가 있어도 표식 파일이 생기지 않는다 | `~/.bamti/workarea.guard` 의 존재 여부를 직접 보십시오 |
| 자리를 잡기까지 걸린 시간이 아홉 초보다 짧아졌다 | 첫 `workarea` 줄과 `top` 이 `want` 에 닿은 첫 줄의 시각 차이를 재십시오 |
| 재시도가 열 번을 넘지 않는다 | `workarea retry n=` 의 최댓값을 보십시오. `giveup` 이 찍혔다면 그 사실도 보고하십시오 |
| 작업 영역이 상단바 아래로 잡힌다 | `[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea` 의 `Y` 를 재십시오 |

5. 창을 최대화했을 때 상단바에 가리지 않는지 **사용자에게 확인을 부탁하십시오.**
6. bamti 를 정상 종료한 뒤 작업 영역이 화면 맨 위로 돌아오는지 `WorkingArea.Y` 로 재십시오.
7. 확인하지 못한 항목은 확인하지 못했다고 적으십시오. 추측으로 메우지 마십시오.

## 5. 하지 말 것

- **`MenuBar::Create()` 의 시작 순서를 바꾸지 마십시오.** 작업 표시줄을 숨긴 뒤 셸의 재계산이 늦게 도착하는 것이 흔들림의 원인이지만, 그것은 2부의 재시도가 덮습니다. 순서를 건드리면 앱바 등록과 작업 표시줄 처리의 다른 전제가 함께 깨집니다.
- **`SystemParametersInfoW(SPI_SETWORKAREA, ...)` 호출 자체를 지우지 마십시오.** 이 기기에서 통하지 않는다는 것만 확인했을 뿐입니다. 통하는 환경에서는 앱바보다 빨리 잡아 주므로 남겨 둡니다.
- 재시도 간격을 300밀리초보다 짧게 잡거나 횟수 제한을 없애지 마십시오. `ABM_SETPOS` 와 `WM_SETTINGCHANGE` 사이에 순환이 있습니다.
- `TASK-WORKAREA-PROBE.md` 가 넣은 탐침 로그(`workarea at=`, `workarea set`, `LogWorkAreaPoint`, `ReadWorkAreaTop`)를 지우지 마십시오. 이번 수정의 판정 근거입니다.
- `TASK-WIN-REPEAT-PROBE.md` 가 넣은 키보드 관련 코드는 건드리지 마십시오. 검증까지 끝났습니다.
- 레지스트리에 쓰지 마십시오. 작업 영역을 되돌리려고 `Explorer` 관련 키를 만지면 안 됩니다.
- `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
