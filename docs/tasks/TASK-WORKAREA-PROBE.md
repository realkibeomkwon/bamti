# 작업 지시서: 작업 영역이 흔들리는 원인을 측정으로 가른다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

시작 직후에 작업 영역의 위쪽 경계가 `48 → 0 → 48` 로 흔들립니다. 지금 있는 로그만으로는 원인을 가릴 수 없으므로, **이번 지시서는 탐침만 넣습니다. 동작은 한 줄도 바꾸지 마십시오.** 건드리는 파일은 `src/menu_bar.cpp` 하나입니다.

---

## 1. 지금 무엇을 모르는가

`~/.bamti/bamti.log` 에 남은 회차입니다.

```
2026-09-08 07:01:13.441 [bar] workarea top=32 want=48 forced=1 moved=0
2026-09-08 07:01:14.568 [bar] workarea top=32 want=48 forced=1 moved=0
2026-09-08 07:01:15.312 [bar] workarea top=32 want=48 forced=1 moved=0
2026-09-08 07:01:15.388 [bar] workarea top=48 want=48 forced=1 moved=0
```

`MenuBar::ReserveWorkArea()` 는 `SystemParametersInfoW(SPI_SETWORKAREA, ...)` 가 `TRUE` 를 돌려주었을 때에만 `GetMonitorInfoW` 를 다시 읽고 그 값을 `top` 으로 찍습니다. 그런데도 `top` 이 `want` 에 못 미치는 회차가 세 번 연속으로 나왔습니다.

**여기에서 반드시 짚어야 할 점은 로그의 `forced` 가 이번 회차의 결과가 아니라는 사실입니다.** `work_area_forced_` 는 한 번 참이 되면 `UnregisterAppBar` 가 부를 때까지 참으로 남는 누적 표식입니다. 그러므로 `forced=1` 은 "이번에 강제가 통했다"가 아니라 "예전 어느 회차에 강제를 시도한 적이 있다"만 뜻합니다. 지금 로그로는 이번 회차의 `SPI_SETWORKAREA` 가 성공했는지조차 알 수 없습니다.

가려야 할 갈래는 세 가지입니다.

| 가설 | 내용 | 구분하는 방법 |
| --- | --- | --- |
| A | `SPI_SETWORKAREA` 가 `TRUE` 를 돌려주고도 실제로 적용되지 않는다 | 호출 직후 `SPI_GETWORKAREA` 로 다시 읽어도 옛 값이면 A입니다 |
| B | 적용은 되었으나 `GetMonitorInfoW` 의 `rcWork` 가 낡은 값을 돌려주어 로그만 거짓이다 | `SPI_GETWORKAREA` 는 새 값인데 `GetMonitorInfoW` 만 옛 값이면 B입니다 |
| C | 적용된 뒤에 셸의 앱바 재계산이 곧바로 되돌린다 | 호출 직후에는 새 값인데 그 뒤의 관측 지점에서 옛 값으로 떨어지면 C입니다 |

가설 C를 의심하는 근거도 적어 둡니다. `ReserveWorkArea()` 는 맨 앞에서 `Layout()` 을 부르고, `Layout()` 은 `ABM_QUERYPOS` 와 `ABM_SETPOS` 를 보냅니다. `ABM_SETPOS` 는 셸이 등록된 앱바들로부터 작업 영역을 다시 계산하게 만듭니다. 한편 `ABN_POSCHANGED` 알림도 `Layout()` 을 부르고(`src/menu_bar.cpp:1214`), `WM_SETTINGCHANGE` 는 `ReserveWorkArea()` 를 부릅니다(`src/menu_bar.cpp:740`). `SPIF_SENDCHANGE` 로 보낸 변경 알림이 셸을 깨우고 셸이 다시 이 두 경로를 깨우는 순환이 있으므로, 흔들림의 정체가 이 순환일 가능성이 있습니다. **다만 이것은 코드를 읽은 추측이므로, 측정으로 확인하기 전에는 고치지 마십시오.**

## 2. 관측 지점을 하나로 모으는 도우미

작업 영역을 읽는 방법을 두 가지로 나누어 함께 찍어야 가설 A와 B가 갈립니다. 익명 이름공간에 다음 두 함수를 두십시오. 위치는 `ReleaseLeftoverWorkArea` 앞의 익명 이름공간 안이면 충분합니다.

