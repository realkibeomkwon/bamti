# TASK-BT-SCAN — 블루투스 패널에 "장치 추가" 줄을 넣는다

이 문서는 2026-09-05 시점의 작업 지시서이며, 현재 코드의 설명이 아니라 당시의 기록이다.

## 배경

블루투스 패널은 지금 짝지어졌거나 기억된 장치만 보여 준다. `bt_devices.cpp`의 검색 조건이 그렇게 되어 있다.

```cpp
search.fReturnUnknown = FALSE;
search.fIssueInquiry  = FALSE;
```

Wi-Fi 패널은 "알려진 네트워크"와 "다른 네트워크"를 나누어 보여 주는데 블루투스에는 뒤쪽에 해당하는 것이 없다. 사용자가 그 대칭을 요청했다.

**다만 패널을 열 때마다 자동으로 검색하지는 않는다.** 블루투스 검색(inquiry)은 2.4GHz 대역을 점유해서 연결된 오디오 장치의 소리를 끊고, 쓸 만한 결과를 얻는 데 5초 안팎이 걸린다. 그래서 검색은 **사용자가 "장치 추가…"를 누른 순간에만** 돈다.

## 목표 화면

검색하기 전이다. 지금과 같고 줄 하나만 늘어난다.

```
┌────────────────────────────────────┐
│ Bluetooth                    (◯━)  │
│ ──────────────────────────────────  │
│ (●) 기범의 AirPods Pro              │
│ (◌) BT5.0 KB                       │
│ ──────────────────────────────────  │
│ 장치 추가…                          │   ← 새로 넣는 줄
│ Bluetooth 설정…                     │
└────────────────────────────────────┘
```

검색하는 동안이다.

```
│ ──────────────────────────────────  │
│ 다른 장치                           │   ← 섹션 헤더
│ 검색 중…                            │   ← 결과가 아직 없을 때
│ ──────────────────────────────────  │
│ 검색 중지                           │   ← "장치 추가…" 자리가 바뀐다
│ Bluetooth 설정…                     │
```

결과가 나온 뒤다.

```
│ ──────────────────────────────────  │
│ 내 장치                             │   ← 다른 장치 구역이 생길 때만 붙인다
│ (●) 기범의 AirPods Pro              │
│ (◌) BT5.0 KB                       │
│ 다른 장치                           │
│ (◌) Galaxy Buds3 Pro               │
│ (◌) 알 수 없는 장치                 │
│ ──────────────────────────────────  │
│ 다시 검색…                          │
│ Bluetooth 설정…                     │
└────────────────────────────────────┘
```

Wi-Fi 패널의 두 구역과 같은 모양이다. 섹션 헤더는 `panel::DrawSectionHeader`를 그대로 쓴다.

---

## 1. 검색 계층

`src/bt_devices.hpp`에 검색 함수를 더한다.

```cpp
// 주변을 실제로 훑는다. 5초 안팎이 걸리므로 UI 스레드에서 부르면 안 된다.
// 이미 짝지어졌거나 기억된 장치는 결과에서 뺀다.
std::vector<BtDeviceInfo> ScanBtDevices();
```

구현은 `EnumBtDevices`와 같은 뼈대를 쓰되 검색 조건만 바꾼다.

```cpp
search.fReturnAuthenticated = FALSE;
search.fReturnRemembered    = FALSE;
search.fReturnUnknown       = TRUE;
search.fReturnConnected     = FALSE;
search.fIssueInquiry        = TRUE;
search.cTimeoutMultiplier   = 4;   // 1당 1.28초이므로 약 5.1초
```

- 이름이 비어 있는 장치는 "알 수 없는 장치"로 보인다. 주소는 이미 `BtDeviceInfo::address`에 담기므로 그대로 둔다.
- 결과가 최대 8개를 넘으면 자른다(`kBtScanMax = 8`).
- 이름 오름차순으로 안정되게 정렬한다. 이름이 없는 것은 뒤로 보낸다.
- 걸린 시간을 `Log(L"bt", L"scan took %.0f ms n=%d", ...)`로 남긴다.

## 2. 검색을 도는 자리

