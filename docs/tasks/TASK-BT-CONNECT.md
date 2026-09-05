# TASK-BT-CONNECT — 짝지어진 블루투스 장치를 패널에서 바로 연결하고 끊는다

이 문서는 2026-09-05 시점의 작업 지시서이며, 현재 코드의 설명이 아니라 당시의 기록이다.

## 배경

블루투스 패널에서 짝지어진 장치를 누르면 Windows 설정 앱이 열린다.

```cpp
} else if (page_ == Page::kBluetooth && i >= 0 && i < static_cast<int>(bt_devices_.size())) {
  BtDevice& dev = bt_devices_[static_cast<size_t>(i)];
  if (dev.paired) {
    OpenSettingsPage(L"ms-settings:bluetooth");   // ← 여기
  } else {
    ... BluetoothAuthenticateDeviceEx ...
  }
```

`TASK-SYSTEM-PANELS.md`를 쓸 때 "짝짓기 해제 없이 연결만 거는 공개 API가 마땅치 않다"고 적고 설정 앱으로 미뤄 두었는데, 이는 잘못된 판단이었다. `bthprops`에 쓸 만한 공개 API가 있다.

## 쓸 API

```c
DWORD BluetoothEnumerateInstalledServices(HANDLE hRadio, const BLUETOOTH_DEVICE_INFO* pbtdi,
                                          DWORD* pcServiceInout, GUID* pGuidServices);

DWORD BluetoothSetServiceState(HANDLE hRadio, const BLUETOOTH_DEVICE_INFO* pbtdi,
                               const GUID* pGuidService, DWORD dwServiceFlags);
```

둘 다 문서화된 공개 함수이고 `bthprops.lib`에 있다. 이 저장소는 이미 그 라이브러리를 링크한다.

장치에 설치된 서비스를 열거해서 하나씩 `BLUETOOTH_SERVICE_ENABLE`로 켜면 연결되고, `BLUETOOTH_SERVICE_DISABLE`로 끄면 끊긴다. 오디오 장치(A2DP, 핸즈프리)에는 잘 듣는 방법이다.

**한계를 먼저 밝혀 둔다.** 이 방법이 모든 장치에 통하지는 않는다.

| 장치 | 전망 |
| --- | --- |
| 오디오(헤드폰, 이어버드, 스피커) | 잘 된다. 이 작업의 주 대상이다 |
| 키보드, 마우스 같은 HID | 서비스만 켜서는 안 붙을 수 있다. Windows가 재연결을 스스로 관리한다 |
| BLE 전용 장치 | 통하지 않는다. GATT 연결이 따로 필요하다 |

그래서 **실패했을 때 설정 앱으로 물러서는 길을 반드시 남긴다.** 지금 동작을 없애는 것이 아니라, 먼저 시도해 보고 안 되면 지금처럼 하는 것이다.

---

## 1. 연결 계층

`src/bt_devices.hpp`에 더한다.

```cpp
// 짝지어진 장치에 설치된 서비스를 모두 켜거나 끈다.
// 몇 초가 걸릴 수 있으므로 UI 스레드에서 부르면 안 된다.
// 서비스를 하나도 바꾸지 못했으면 거짓을 돌려준다.
bool SetBtDeviceConnected(const BLUETOOTH_DEVICE_INFO& info, bool connect);
```

구현 순서다.

1. `BluetoothFindFirstRadio`로 라디오 핸들을 얻는다.
2. `BluetoothEnumerateInstalledServices`를 **두 번** 부른다. 처음에는 `pGuidServices`를 `nullptr`로 주어 개수를 받고, 버퍼를 잡은 뒤 다시 부른다. 개수가 0이면 곧바로 거짓을 돌려준다.
3. 서비스마다 `BluetoothSetServiceState`를 부르고 결과를 센다. **하나라도 `ERROR_SUCCESS`면 참**이다. 일부 서비스는 실패하는 것이 정상이다.
4. 걸린 시간과 결과를 로그에 남긴다.

```cpp
Log(L"bt", L"set service state %s connect=%d ok=%d/%d took %.0f ms", name, connect ? 1 : 0, ok_n, total_n, ms);
```

서비스 GUID를 직접 고르지 마라. 장치가 실제로 설치한 것을 열거해서 그대로 쓰는 편이 종류마다 갈라 쓰는 것보다 안전하다.

버퍼는 `std::vector<GUID>`로 잡고, 개수가 32를 넘으면 32로 자른다.

## 2. 연결을 도는 자리

**UI 스레드에서 부르지 마라.** `BluetoothSetServiceState`는 장치와 실제로 통신하므로 수 초가 걸릴 수 있다.

블루투스 검색(`bt_scan`)을 붙인 것과 같은 길을 쓴다.