```cpp
// 작업 영역의 위쪽 경계를 두 경로로 읽는다. mi 는 모니터 정보, spi 는 시스템 파라미터이다.
void ReadWorkAreaTop(HWND hwnd, LONG* mi_top, LONG* spi_top) {
  *mi_top = -1;
  *spi_top = -1;
  const HMONITOR monitor = MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY);
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  if (GetMonitorInfoW(monitor, &info)) {
    *mi_top = info.rcWork.top;
  }
  RECT work{};
  if (SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0) != FALSE) {
    *spi_top = work.top;
  }
}

// 관측 지점의 작업 영역을 찍는다. 직전에 찍은 값과 같으면 찍지 않는다.
void LogWorkAreaPoint(HWND hwnd, const wchar_t* where) {
  static bool seen = false;
  static LONG last_mi = 0;
  static LONG last_spi = 0;
  if (hwnd == nullptr) {
    return;
  }
  LONG mi_top = -1;
  LONG spi_top = -1;
  ReadWorkAreaTop(hwnd, &mi_top, &spi_top);
  if (seen && mi_top == last_mi && spi_top == last_spi) {
    return;
  }
  seen = true;
  last_mi = mi_top;
  last_spi = spi_top;
  Log(L"bar", L"workarea at=%s mi=%ld spi=%ld", where, mi_top, spi_top);
}
```

값이 바뀌었을 때에만 찍게 만든 것은 로그가 넘치지 않게 하려는 것입니다. 우리가 알고 싶은 것은 "값이 떨어진 직전에 무엇이 있었는가"이므로 변화 지점만 남아도 순서를 재구성할 수 있습니다.

## 3. 넣을 측정

### 3.1 회차 하나의 결과를 그대로 찍기

`MenuBar::ReserveWorkArea()` 안의 강제 구간을 다음처럼 바꾸십시오. **판정 논리와 표식 파일 처리는 그대로 두고, 측정값만 추가하는 것입니다.**

```cpp
    const LONG want = info.rcMonitor.top + BarHeightPx();
    if (info.rcWork.top < want) {
      LONG pre_mi = -1;
      LONG pre_spi = -1;
      ReadWorkAreaTop(hwnd_, &pre_mi, &pre_spi);
      RECT work = info.rcWork;
      work.top = want;  // ABM_SETPOS is often ignored; keep left/right/bottom.
      SetLastError(0);
      const BOOL ok = SystemParametersInfoW(SPI_SETWORKAREA, 0, &work, SPIF_SENDCHANGE);
      const DWORD err = ok != FALSE ? 0 : GetLastError();
      LONG post_mi = -1;
      LONG post_spi = -1;
      ReadWorkAreaTop(hwnd_, &post_mi, &post_spi);
      Log(L"bar", L"workarea set want=%ld pre_mi=%ld pre_spi=%ld ok=%d err=%lu post_mi=%ld post_spi=%ld", want,
          pre_mi, pre_spi, ok != FALSE ? 1 : 0, err, post_mi, post_spi);
      if (ok != FALSE) {
        if (!work_area_forced_) {
          work_area_forced_ = true;
          WriteWorkAreaGuard();
        }
        GetMonitorInfoW(monitor, &info);
      }
      SetTimer(hwnd_, kWorkAreaRecheckTimerId, 300, nullptr);
    }
```

기존의 `workarea top=... want=... forced=... moved=...` 줄은 지우지 말고 그대로 두십시오. 예전 로그와 대조할 기준선이 됩니다.

### 3.2 잠시 뒤에 다시 재기

타이머 식별자를 하나 더하십시오. 8번은 예전에 쓰였을 수 있으므로 비켜 갑니다.

```cpp
constexpr UINT_PTR kWorkAreaRecheckTimerId = 10;
```

`WM_TIMER` 처리에 갈래를 더하십시오. 단발성이므로 반드시 스스로를 끕니다.

```cpp
      if (wparam == kWorkAreaRecheckTimerId) {
        KillTimer(hwnd_, kWorkAreaRecheckTimerId);
        LONG mi_top = -1;
        LONG spi_top = -1;
        ReadWorkAreaTop(hwnd_, &mi_top, &spi_top);
        Log(L"bar", L"workarea recheck mi=%ld spi=%ld", mi_top, spi_top);
        return 0;
      }
```