**`ControlCenterContent`에서 직접 부르지 마라.** `Invoke`는 UI 스레드에서 돌고, 5초를 막으면 상단바 전체가 멈춘다.

`BuiltinWidgets`의 워커에 맡긴다. 볼륨 장치 재획득(`volume_device`)을 붙인 것과 같은 방식이다.

1. `ControlCenterContent`가 `host_.dispatch`로 `bamti.widget/bluetooth`에 `row_id = "bt_scan"` 이벤트를 보낸다. `on`이 참이면 시작, 거짓이면 중지 요청이다.
2. `BuiltinWidgets::OnEvent`가 그것을 받아 `pending_bt_scan_`을 세우고 워커를 깨운다.
3. 워커가 `ScanBtDevices()`를 부르고, 결과를 `bt_scan_result_`에 넣고 `++bt_scan_rev_`한다.
4. 검색이 도는 동안 `bt_scanning_`을 참으로 둔다.

`BuiltinWidgets`에 아래를 더한다. 모두 `mu_`로 지킨다.

```cpp
bool pending_bt_scan_ = false;
bool bt_scanning_ = false;
uint64_t bt_scan_rev_ = 0;
std::vector<BtDeviceInfo> bt_scan_result_;
```

**중복 실행을 막아라.** `bt_scanning_`이 참인 동안 새 요청이 오면 무시한다. inquiry를 겹쳐 돌리면 드라이버가 오류를 돌려준다.

**중지 요청은 진행 중인 inquiry를 끊지 않는다.** `BluetoothFindFirstDevice`는 중간에 취소할 수 없다. 중지는 "결과가 와도 버린다"는 표시일 뿐이고, 화면은 곧바로 검색 전 상태로 돌아간다. 이 사실을 코드 주석에 적어 두어라.

## 3. 결과를 패널로 옮기는 길

`ControlCenterLive`에 목록 전체를 싣지 마라. `ApplyLive`가 주기마다 도는데 벡터를 매번 복사하게 된다. 가벼운 값만 싣는다.

```cpp
bool bt_scanning = false;
uint64_t bt_scan_rev = 0;
```

`ControlCenterHost`에 결과를 가져오는 콜백을 하나 더 둔다.

```cpp
std::function<std::vector<BtDeviceInfo>()> bt_scan_result;
```

`menu_bar.cpp`의 `ShowControlCenter`에서 `widgets_`를 캡처해 넘긴다. `host.live`를 넘기는 자리 바로 옆이다.

`ControlCenterContent::ApplyLive`는 이렇게 움직인다.

- `live.bt_scanning`을 `bt_scanning_`에 반영한다.
- `live.bt_scan_rev`가 자기가 가진 값과 다르면, 그때만 `host_.bt_scan_result()`를 불러 목록을 받아 `bt_found_`에 넣고 리비전을 갱신한다.

이렇게 하면 결과가 새로 나왔을 때만 목록이 오간다.

## 4. 페이지 배치

`MakeBluetoothPage`의 인자를 늘린다.

```cpp
BluetoothPageMetrics MakeBluetoothPage(bool present, bool on, int device_n, int found_n, bool scanning);
```

세로 순서다. `panel::Stack`을 그대로 쓴다.

| 순서 | 요소 | 높이 | 조건 |
| --- | --- | --- | --- |
| 1 | 제목 + 토글 | `panel::kHeaderHDip` | 늘 |
| 2 | 간격 | `panel::kHeaderGapDip` | 늘 |
| 3 | 구분선 | `panel::kDivHDip` | 늘 |
| 4 | 간격 | `panel::kDivGapDip` | 늘 |
| 5 | "내 장치" 헤더 | `panel::kSectionHDip` | 아래 6이 있고 9도 있을 때만 |
| 6 | 짝지어진 장치 행 × n | `panel::kRowHDip` | 장치가 있을 때 |
| 7 | 비어 있음 안내 | `panel::kRowHDip` | 장치도 결과도 없을 때 |
| 8 | "다른 장치" 헤더 | `panel::kSectionHDip` | 검색 중이거나 결과가 있을 때 |
| 9 | 결과 행 × n, 또는 "검색 중…" 한 줄 | `panel::kRowHDip` | 같은 조건 |
| 10 | 간격 | `panel::kDivGapDip` | 늘 |
| 11 | 구분선 | `panel::kDivHDip` | 늘 |
| 12 | 간격 | `panel::kDivGapDip` | 늘 |
| 13 | 검색 줄 | `panel::kSettingsHDip` | 늘 |
| 14 | "Bluetooth 설정…" | `panel::kSettingsHDip` | 늘 |
| 15 | 하단 패딩 | `panel::kBottomPadDip` | 늘 |

