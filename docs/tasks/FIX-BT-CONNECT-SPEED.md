# FIX-BT-CONNECT-SPEED — 블루투스 연결과 해제를 병렬로 처리해 시간을 줄인다

이 문서는 2026-09-05 시점의 작업 지시서이며, 현재 코드의 설명이 아니라 당시의 기록이다.

## 지금까지 온 길

`FIX-BT-CONNECT.md`의 수정으로 **재연결이 정상 동작하게 되었다.** 로그가 그것을 보여 준다.

```
21:30:04 set service source=enumerated class=0x04 listed=8 keep=3
21:30:15 set service state ... connect=0 ok=3/3 took 10813 ms

21:30:25 set service source=remembered class=0x04 listed=0 keep=3
21:30:34 set service state ... connect=1 ok=3/3 took 9060 ms
```

기억해 둔 GUID 를 쓰는 경로(`source=remembered`)가 제대로 돌았고, 처리 대상도 8개에서 3개로 줄어 해제가 27초에서 10.8초가 되었다. **이 부분은 그대로 두어라.**

## 남은 문제

각 호출에 걸린 시간이다.

| 작업 | 서비스 | 걸린 시간 |
| --- | --- | --- |
| 해제 | A2DP Sink (`110B`) | 3858 ms |
| 해제 | AVRCP (`110E`) | 3230 ms |
| 해제 | Handsfree (`111E`) | 3724 ms |
| 연결 | A2DP Sink (`110B`) | 3022 ms |
| 연결 | AVRCP (`110E`) | 3011 ms |
| 연결 | Handsfree (`111E`) | 3026 ms |

연결 쪽 세 값이 3022, 3011, 3026이다. **거의 정확히 3초씩이고 `state=0`, 곧 성공이다.** 실제 통신에 걸린 시간이라면 이렇게 고르게 나올 수 없다. `BluetoothSetServiceState`가 성공하고도 3초를 동기적으로 기다린 뒤 돌아온다는 뜻이다.

그러므로 **서비스를 줄여도 남은 개수만큼 3초씩 쌓인다.** 개수를 더 줄이는 것으로는 크게 나아지지 않는다. 3개를 2개로 줄여야 9초가 6초일 뿐이고, 그 대가로 통화(Handsfree)나 미디어 제어(AVRCP)를 잃는다.

**순서를 없애야 한다. 세 호출을 동시에 돌리면 9초가 3초가 된다.**

---

## 1. 서비스마다 스레드를 띄워 동시에 처리한다

`SetBtDeviceConnected`의 마지막 반복문을 병렬로 바꾼다.

```cpp
const DWORD flag = connect ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE;
int ok_n = 0;
for (int i = 0; i < total_n; ++i) {
  ... BluetoothSetServiceState(radio, &di, &guids[i], flag) ...   // ← 여기
}
```

**라디오 핸들을 공유하지 마라.** 각 작업이 `BluetoothFindFirstRadio`로 자기 핸들을 따로 열고, 끝나면 자기가 닫는다. 하나의 핸들을 여러 스레드가 함께 쓰면 드라이버가 어떻게 반응할지 알 수 없다.

작업 스레드는 `TrySubmitThreadpoolCallback`으로 띄운다. 이 저장소는 `BuiltinWidgets::SubmitSave`에서 이미 같은 방식을 쓰고 있으니 그 모양을 따르라. 각 작업이 자기 결과(`GUID`, `state`, 걸린 시간)를 자기 자리에 적고, 완료를 알리는 이벤트를 세운다. 본체는 `WaitForMultipleObjects`로 모두 기다린다.

- 동시에 도는 작업은 **최대 4개**로 제한한다. 서비스가 그보다 많으면 4개씩 묶어 처리한다.
- **기다리는 시간에 상한을 둔다.** 작업 하나당 3초가 정상이므로, 전체 대기는 **20초**를 넘기지 않는다. 시간이 지나면 그때까지의 결과로 판정한다. 다만 스레드풀 작업이 아직 돌고 있을 수 있으므로, 각 작업이 쓰는 자리는 작업이 끝날 때까지 살아 있어야 한다. **결과 버퍼를 스택에 두고 함수가 먼저 돌아가면 안 된다.** 이 점을 반드시 지켜라.
- 로그는 지금 형식을 유지한다. 다만 순서가 뒤섞이므로 각 줄에 어느 서비스인지가 이미 GUID로 적혀 있어 문제없다. 완료된 순서대로 적어도 된다.

## 2. 병렬이 듣지 않으면 되돌린다

동시에 돌렸을 때 드라이버가 오류를 돌려줄 수 있다. **그 경우를 반드시 확인하고 보고하라.**

- 병렬로 처리한 결과 `ok_n`이 0이면, **한 번만** 순차로 다시 시도한다. 그때는 지금 코드와 같은 길이다.
- 순차 재시도가 돌았다는 사실을 로그에 남긴다.

```cpp
Log(L"bt", L"parallel failed ok=0/%d; retrying serially", total_n);
```

이렇게 하면 병렬이 통하는 환경에서는 3초로 끝나고, 통하지 않는 환경에서도 지금과 같은 결과를 얻는다.

## 3. 곁들여 고칠 것

`ControlCenterContent::Invoke`의 중복 차단에 60초 조건이 빠져 있다.

```cpp
if (!bt_connecting_addr_.empty() && bt_connecting_addr_ == dev.address) {
  break;
}
```

`RenderBluetoothPage`는 `GetTickCount64() - bt_connecting_since_ < 60000`을 함께 보는데 이 자리에는 그 조건이 없다. 그래서 워커가 응답하지 않으면 진행 표시는 사라지는데 클릭은 계속 막힌다. 같은 조건을 넣어 맞춰라.

---

## 검증

```powershell
cmake --build out/cmake-debug --config Debug
```

경고 없이 통과해야 한다.

### 보고할 것

- 병렬로 바꾼 뒤 `set service state ... took` 이 몇 밀리초인지. 3000 대로 떨어지면 성공이다.
- 각 서비스의 `state` 가 여전히 0인지. 병렬 때문에 실패하는 것이 있으면 그것이 무엇인지.
- 순차 재시도 경로가 돌았는지.

### 하지 말 것

- **연결과 해제를 반복해서 시험하지 마라.** 사용자가 실제로 쓰는 오디오 장치다. 코드가 맞는지는 빌드와 검토로 확인하고, 실제 동작은 사용자에게 맡겨라.
- **기억해 둔 GUID 를 쓰는 경로와 표준 서비스로 물러서는 경로를 건드리지 마라.** 방금 확인이 끝났다.
- **처리 대상을 더 줄이지 마라.** 3개에서 더 줄이면 통화나 미디어 제어를 잃는다. 이 작업의 목표는 개수가 아니라 순서를 없애는 것이다.
- **앱을 `taskkill` 로 강제 종료하지 마라.** `LNK1168` 이 나면 종료를 요청하라.
