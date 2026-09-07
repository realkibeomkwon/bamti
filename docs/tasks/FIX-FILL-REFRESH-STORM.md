# 작업 지시서: 보충 갱신이 초당 한 번씩 강제되는 문제를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/tray_mirror.cpp` 와 `src/tray_mirror.hpp` 두 개입니다.
`docs/tasks/FIX-STALE-TRAY-FILL.md` 의 구현을 검수하다 나온 결함이므로 그 지시서를 함께 읽으십시오.

---

## 1. 증상

`FIX-STALE-TRAY-FILL.md` 는 UIA 보충 열거를 **5초 주기**로 제한하도록 지시했습니다. 그런데 실제로는 **1.4초에 한 번꼴로** 열거가 강제되고 있습니다.

`~/.bamti/bamti.log` 의 18시 20분부터 30분까지 10분 구간을 세면 이렇습니다.

| 로그 줄 | 횟수 |
| --- | --- |
| `[tray] fill refresh intercept lost` | **437** |
| 그 구간의 전체 로그 줄 | 528 |

로그의 83퍼센트가 이 한 줄입니다.

---

## 2. 무엇이 원인인가 (로그로 확정)

`FIX-STALE-TRAY-FILL.md` 에 없던 `intercept_cover_tips_` 최적화가 원인입니다. 이 집합은 가로채기가 들고 있는 항목을 **툴팁 문자열**로 기억해 두었다가, 다음 라운드에 그 문자열이 사라지면 앱이 종료된 것으로 보고 즉시 재열거합니다.

그런데 툴팁이 실시간으로 바뀌는 트레이 아이콘이 있습니다.

```
18:30:15.027 [tray] fill refresh intercept lost tip="CPU 9% 메모리 41% 디스크 4% 네트워크 0%"
18:30:16.054 [tray] fill refresh intercept lost tip="CPU 12% 메모리 41% 디스크 3% 네트워크 0%"
18:30:18.072 [tray] fill refresh intercept lost tip="CPU 12% 메모리 41% 디스크 3% 네트워크 0%"
```

작업 관리자입니다. 사용률이 바뀔 때마다 툴팁 문자열이 달라지므로, **매번 "가로채기가 사라졌다"고 오판합니다.** 앱은 멀쩡히 살아 있습니다.

툴팁이 동적으로 바뀌는 아이콘은 드물지 않습니다. 네트워크 상태, 배터리, 백신, 동기화 도구가 모두 그렇습니다. 작업 관리자를 끄더라도 근본 결함은 남습니다.

### 실제 피해는 무엇인가

CPU 점유율은 문제가 아닙니다. 20초 동안 재 보니 코어 하나 기준 **1.9퍼센트**였습니다. 과장하지 마십시오.

실제로 남는 피해는 셋입니다.

1. **트레이 클릭 반응이 밀립니다.** 워커 스레드는 `DoRound` 와 `DrainInvoke` 를 번갈아 처리합니다. UIA 열거가 도는 동안 클릭 처리가 대기합니다. 열거 시간은 로그에 이렇게 남아 있습니다.
   ```
   18:14:47.767 [tray] fill enum slow ms=891 count=10
   18:29:29.610 [tray] fill enum slow ms=656 count=11
   ```
   **최악의 경우 클릭이 0.9초 밀립니다.**
2. **로그가 오염되어 진단이 어려워집니다.** 위에서 보았듯 전체의 83퍼센트를 이 한 줄이 차지합니다.
3. UIA 열거는 explorer 를 상대로 하는 프로세스 간 COM 호출이므로 explorer 쪽에도 부하가 갑니다.

---

## 3. 무엇을 고치는가

**`intercept_cover_tips_` 를 없애고 5초 주기만 남기십시오.**

- `src/tray_mirror.hpp` 에서 `intercept_cover_tips_` 멤버를 지웁니다.
- `src/tray_mirror.cpp` 의 `DoRound` 에서 이 집합을 채우고 비교하는 블록을 통째로 지웁니다. `Start`, `DropAll`, `WorkerLoop` 의 초기화 자리에서도 지웁니다.
- `fill refresh intercept lost` 로그 줄도 함께 지웁니다.
- `kFillRefreshMs` 5초 주기와 `TakeStartupFillPulse` 펄스는 **그대로 둡니다.**

이렇게 하면 앱을 종료했을 때 보충 항목이 사라지기까지 최대 5초가 걸립니다. 그 정도면 충분합니다.

