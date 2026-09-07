# 작업 지시서: 답을 얻은 탐침을 걷어내고 운영에 필요한 기록만 남긴다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-WORKAREA-PROBE.md` 와 `TASK-WIN-REPEAT-PROBE.md` 가 넣은 탐침이 제 몫을 다했습니다(커밋 `6ac938a`, `7f0986b`). **원인 규명이 끝난 것만 걷어내고, 앞으로도 이상을 알려 줄 기록은 남깁니다.** 건드리는 파일은 `src/menu_bar.cpp` 하나입니다.

**이 작업은 동작을 바꾸지 않습니다.** 판정 논리에 손을 대면 안 됩니다.

---

## 1. 무엇을 남기고 무엇을 지우는가

기준은 하나입니다. **이미 답이 나온 질문에 답하려고 넣은 것은 지우고, 앞으로 이상이 생겼을 때 그것을 알려 줄 것은 남깁니다.**

| 대상 | 처분 | 까닭 |
| --- | --- | --- |
| `ReadWorkAreaTop` | **남깁니다** | 탐침이 아니라 `applied` 판정의 일부입니다. 지우면 동작이 바뀝니다 |
| `workarea set ...` 로그 | **남깁니다** | 강제가 통했는지를 남기는 유일한 기록이고, 작업 영역이 목표에 못 미칠 때에만 찍히므로 잦지 않습니다 |
| `workarea top=... retry=... spi_fail=...` 로그 | **남깁니다** | 회차 요약입니다 |
| `workarea retry n=`, `workarea retry giveup`, `workarea spi giveup` | **남깁니다** | 이상 상황을 알리는 신호입니다. 평시에는 찍히지 않습니다 |
| `LogWorkAreaPoint` 와 `workarea at=` 로그 | **지웁니다** | "누가 작업 영역을 되돌리는가"를 재려고 넣었고 답이 나왔습니다 |
| `workarea settingchange spi=1` 로그 | **지웁니다** | 같은 질문에 딸린 기록입니다. 설정 변경은 자주 오므로 로그만 쌓입니다 |
| `g_key_repeat_count` 계열과 `g_last_down_vk` | **지웁니다** | "자동 반복이 훅에 닿는가"에 답이 나왔습니다. 타자를 칠 때마다 셈이 오릅니다 |
| `g_win_repeat_count` | **지웁니다** | 위와 같은 이유입니다. Win 반복이 닿는다는 것은 확인되었습니다 |
| `g_win_repeat_combo` | **남깁니다** | 조합 기억이 살아 있는 채로 Win 반복이 들어온 횟수입니다. **이것이 오르면 결함 경로를 실제로 밟았다는 뜻**이므로 앞으로도 알아야 합니다 |

## 2. 작업 영역 쪽에서 지울 것

### 2.1 `LogWorkAreaPoint` 정의

익명 이름공간의 `LogWorkAreaPoint` 함수 전체를 지우십시오. **바로 위의 `ReadWorkAreaTop` 은 그대로 두십시오.**

### 2.2 호출 네 곳

- `MenuBar::Layout()` 의 `LogWorkAreaPoint(hwnd_, L"layout-pre");` 와 `LogWorkAreaPoint(hwnd_, L"layout-post");`
- `kAppBarCallback` 의 `case ABN_POSCHANGED:` 안의 `LogWorkAreaPoint(hwnd_, L"appbar-poschanged");`
- `WM_SETTINGCHANGE` 갈래의 `LogWorkAreaPoint(hwnd_, L"settingchange");`

`Layout()` 쪽은 특히 중요합니다. 이 두 줄은 `Layout()` 이 불릴 때마다 `GetMonitorInfoW` 와 `SPI_GETWORKAREA` 를 부르므로, 로그를 찍지 않는 회차에도 비용이 듭니다.

### 2.3 설정 변경 로그

`WM_SETTINGCHANGE` 갈래에서 다음 세 줄을 지우십시오.

```cpp
      if (msg == WM_SETTINGCHANGE && wparam == SPI_SETWORKAREA) {
        Log(L"bar", L"workarea settingchange spi=1");
      }
```

지운 뒤 그 자리는 이렇게 남아야 합니다.

```cpp
      layout_.SetDpi(Dpi());
      clock_.SetDpi(Dpi());
      ReserveWorkArea();
      Present();
      return 0;
```

## 3. 키보드 쪽에서 정리할 것

### 3.1 전역 변수

일곱 개 가운데 둘만 남기십시오.

```cpp
UINT g_win_repeat_combo = 0;  // 조합 기억이 살아 있는 채로 들어온 Win 반복 누름
UINT g_win_repeat_combo_logged = 0;
```

`g_key_repeat_count`, `g_win_repeat_count`, `g_key_repeat_logged`, `g_win_repeat_logged`, `g_last_down_vk` 는 지웁니다.

### 3.2 훅의 일반 반복 셈