**"내 장치" 헤더는 "다른 장치" 구역이 생길 때만 붙인다.** 검색 전에는 구역이 하나뿐이라 헤더가 군더더기다. Wi-Fi 패널이 알려진 망만 있을 때 헤더를 그대로 두는 것과 다르게 가는 이유는, 블루투스는 평소 상태가 구역 하나이기 때문이다.

13번 줄의 글은 상태에 따라 바뀐다.

| 상태 | 글 |
| --- | --- |
| 검색 전 | `장치 추가…` |
| 검색 중 | `검색 중지` |
| 결과가 있음 | `다시 검색…` |
| 라디오가 꺼짐 또는 어댑터 없음 | 줄을 아예 넣지 않는다 |

## 5. 그리기와 누르기

- 결과 행은 짝지어진 장치 행과 같은 모양이다. `panel::DrawRowCircle`을 쓰고 `active`는 늘 거짓이다. 이름은 muted 색으로 그린다. 아직 내 장치가 아니기 때문이다.
- 이름이 빈 장치는 `알 수 없는 장치`로 그린다.
- "검색 중…"은 `panel::kRowHDip` 높이에 muted 색 `regular14_`로 왼쪽 인셋에서 그린다. 원 아이콘은 넣지 않는다.
- 결과 행을 누르면 지금 짝짓기에 쓰는 `BluetoothAuthenticateDeviceEx` 경로를 그대로 탄다. 성공하면 검색 결과에서 그 장치를 빼고 짝지어진 목록을 다시 열거한다.
- 검색 줄의 히트 아이디는 새로 만든다(예: `kPageScan`). 기존 `kPageMore`나 `kPageList`와 겹치지 않게 하라. 결과 행의 히트는 `kPageList + kBtListMax + i`처럼 짝지어진 목록 뒤에 이어 붙이고, `kWifiListTotalMax`를 쓰는 기존 경계 검사가 이 값을 삼키지 않는지 확인하라.

## 6. 닫을 때

패널이 닫히면(`Dismissed`) `bt_found_`를 비우고 리비전을 초기화한다. 다시 열었을 때 지난 검색 결과가 남아 있으면 안 된다. 이미 사라진 장치를 보여 주게 된다.

---

## 검증

```powershell
cmake --build out/cmake-debug --config Debug
```

경고 없이 통과해야 한다.

화면 확인은 사용자에게 남긴다. 다만 아래는 로그로 확인하고 보고하라.

- `bt scan took ... ms n=...`이 남고 5초 안팎인지
- 검색이 도는 동안 상단바가 멈추지 않는지. `[perf] bar full[...]`의 `max`가 평소와 같은지 보면 된다
- 검색을 두 번 잇달아 눌러도 inquiry가 겹치지 않는지

### 하지 말 것

- **앱을 강제로 종료하지 마라.** 빌드를 위해 실행 중인 `bamti.exe`를 끝내야 한다면, `taskkill`이 아니라 상단바 메뉴의 종료를 쓰거나 사용자에게 부탁하라. 2026-09-05에 정상 종료 흔적(`[tray] restored`) 없이 프로세스가 사라진 일이 있었다. 이 앱은 셸을 대체하므로 강제 종료하면 태스크바가 숨겨진 채 남을 수 있다.
- **검색을 반복해서 돌리며 시험하지 마라.** 연결된 오디오 장치의 소리가 끊긴다. 한 번 돌려 로그를 확인하는 것으로 끝낸다.
- 지금 잘 도는 것을 건드리지 마라. 트레이 블루투스 아이콘 필터는 사용자가 확인했고 정상이다.