### 즉시성을 굳이 살리려면

5초가 길다고 판단되면 **툴팁이 아니라 `TrayIconInfo::key` 로 추적하십시오.** 이 값은 소유 창과 uid 로 만들어지므로 툴팁이 바뀌어도 흔들리지 않습니다. 다만 그 경우에도 강제 갱신에 **최소 간격 1초**를 두십시오. 작업 관리자는 uid 를 바꿔 가며 아이콘을 열세 개까지 다시 등록하므로, key 기반으로 바꿔도 폭주할 여지가 남습니다.

```
18:14:42.384 [tray] intercept item tip="작업 관리자" exe=Taskmgr.exe hwnd=0xB9002E uid=4294967295 ...
18:14:42.394 [tray] intercept item tip="작업 관리자" exe=Taskmgr.exe hwnd=0xB9002E uid=4294967294 ...
   (uid 를 하나씩 줄이며 열세 개)
```

**둘 중 어느 쪽을 골랐는지 보고하십시오.** 단순한 쪽을 권합니다.

---

## 4. 함께 처리할 것

### 4-1. 지시서 범위를 벗어난 수정이 있었습니다

`FIX-DOCK-TRAY-ACTIVATE.md` 6절은 `src/dock.cpp` 2477행의 우클릭 메뉴 경로를 건드리지 말라고 명시했는데, `kOpenCommand` 가 `kNewWindowCommand` 에서 분리되어 `RevealDockApp` 을 쓰도록 바뀌었습니다.

**이 변경은 되돌리지 마십시오.** 동작이 일관되고 이미 검증되었습니다. 다만 다음부터는 "건드리지 말 것" 목록을 지키고, 넘어야 할 이유가 있으면 먼저 보고하십시오.

### 4-2. 검증되지 않은 항목이 남았습니다

`FIX-DOCK-TRAY-ACTIVATE.md` 5절 1번은 **독 아이콘을 클릭**했을 때 `route=tray` 가 나오는지 보라고 했습니다. 로그에는 우클릭 메뉴의 "열기" 로만 확인되어 있습니다.

```
18:16:05.152 [dock] menu cmd=7 open name=KakaoTalk windows=0 route=tray
```

독 아이콘 클릭 경로를 다시 확인하십시오. 카카오톡 창을 모두 닫아 트레이로 보낸 뒤(**완전 종료가 아닙니다**) 독 아이콘을 왼쪽 클릭하면 `route=tray` 가 나와야 합니다.

### 4-3. 더블클릭 고정이 맞는지 확인하십시오

`InvokeByExe` 는 `pending_dblclk_ = true` 로 고정되어 있습니다. 카카오톡에서는 통했지만, 좌클릭 한 번으로 창을 여는 앱에서는 더블클릭이 창을 열었다 닫을 수 있습니다.

트레이 아이콘을 쓰는 다른 앱(예: Tailscale)으로 시험해 보고, 문제가 있으면 **여기서 멈추고 보고하십시오.** 스스로 판정 규칙을 만들지 마십시오.

---

## 5. 검증

측정값으로 판정하십시오.

1. 빌드 후 실행하고 **작업 관리자를 켠 채로** 5분 이상 둡니다.
2. 로그에서 세십시오.
   - `fill refresh intercept lost` 는 **0회**여야 합니다(줄 자체를 지웠으므로).
   - `fill enum slow` 가 쌓이지 않아야 합니다.
   - 5분 구간에서 UIA 열거는 대략 60회 안팎이어야 합니다. 그보다 많으면 주기가 듣지 않는 것입니다.
3. 카카오톡을 완전히 종료하고 `[tray] fill drop` 이 **5초 안에** 나오는지 확인합니다.
4. 4-2 의 독 클릭 경로를 확인합니다.
5. 화면을 눈으로 봐야 하는 항목은 사용자에게 부탁하십시오. 사용자 화면에 입력을 합성하지 마십시오.

---

## 6. 건드리지 말 것

- `kFillRefreshMs` 5초 주기와 `TakeStartupFillPulse` 펄스
- `InterceptOwns` 와 `SameFillItem` 의 대조 규칙
- `InvokeByExe` 의 소유 PID 대조 방식
- `Dock::RevealDockApp` 의 경로 순서
- `NeedProcessRecheck` 와 `InvalidateLiveProcessCache`
- `src/menu_bar.cpp` 전체
