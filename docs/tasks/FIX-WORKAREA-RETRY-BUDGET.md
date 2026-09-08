# 작업 지시서: 재시도 예산이 엉뚱한 곳에서 소진되고 포기 뒤에 회복하지 못하는 것을 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-WORKAREA-SETTLE-2.md`(커밋 `7f0986b`)가 넣은 재시도가 **처음으로 실전에서 발화했고, 그리고 실패했습니다.** 건드리는 파일은 `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 입니다.

---

## 1. 측정으로 확인한 것

`~/.bamti/bamti.log` 의 `2026-09-08 10:17` 실행입니다. 이 회차는 `want=32`(DPI 100%) 였고, 앞서 잘 되던 회차들은 모두 `want=48` 이었습니다.

```
10:17:22.735 [bar] workarea top=0 want=32 forced=0 moved=0 retry=1 spi_fail=1
10:17:24.389 [bar] workarea top=0 want=32 forced=0 moved=0 retry=2 spi_fail=2
10:17:25.796 [bar] workarea spi giveup n=3
10:17:25.796 [bar] workarea top=0 want=32 forced=0 moved=0 retry=3 spi_fail=3
10:17:25.811 [bar] workarea top=0 want=32 forced=0 moved=0 retry=4 spi_fail=3
10:17:25.814 [bar] workarea top=0 want=32 forced=0 moved=0 retry=5 spi_fail=3
10:17:25.816 [bar] workarea top=0 want=32 forced=0 moved=0 retry=6 spi_fail=3
10:17:28.594 [bar] workarea retry n=6 mi=0 spi=0
   ... 타이머 발화 n=7, 8, 9, 10 ...
10:17:30.094 [bar] workarea retry giveup n=10 top=0 want=32
10:18:20.747 [bar] workarea top=32 want=32 forced=0 moved=1 retry=0 spi_fail=3
```

### 결함 1: 예산이 타이머가 아닌 곳에서 소진된다

`work_area_retry_` 는 "재시도 횟수"라는 이름을 달고 있지만 실제로는 **잡지 못한 모든 회차**를 셉니다. `WM_SETTINGCHANGE` 나 `ABN_POSCHANGED` 로 불려서 실패해도 오릅니다.

`10:17:25.796` 부터 `10:17:25.816` 까지 **0.02초 만에 3에서 6으로 뛴 것**이 그 증거입니다. 이 세 회차는 타이머가 아니라 바깥에서 온 알림이 부른 것입니다. 실제 타이머 발화는 `n=6` 부터 `n=10` 까지 다섯 번뿐인데, 열 개의 예산은 이미 절반이 남의 손에 쓰였습니다.

### 결함 2: 포기한 뒤에 스스로 회복하지 못한다

`workarea retry giveup` 뒤에는 타이머를 다시 걸지 않습니다. `work_area_retry_` 를 0 으로 되돌리는 자리는 "잡았을 때"와 "`want` 가 바뀌었을 때"뿐이므로, **포기한 뒤에는 바깥 알림이 우연히 오기를 기다리는 것 말고 할 수 있는 일이 없습니다.**

`10:17:30.094` 에 포기하고 `10:18:20.747` 에 잡혔으니 **50초를 그대로 흘려보냈습니다.** 그동안 작업 영역은 `top=0` 이라 최대화한 창이 상단바에 가립니다.

두 결함은 짝입니다. 결함 1이 예산을 순식간에 태우고, 결함 2가 그 뒤를 방치합니다. `FIX-WORKAREA-SETTLE-2.md` 에서 무한 재시도를 막으려고 리셋 조건을 좁힌 것이 반대편 극단으로 간 셈입니다.

**아직 모르는 것도 적어 둡니다.** 50초 뒤에 무엇이 작업 영역을 잡아 주었는지는 로그가 없어 알 수 없습니다. 그래서 이번 수정에는 그 구간을 볼 관측 하나를 함께 넣습니다.

## 2. 고칠 것

### 2.1 상수

`kWorkAreaRetryMs` 와 `kWorkAreaRetryMax` 를 다음으로 바꾸십시오. 간격이 늘어나므로 이름에 `Min` 을 붙입니다.

