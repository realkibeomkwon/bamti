# 작업 지시서: 작업 영역 타이머의 이름을 역할에 맞춘다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

이름만 바꾸는 작업입니다. **동작이 바뀌면 안 됩니다.** 건드리는 파일은 `src/menu_bar.cpp` 하나입니다.

---

## 1. 왜 바꾸는가

`kWorkAreaRecheckTimerId` 는 `TASK-WORKAREA-PROBE.md` 에서 **값을 다시 재어 로그로 남기는** 단발성 타이머로 태어났습니다. 그 뒤 `FIX-WORKAREA-SETTLE.md` 에서 역할이 **작업 영역을 다시 잡는 재시도**로 바뀌었는데 이름은 그대로 남았습니다. 지금은 같은 기능을 두 이름으로 부르고 있습니다.

- 상수 이름은 `Recheck`
- 짝이 되는 상수는 `kWorkAreaRetryMs` 와 `kWorkAreaRetryMax`
- 남기는 로그도 `workarea retry n=` 과 `workarea retry giveup`
- 세는 멤버도 `work_area_retry_`

**이름 하나만 옛 역할에 묶여 있으므로 그것을 맞춥니다.**

## 2. 바꿀 것

`kWorkAreaRecheckTimerId` 를 **`kWorkAreaRetryTimerId`** 로 바꾸십시오. 여섯 자리입니다.

| 위치 | 지금 |
| --- | --- |
| 상수 선언 | `constexpr UINT_PTR kWorkAreaRecheckTimerId = 10;` |
| `WM_TIMER` 갈래의 판정 | `if (wparam == kWorkAreaRecheckTimerId) {` |
| 같은 갈래의 해제 | `KillTimer(hwnd_, kWorkAreaRecheckTimerId);` |
| `WM_ENDSESSION` 의 해제 | `KillTimer(hwnd_, kWorkAreaRecheckTimerId);` |
| `WM_DESTROY` 의 해제 | `KillTimer(hwnd_, kWorkAreaRecheckTimerId);` |
| `ReserveWorkArea()` 의 예약 | `SetTimer(hwnd_, kWorkAreaRetryMs, ...)` 에 넘기는 식별자 |

**바꾸는 것은 이름뿐입니다.** 타이머 식별자 값 `10` 도, 간격도, 예약과 해제의 위치도 그대로 두십시오.

선언 자리도 옮기지 마십시오. 지금 `kCtrlPollTimerId` 다음에 있고 그 아래에 `kWorkAreaRetryMs` 와 `kWorkAreaRetryMax` 가 이어지므로, 이름만 바꾸면 세 줄이 자연스럽게 한 묶음으로 읽힙니다.

## 3. 검증

1. `kWorkAreaRecheckTimerId` 가 저장소 어디에도 남아 있지 않아야 합니다. 검색해서 결과가 없음을 확인하고 보고하십시오.
2. `kWorkAreaRetryTimerId` 가 정확히 여섯 자리에 나와야 합니다. 세어서 보고하십시오.
3. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

4. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
5. 새 빌드를 실행하고 시작 직후 로그에서 `workarea top=` 이 `want` 에 닿는지, 그때까지 걸린 시간이 1초 안쪽인지 확인하십시오. 직전 실행은 0.83초였습니다.

## 4. 하지 말 것

- 타이머 식별자 값 `10` 을 바꾸지 마십시오.
- 다른 타이머 상수의 이름이나 값을 함께 손대지 마십시오.
- `work_area_retry_`, `work_area_spi_fail_`, `work_area_want_` 와 그 판정 논리를 건드리지 마십시오.
- 로그 문구를 바꾸지 마십시오.
- `src/menu_bar.cpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