`LowLevelKeyboardProc` 에서 `ExpireStaleWin();` 바로 앞의 다음 블록을 통째로 지우십시오.

```cpp
  if (down) {
    if (g_last_down_vk == vk) {
      ++g_key_repeat_count;
    }
    g_last_down_vk = vk;
  } else if (up && g_last_down_vk == vk) {
    g_last_down_vk = 0;
  }
```

### 3.3 Win 누름 갈래

`else` 갈래에서 `g_win_repeat_count` 만 빼고 나머지는 그대로 두십시오.

```cpp
      } else if (g_win_combo) {
        ++g_win_repeat_combo;
      }
      g_win_held = true;
      return 1;
```

**`if (!g_win_held)` 안의 초기화는 절대 건드리지 마십시오.** 그것이 `FIX-WORKAREA-SETTLE` 이전에 고친 결함의 수정 본체입니다.

### 3.4 타이머의 기록

`kClockTimerId` 갈래의 `key repeat` 로그를 다음으로 바꾸십시오.

```cpp
        if (g_win_repeat_combo != g_win_repeat_combo_logged) {
          g_win_repeat_combo_logged = g_win_repeat_combo;
          Log(L"bar", L"win combo repeat count=%u", g_win_repeat_combo);
        }
```

이 줄은 평소에 한 번도 찍히지 않아야 정상입니다. 찍힌다면 조합키를 쓴 뒤 Win 을 계속 누르고 있는 상황에서 반복이 훅에 닿았다는 뜻이므로, 그때 다시 살펴보면 됩니다.

### 3.5 훅 해제

`MenuBar::RemoveWinHook()` 의 `g_last_down_vk = 0;` 을 지우십시오. 셈한 값(`g_win_repeat_combo`)은 되돌리지 마십시오. 누적값이라야 나중에 알아볼 수 있습니다.

## 4. 검증

1. Release 클린 빌드가 경고 없이 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

**쓰지 않는 변수가 남으면 경고가 납니다.** 경고가 하나라도 나오면 지우다 만 것이 있다는 뜻이므로 그대로 두지 말고 마저 정리하십시오.

2. 빌드하려고 실행 중인 `bamti.exe` 를 종료했다면(`LNK1104`), 끝난 뒤 반드시 WMI 로 다시 띄워 놓으십시오. `Start-Process` 로 띄우면 세션이 끝날 때 함께 죽습니다.
3. `LowLevelKeyboardProc` 안에 `Log` 호출이 하나도 없어야 합니다. 눈으로 확인하고 보고하십시오.
4. 새 빌드를 실행하고 다음 네 가지를 판정해서 각각 참인지 거짓인지 적으십시오.

| 확인할 것 | 판정 방법 |
| --- | --- |
| 정리 뒤에도 자리 잡기가 1초 안에 끝난다 | 첫 `workarea` 줄과 `top` 이 `want` 에 닿은 첫 줄의 시각 차이를 재십시오. 직전 실행은 0.43초였습니다 |
| `workarea at=` 과 `workarea settingchange` 줄이 사라졌다 | 새 실행 구간의 로그에서 두 문자열을 찾아보십시오 |
| `workarea set` 과 `workarea top=` 은 그대로 찍힌다 | 같은 구간에서 두 줄이 남아 있는지 보십시오 |
| 작업 영역이 상단바 아래로 잡힌다 | `[System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea` 의 `Y` 를 재십시오 |

5. Win 단독 누름으로 시작 메뉴가 열리는지, `Win+E` 가 탐색기를 여는지 **사용자에게 확인을 부탁하십시오.** 이 환경에서는 키보드 입력을 합성할 수 없습니다.
6. 확인하지 못한 항목은 확인하지 못했다고 적으십시오.

## 5. 하지 말 것

- **`ReadWorkAreaTop` 을 지우지 마십시오.** 탐침처럼 보이지만 `applied` 판정에 쓰이는 본체입니다.
- **판정 논리를 건드리지 마십시오.** `applied`, `work_area_retry_`, `work_area_spi_fail_`, `work_area_want_` 의 계산과 리셋 조건은 그대로입니다. 이 작업은 기록만 줄이는 것입니다.
- `kWorkAreaRecheckTimerId` 의 이름을 바꾸지 마십시오. 역할이 재시도로 바뀌어 이름과 어긋나지만, 이번 범위 밖입니다. 그것만 따로 정리할 때 함께 처리합니다.
- `workarea guard released` 나 `win stale expired` 처럼 이전부터 있던 로그는 건드리지 마십시오.
- Win 누름 갈래의 `if (!g_win_held)` 안쪽을 건드리지 마십시오.
- `MenuBar::Create()` 의 시작 순서를 바꾸지 마십시오.
- `src/menu_bar.cpp` 외의 파일을 건드리지 마십시오.
- 이 지시서에 적히지 않은 정리나 개선을 함께 넣지 마십시오.
