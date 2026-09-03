# 작업 지시서 32: 셸 준비 판정이 이른지 계측한다

**이 지시서는 우선순위가 가장 낮습니다.** 지시서 30과 31을 먼저 처리하십시오.

**증상이 관측되지 않은 사안입니다.** 아래에 적는 것은 로그에서 눈에 띈 정황일 뿐이고, 실제 손실은 이번 부팅에서 하나도 없었습니다. 그래서 이번 작업은 **계측과 조사만** 수행하며, 동작을 바꾸지 않습니다.

---

## 1. 무엇이 눈에 띄었는가

2026-09-03 13:48:45 재부팅의 기동 구간입니다.

```
13:49:04.065  [host] start data=C:\Users\KIBEOMKWON\.bamti log=...
13:49:04.087  [tray] intercept priority acquired ms=0
13:49:04.646  [tray] intercept z-order lost first=0x100DC spy=0x80070 count=1
13:49:04.725  [host] shell wait ms=610 found=1 tray=1 progman=1 notify=1
13:49:04.745  [tray] intercept z-order lost first=0x100DC spy=0x80070 count=2
13:49:13.494  [bar] ready hwnd=000000000001016E taskbar_hidden=1
13:49:13.595  [tray] intercept shell restart
13:49:13.610  [tray] intercept priority acquired ms=0
```

두 가지가 직전 부팅과 다릅니다.

1. **`shell wait`가 610ms만에 끝났습니다.** 09:05 부팅에서는 5484ms를 기다렸습니다.
2. **`priority acquired`가 두 번 찍혔습니다.** 09:05 부팅에서는 한 번이었습니다.

두 번째 `priority acquired`(13:49:13.610) 직전에 `intercept shell restart`(13:49:13.595)가 있습니다. 즉 `shell wait`가 셸이 준비되었다고 판정한 뒤 **9초쯤 지나서 explorer가 셸을 다시 만들었고**, 트레이 가로채기가 `TaskbarCreated`를 받아 우선순위를 다시 잡았습니다.

---

## 2. 이것이 문제인지 아직 모릅니다

**회복 경로가 의도대로 동작했습니다.** `src/host.cpp` 209~211행의 주석이 바로 이 상황을 예상하고 적혀 있습니다.

```cpp
// 작업 스케줄러로 로그온 직후에 뜨면 explorer의 셸 창이 아직 없을 수 있다.
// 최대 30초 동안 200ms 간격으로 기다린다. 시간이 다 되어도 그냥 진행한다.
// 뒤늦게 셸이 뜨는 경우는 TaskbarCreated 처리가 회복시킨다.
```

그리고 결과도 좋았습니다. 기동 3분 뒤의 로스터에 서드파티 손실이 하나도 없었습니다.

```
13:52:06.789 [tray] intercept roster n=8 tips="배터리 상태…|스피커: 0%|하드웨어 안전하게 제거…|
                     Bluetooth 장치|오피스키퍼|Everything|Tailscale: Connected…|KakaoTalk"
13:52:06.789 [tray] uia roster n=12 tips="숨겨진 아이콘 표시|KakaoTalk|Tailscale…|Bluetooth 장치|
                     하드웨어 안전하게 제거…|…"
```

UIA가 본 서드파티가 전부 가로채기에 들어왔고, 숨김 영역에 있어 UIA가 보지 못한 오피스키퍼와 Everything까지 받았습니다.

**따라서 지금은 고칠 근거가 없습니다.** 다만 셸이 재시작하는 9초 동안 등록한 앱이 있었다면 놓쳤을 수 있고, 그 구간에서 무슨 일이 있었는지를 지금 로그로는 알 수 없습니다. 이번 작업의 목적은 **다음 부팅에서 판단할 재료를 남기는 것**입니다.

---

## 3. 무엇을 남길 것인가

### 3-1. 셸 재시작까지의 간격

`src/tray_intercept.cpp`의 `intercept shell restart` 로그에 **기동 후 경과 시간**을 붙이십시오.

```
[tray] intercept shell restart elapsed_ms=9508
```