```cpp
constexpr UINT kWorkAreaRetryMinMs = 300;
constexpr UINT kWorkAreaRetryMaxMs = 30000;
```

`kWorkAreaRetryMax`(횟수 한도)는 **없앱니다.** 포기하지 않기 때문입니다. `kWorkAreaSpiGiveUp` 은 그대로 두십시오. 그쪽은 비싼 호출을 그만두는 별개의 장치이고 이번에 제대로 작동했습니다.

### 2.2 간격 계산

익명 이름공간에 두십시오. `ReadWorkAreaTop` 근처가 알맞습니다.

```cpp
// 실패가 이어질수록 간격을 늘린다. 300, 600, 1200, ... 30000 밀리초에서 멈춘다.
UINT WorkAreaRetryDelayMs(unsigned tries) {
  UINT ms = kWorkAreaRetryMinMs;
  for (unsigned i = 0; i < tries && ms < kWorkAreaRetryMaxMs / 2; ++i) {
    ms *= 2;
  }
  return ms > kWorkAreaRetryMaxMs ? kWorkAreaRetryMaxMs : ms;
}
```

이러면 처음 여덟 번이 약 1분을 덮고, 그 뒤로는 30초마다 한 번씩 계속 붙잡습니다. 이 간격이면 재시도가 오래 이어져도 로그는 분당 두 줄 남짓이라 넘치지 않습니다.

### 2.3 헤더

`src/menu_bar.hpp` 입니다. 인자를 되살리되 **용도가 다릅니다.** 앞서는 횟수를 *되돌리는* 데 썼다가 실패했고, 이번에는 횟수를 *올릴지* 정하는 데 씁니다.

```cpp
  void ReserveWorkArea(bool from_retry = false);
  void NoteWorkAreaWait(const wchar_t* where);
```

```cpp
  unsigned work_area_retry_ = 0;
  unsigned work_area_spi_fail_ = 0;
  LONG work_area_want_ = 0;
  bool work_area_pending_ = false;
```

### 2.4 본체

`MenuBar::ReserveWorkArea()` 의 서명과 끝부분을 바꾸십시오. **가운데의 `SPI_SETWORKAREA` 구간과 `applied` 판정은 한 글자도 건드리지 마십시오.**

```cpp
void MenuBar::ReserveWorkArea(bool from_retry) {
```

끝부분은 이렇게 됩니다.

```cpp
    if (info.rcWork.top >= want) {
      moved = RemaximizeOverlapping(hwnd_, monitor, info.rcWork);
      work_area_retry_ = 0;
      work_area_pending_ = false;
    } else {
      if (from_retry) {
        ++work_area_retry_;  // 예산은 타이머 발화로만 쓴다. 바깥 알림은 세지 않는다.
      }
      work_area_pending_ = true;
      SetTimer(hwnd_, kWorkAreaRetryTimerId, WorkAreaRetryDelayMs(work_area_retry_), nullptr);
    }
    Log(L"bar", L"workarea top=%ld want=%ld forced=%d moved=%d retry=%u spi_fail=%u", info.rcWork.top, want,
        work_area_forced_ ? 1 : 0, moved, work_area_retry_, work_area_spi_fail_);
```

`workarea retry giveup` 로그는 함께 사라집니다. `want != work_area_want_` 갈래에서 `work_area_retry_` 와 `work_area_spi_fail_` 을 0 으로 되돌리는 것은 그대로 두십시오.

**같은 타이머 식별자로 `SetTimer` 를 다시 부르면 기존 예약이 재설정됩니다.** 바깥 알림이 잦은 동안에는 발화가 뒤로 밀리지만, 그때는 이미 `ReserveWorkArea` 가 그 알림을 타고 실행되고 있으므로 잡을 기회를 잃지 않습니다.

### 2.5 타이머 처리부

`ReserveWorkArea(true);` 로 부르십시오.

```cpp
        Log(L"bar", L"workarea retry n=%u mi=%ld spi=%ld", work_area_retry_, mi_top, spi_top);
        ReserveWorkArea(true);
```

## 3. 함께 넣을 관측 하나

50초 구간에 무엇이 도착했는지 알아야 다음에 또 실패했을 때 원인을 짚을 수 있습니다. **아직 잡지 못한 동안에만 찍는** 기록을 하나 두십시오. 평소에는 한 줄도 남지 않습니다.

