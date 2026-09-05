# 오버플로 아이콘 열거 측정 결과

> 조사 당시의 측정 기록입니다. 현재 코드의 설명이 아닙니다.

측정 일시: 2026-08-29 (현지 11:49)
OS: Windows 11 Pro, RtlGetVersion 10.0.26200
아키텍처: x64 (IsWow64Process2 process_machine=UNKNOWN, native=AMD64)
권한: elevated=no
bamti 상주: no (`Local\bamti.singleton` 없음)
보고서 파일: `%USERPROFILE%\.bamti\probe-tray.txt` (실행마다 덮어씀). 아래 7절은 복사본.

측정 명령: `bamti.exe --probe-tray`

오버플로 창은 열지 않았다. `Invoke`는 조회만 했고 호출하지 않았다.

`FIX-TRAY-OVERFLOW.md`가 인용한 11:35 탐침에서는 서드파티 `NotifyItemIcon` 여섯 개가 전부 오버플로에 있었고 좌표는 `(0,0,0,0)`이었다. 이번 11:49 측정에서는 그중 일부가 알림 영역으로 나와 있었다.

측정 시 알림 영역에 보이던 서드파티 `NotifyItemIcon` (5절, 실제 좌표, `IsOffscreen=no`):

1. (이름 없음)
2. KakaoTalk
3. Tailscale: Connected. Click for options.
4. Bluetooth 장치
5. Microsoft Outlook
6. Microsoft Teams

오버플로 루트(`TopLevelWindowForOverflowXamlIsland`의 자식 `DesktopWindowContentBridge`)에서만 열거된 `NotifyItemIcon` (좌표 `(0,0,0,0)`, `IsOffscreen=yes`):

1. Everything
2. 오피스키퍼

5절 ControlView 보행이 오버플로 브리지에서 본 버튼도 이 둘뿐이었다. `FindAll`이 닫힌 상태에서 보이는 버튼을 빠뜨리지는 않았다.

## 판정

| 항목 | 결과 |
|---|---|
| 오버플로 창 | 측정 전후 모두 `visible=no`. 창을 열지 않았다 |
| 닫힌 오버플로에서 열거된 `NotifyItemIcon` | 2개. 이름·순서·`RuntimeId`가 다섯 회차 동안 같음 |
| 좌표 | 전부 `rect(0,0,0,0)`. 순서를 좌표로 정할 수 없다 |
| `RuntimeId` | `[42,5179978,4,139]` (Everything), `[42,5179978,4,143]` (오피스키퍼). 5회 모두 동일 |
| 열거 순서 | 다섯 회차 모두 Everything → 오피스키퍼 |
| 회차 비용 (`GetTickCount64`) | 0, 0, 15, 16, 15 ms. 0은 타이머 해상도(약 15.6ms) 미만 |
| 기존 5절 UIA 보행 | 106노드, 156ms. 오버플로 `FindAll` 한 번은 그보다 한 자릿수 이상 작다 |
| `Invoke` 패턴 | 두 버튼 모두 `Invoke=yes`. 호출하지 않았다 |

적용 메모 (코드가 아니라 사람이 표에 맞춘 것):

- 오버플로 아이콘을 안정적인 식별자로 추적할 수 있는가. **이번 표본에서는 그렇다.** `RuntimeId`가 3초 간격 다섯 회차 동안 유지되었고, 열거 순서도 같았다. 좌표는 전부 원점이라 식별자로 쓸 수 없다. 표본은 아이콘 2개, 약 12초이다. 이름이 비어 있는 아이콘은 이번 오버플로 목록에 없었다.
- 열거 루트를 하나 더 늘렸을 때 회차 비용이 얼마나 늘어나는가. **아이콘 2개 기준 0~16ms.** 미러가 이미 쓰는 알림 영역 `FindAll`에 오버플로 브리지 루트 하나를 더하는 비용은, 이번 기계에서 기존 5절 보행(156ms)보다 훨씬 작다. `GetTickCount64`가 0을 준 회차는 1ms 미만이 아니라 타이머 틱 미만이다.
- `Invoke` 패턴이 사용 가능하다고 보고되는가. **그렇다.** 닫힌 상태에서도 두 버튼 모두 `IsInvokePatternAvailable=yes`였다. 실제로 `Invoke`를 호출하지는 않았으므로, 닫힌 오버플로 아이콘을 눌러 메뉴가 뜨는지는 이번 측정 밖이다.

미러 구현은 하지 않았다. 오버플로 아이콘을 미러에 넣을지는 이 측정 결과를 보고 다음 지시서에서 정한다.

## 원본 보고서 (7절)

## 7. 오버플로 NotifyItemIcon
island hwnd=0x350AEC class=TopLevelWindowForOverflowXamlIsland visible=no rect(4348,1977,4519,2088)
bridge hwnd=0x160CB2 class=Windows.UI.Composition.DesktopWindowContentBridge visible=no rect(4348,1977,4519,2088)
rounds=5 gap_ms=3000
형식: order  name  rect  IsOffscreen  RuntimeId  Invoke

### round 1
elapsed_ms=0 count=2
0  name="Everything"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,139]  Invoke=yes
1  name="오피스키퍼"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,143]  Invoke=yes

### round 2
elapsed_ms=0 count=2
0  name="Everything"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,139]  Invoke=yes
1  name="오피스키퍼"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,143]  Invoke=yes

### round 3
elapsed_ms=15 count=2
0  name="Everything"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,139]  Invoke=yes
1  name="오피스키퍼"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,143]  Invoke=yes

### round 4
elapsed_ms=16 count=2
0  name="Everything"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,139]  Invoke=yes
1  name="오피스키퍼"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,143]  Invoke=yes

### round 5
elapsed_ms=15 count=2
0  name="Everything"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,139]  Invoke=yes
1  name="오피스키퍼"  rect(0,0,0,0)  IsOffscreen=yes  RuntimeId=[42,5179978,4,143]  Invoke=yes

### identity
count_same=yes name_order_same=yes runtime_id_order_same=yes runtime_id_set_same=yes runtime_id_present=yes
island_visible_end=no rect(4348,1977,4519,2088)

handles_end=266
elapsed_ms=12281