`FIX-BOOT-COLD-START-2.md` 2절에서 넣은 `started_at_` 멤버가 이미 있으므로 그것을 쓰면 됩니다. 새 멤버를 만들지 마십시오.

`intercept shell restart suppressed` 쪽에도 같은 값을 붙이십시오. 이번 부팅에서 13:49:14.847에 한 번 나왔는데, 억제된 이유와 시점을 함께 봐야 판단이 됩니다.

### 3-2. 판정 시점의 셸 상태

`src/host.cpp` 230행의 `shell wait` 로그에 값을 하나 더합니다. 판정이 끝난 시점에 explorer 프로세스가 얼마나 오래 살아 있었는지입니다.

```
[host] shell wait ms=610 found=1 tray=1 progman=1 notify=1 explorer_age_ms=...
```

`Shell_TrayWnd`의 소유 프로세스를 `GetWindowThreadProcessId`로 얻고, `OpenProcess`와 `GetProcessTimes`로 생성 시각을 읽어 현재 시각과의 차이를 계산하십시오. 실패하면 `-1`을 남기고 넘어가면 됩니다.

이 값이 작으면 갓 만들어진 explorer를 붙잡은 것이고, 크면 이미 오래 돌던 explorer가 나중에 셸을 다시 만든 것입니다. **둘은 원인이 다르므로 대응도 달라집니다.**

### 3-3. 재시작 구간의 트레이 등록 시도

`intercept shell restart`가 오기 전까지 가로채기가 받은 `NIM_ADD` 개수를 로그에 남기십시오. 재시작 시점에 한 줄이면 충분합니다.

```
[tray] intercept shell restart elapsed_ms=9508 adds_before=4
```

이 값이 0이면 재시작 전 구간에서 잃을 것이 애초에 없었다는 뜻이고, 크면 그 등록들이 재시작 뒤에도 살아남았는지를 확인해야 합니다.

---

## 4. 판정 기준

재부팅한 뒤 다음 줄들을 완료 보고에 그대로 옮겨 적으십시오.

1. `[host] shell wait` 줄
2. `[tray] intercept shell restart` 줄과 `suppressed` 줄
3. 기동 3분 뒤의 `[tray] intercept roster`와 `[tray] uia roster` 줄

**이번 작업에는 통과와 실패를 가르는 기준이 없습니다.** 값을 모으는 것이 목적입니다. 다만 세 가지는 확인해 주십시오.

1. 기존 동작이 그대로인지: `shell wait`의 `found`, `tray`, `progman`, `notify` 값이 이전과 같은 의미로 나오는지 봅니다.
2. 서드파티 손실이 여전히 0인지: 두 로스터를 `RESEARCH-TRAY-REREGISTER.md` 6-1절의 방법으로 대조합니다.
3. `z-order lost` 횟수가 6회 수준을 유지하는지 봅니다.

---

## 5. 하지 말아야 할 것

- **`WaitForShell`의 판정 조건을 바꾸지 마십시오.** `ShellReady()`에 조건을 더하거나 대기 시간을 늘리는 방식은 이번 작업의 범위가 아닙니다. 증상이 관측되지 않은 상태에서 판정을 늦추면 기동만 느려집니다.
- **`intercept shell restart`의 회복 동작을 바꾸지 마십시오.** 이번 부팅에서 의도대로 동작한 경로입니다.
- 셸 재시작을 기다리는 새 타이머나 새 스레드를 만들지 마십시오.
- 자동 시작을 늦춰서 explorer와의 경쟁을 피하려 하지 마십시오. 빨라진 기동은 원하는 결과입니다.
- `FIX-BOOT-COLD-START-2.md` 2절에서 넣은 부팅 구간 z-order 선점을 되돌리지 마십시오.
- 빌드는 `D:\repos\bamti\build` 트리에 Release 구성으로 하십시오. 실행 중이면 링크가 실패하므로, 그때는 컴파일만 확인하고 그 사실을 보고해 주십시오.
- 검증 절차에 레지스트리 쓰기 명령을 넣지 마십시오. 조회만 하십시오.
- 재부팅이 필요한 검증은 수행하지 마십시오. 사용자에게 부탁할 항목입니다.