```cpp
// 작업 영역을 아직 못 잡은 동안에만 알림 도착을 남긴다. 잡히면 조용해진다.
void MenuBar::NoteWorkAreaWait(const wchar_t* where) {
  if (!work_area_pending_ || hwnd_ == nullptr) {
    return;
  }
  LONG mi_top = -1;
  LONG spi_top = -1;
  ReadWorkAreaTop(hwnd_, &mi_top, &spi_top);
  Log(L"bar", L"workarea wait at=%s mi=%ld spi=%ld retry=%u", where, mi_top, spi_top, work_area_retry_);
}
```

부르는 자리는 **두 곳뿐입니다.**

- `WM_SETTINGCHANGE` 갈래의 `ReserveWorkArea();` 바로 앞에 `NoteWorkAreaWait(L"settingchange");`
- `kAppBarCallback` 의 `case ABN_POSCHANGED:` 에서 `Layout();` 바로 앞에 `NoteWorkAreaWait(L"appbar");`

`MenuBar::Layout()` 안에는 넣지 마십시오. 그 함수는 자주 불리고, 앞서 걷어낸 `layout-pre` 와 `layout-post` 가 바로 그것이었습니다.

## 4. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

**`kWorkAreaRetryMax` 를 지우고 나면 쓰지 않는 것이 남아 경고가 날 수 있습니다.** 경고가 하나라도 나오면 정리가 덜 된 것이므로 마저 처리하십시오.

2. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
3. 새 빌드를 실행하고 시작 직후 삼십 초 동안의 `workarea` 로그를 시간 순서 그대로 전부 보고하십시오.
4. 다음 네 가지를 판정해서 각각 참인지 거짓인지 적으십시오.

| 확인할 것 | 판정 방법 |
| --- | --- |
| 자리 잡기가 예전만큼 빠르다 | 첫 `workarea` 줄과 `top` 이 `want` 에 닿은 첫 줄의 시각 차이를 재십시오. 잘 되던 회차는 0.43초와 0.83초였습니다 |
| `workarea retry giveup` 이 더는 없다 | 새 실행 구간에서 그 문자열을 찾아보십시오 |
| `retry=` 가 갑자기 뛰지 않는다 | 한 회차에 1 씩만 오르는지, 그리고 그 직전에 `workarea retry n=` 이 있는지 보십시오 |
| 작업 영역이 상단바 아래로 잡힌다 | `[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea` 의 `Y` 를 재십시오 |

5. **실패 조건을 일부러 만들지 마십시오.** 이번 결함은 `want=32` 인 회차에서 드러났지만, 그것을 재현하려고 화면 배율이나 해상도를 바꾸면 안 됩니다. 사용자 설정을 건드리는 일입니다. 다음에 그 조건이 저절로 관측되면 `workarea wait at=` 로그가 답을 줄 것입니다.
6. 확인하지 못한 항목은 확인하지 못했다고 적으십시오.

## 5. 하지 말 것

- **`SPI_SETWORKAREA` 구간과 `applied` 판정을 건드리지 마십시오.** 검증까지 끝난 부분이고, 여기를 손대서 회귀가 난 적이 있습니다.
- `kWorkAreaSpiGiveUp` 을 없애거나 값을 바꾸지 마십시오. 이번 실행에서 제대로 작동해 비싼 호출을 세 번에서 끊었습니다.
- `work_area_retry_` 를 다른 자리에서 0 으로 되돌리지 마십시오. 리셋 자리는 "잡았을 때"와 "`want` 가 바뀌었을 때" 둘뿐입니다.
- `from_retry` 를 횟수를 *되돌리는* 데 쓰지 마십시오. 그 설계는 이미 한 번 실패했습니다. 이번 용도는 횟수를 *올릴지* 정하는 것뿐입니다.
- `MenuBar::Layout()` 안에 관측을 넣지 마십시오.
- `MenuBar::Create()` 의 시작 순서를 바꾸지 마십시오.
- 키보드 관련 코드는 건드리지 마십시오.
- 레지스트리에 쓰지 마십시오.
- `src/menu_bar.cpp` 와 `src/menu_bar.hpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