`WM_DESTROY` 와 `WM_ENDSESSION` 에서 다른 타이머를 끄는 자리에 `KillTimer(hwnd_, kWorkAreaRecheckTimerId);` 를 함께 넣으십시오.

### 3.3 되돌리는 주체를 잡을 관측 지점

다음 네 자리에 `LogWorkAreaPoint` 를 넣으십시오. 부르는 순서가 곧 증거이므로 자리를 바꾸지 마십시오.

1. `MenuBar::Layout()` 에서 `SHAppBarMessage(ABM_QUERYPOS, &abd);` 바로 앞에 `LogWorkAreaPoint(hwnd_, L"layout-pre");`
2. 같은 함수에서 `SHAppBarMessage(ABM_SETPOS, &abd);` 바로 뒤에 `LogWorkAreaPoint(hwnd_, L"layout-post");`
3. `kAppBarCallback` 의 `case ABN_POSCHANGED:` 에서 `Layout();` 바로 앞에 `LogWorkAreaPoint(hwnd_, L"appbar-poschanged");`
4. `WM_SETTINGCHANGE` 갈래에서 `ReserveWorkArea();` 바로 앞에 `LogWorkAreaPoint(hwnd_, L"settingchange");`

`WM_SETTINGCHANGE` 는 `wparam` 으로 어떤 설정이 바뀌었는지 알려 주므로 그 값도 함께 남기면 좋습니다. 네 번째 자리는 다음처럼 두 줄로 두십시오.

```cpp
      if (msg == WM_SETTINGCHANGE && wparam == SPI_SETWORKAREA) {
        Log(L"bar", L"workarea settingchange spi=1");
      }
      LogWorkAreaPoint(hwnd_, L"settingchange");
      ReserveWorkArea();
```

## 4. 무엇으로 판정하는가

로그를 모은 뒤에 다음 표대로 읽으십시오. **이 판정은 구현자가 직접 내리고 보고해야 합니다.**

| 관측 | 결론 |
| --- | --- |
| `ok=1` 인데 `post_spi` 가 `want` 에 못 미친다 | 가설 A입니다. `SPI_SETWORKAREA` 가 성공을 보고하고도 적용되지 않았습니다 |
| `ok=1` 이고 `post_spi == want` 인데 `post_mi != want` 이다 | 가설 B입니다. 적용은 되었고 `GetMonitorInfoW` 가 낡은 값을 주었으므로 기존 로그가 거짓이었습니다 |
| `post` 가 둘 다 `want` 인데 뒤이은 관측 지점에서 값이 떨어진다 | 가설 C입니다. 값이 떨어진 것을 처음 본 관측 지점의 이름이 범인을 가리킵니다 |
| `ok=0` 인 회차가 있다 | `err` 값을 그대로 보고하십시오. 위의 세 가설과 별개의 사실입니다 |

## 5. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

2. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
3. 새 빌드를 실행하고 **시작 직후 십 초 동안의 로그**를 `~/.bamti/bamti.log` 에서 뽑으십시오. `workarea` 가 들어간 줄을 시간 순서 그대로, 잘라내지 말고 전부 보고하십시오.
4. 4절의 표대로 어느 가설인지 판정하고, 그렇게 판정한 근거가 되는 로그 줄을 함께 적으십시오.
5. 판정할 수 없으면 판정할 수 없다고 보고하십시오. 추측으로 메우지 마십시오.

## 6. 하지 말 것

- **동작을 바꾸지 마십시오.** 재시도 반복문, `ABM_SETPOS` 제거, `work_area_forced_` 의 의미 변경, `WM_SETTINGCHANGE` 에서 `ReserveWorkArea` 를 부르지 않게 만드는 것을 포함해 어떤 수정도 이번에는 넣지 않습니다. 원인을 확정한 뒤에 별도 지시서로 처리합니다.
- 표식 파일(`~/.bamti/workarea.guard`)의 생성과 삭제 규약을 건드리지 마십시오. 검증까지 끝난 부분입니다.
- 레지스트리에 쓰지 마십시오. 작업 영역을 되돌리려고 `Explorer` 관련 키를 만지는 방법을 쓰면 안 됩니다.
- `src/menu_bar.cpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