1. `ControlCenterContent`가 `bamti.widget/bluetooth`에 `row_id = "bt_connect"` 이벤트를 보낸다. 어느 장치인지는 `StatusEvent`에 실어야 하는데, 지금 구조에 장치를 담을 자리가 없다. **`ControlCenterHost`에 요청 콜백을 하나 두는 편이 낫다.**

```cpp
// 주소로 장치를 가리킨다. 워커가 자기 목록에서 찾아 연결하거나 끊는다.
std::function<void(std::wstring address, bool connect)> bt_connect;
```

`menu_bar.cpp`의 `ShowControlCenter`에서 `host.bt_scan_result`를 넘기는 자리 옆에 함께 넘긴다.

2. `BuiltinWidgets`는 요청을 큐에 넣고 워커를 깨운다. 워커가 자기 `EnumBtDevices()` 결과에서 그 주소를 찾아 `BLUETOOTH_DEVICE_INFO`를 얻고 `SetBtDeviceConnected`를 부른다.

```cpp
struct BtConnectReq {
  std::wstring address;
  bool connect = false;
};
std::vector<BtConnectReq> bt_connect_reqs_;
std::wstring bt_connecting_addr_;   // 진행 중인 주소, 비어 있으면 없음
uint64_t bt_list_rev_ = 0;          // 목록이 바뀌면 올린다
```

3. 끝나면 장치 목록을 다시 열거하고 `++bt_list_rev_`한다. 패널이 새 연결 상태를 받아 간다.

**같은 장치에 대한 요청이 이미 진행 중이면 무시한다.** 연달아 누르는 것을 막는다.

`ControlCenterLive`에는 가벼운 값만 싣는다.

```cpp
std::wstring bt_connecting;   // 진행 중인 장치 주소
uint64_t bt_list_rev = 0;
```

`ApplyLive`는 `bt_list_rev`가 바뀌었을 때만 짝지어진 목록을 다시 가져온다. 지금은 `RefreshPageLists`가 2초 주기로 스스로 열거하는데, 연결 직후에는 그 주기를 기다리지 말고 곧바로 반영되어야 한다.

## 3. 패널 동작

- **연결되지 않은 짝지어진 장치**를 누르면 연결을 요청한다.
- **연결된 장치**를 누르면 연결을 끊는다. 맥이 그렇게 움직인다.
- 요청이 진행 중인 장치의 행은 이름 오른쪽에 `연결 중…` 또는 `연결 끊는 중…`을 muted 색 `regular12_`로 보인다. 배터리 백분율이 놓이는 자리와 같다.
- 진행 중인 행을 다시 눌러도 아무 일도 하지 않는다.

## 4. 실패했을 때

`SetBtDeviceConnected`가 거짓을 돌려주면 **그때 설정 앱을 연다.** 지금 동작이 이 자리로 물러나는 것이다.

워커가 결과를 알려 주어야 패널이 설정 앱을 열 수 있으므로, `ControlCenterLive`에 실패를 알리는 값을 하나 더 둔다.

```cpp
uint64_t bt_connect_fail_rev = 0;   // 실패할 때마다 올린다
```

`ApplyLive`가 이 값이 바뀐 것을 보면 `OpenSettingsPage(L"ms-settings:bluetooth")`를 부른다. 실패 이유는 로그에만 남기고 패널에 글로 띄우지 않는다. 자리가 없다.

HID나 BLE 장치처럼 원래 이 방법이 통하지 않는 종류라면 매번 설정 앱이 열릴 텐데, 그것이 지금 동작과 같으므로 나빠지지 않는다.

---

## 검증

```powershell
cmake --build out/cmake-debug --config Debug
```

경고 없이 통과해야 한다.

로그로 확인하고 보고할 것이다.

- `set service state ... ok=n/m took ... ms`가 남는지, 몇 초 걸리는지
- 연결을 요청하는 동안 상단바가 멈추지 않는지. `[perf] bar full[...]`의 `max`가 평소와 같은지 보면 된다

화면과 실제 연결 확인은 사용자에게 남긴다.

### 하지 말 것

- **앱을 `taskkill`로 강제 종료하지 마라.** 빌드에서 `LNK1168`이 나면 지난번처럼 저에게 종료를 요청하라. 이 앱은 셸을 대체하므로 강제로 끝내면 태스크바가 숨겨진 채 남을 수 있다.
- **연결과 해제를 반복해서 시험하지 마라.** 사용자가 쓰고 있는 오디오 장치가 끊긴다. 코드가 맞는지는 빌드와 코드 검토로 확인하고, 실제 동작은 사용자에게 맡겨라.
- **짝짓기를 해제하지 마라.** `BluetoothRemoveDevice`는 이 작업에 쓰지 않는다. 한 번 풀면 사용자가 다시 짝지어야 한다.
- 지금 잘 도는 것을 건드리지 마라. 검색(`bt_scan`)과 트레이 아이콘 필터는 확인이 끝났다.
