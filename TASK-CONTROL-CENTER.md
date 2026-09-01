# 작업 지시서: 제어 센터 패널을 만든다 (1단계)

`TASK-SPOTLIGHT-WINSEARCH.md` 다음에 하십시오.

---

## 0. 무엇을 만드는가

Windows 11의 빠른 설정 패널(`Win+A`로 열리는 것)에 해당하는 패널을 bamti 상단바에 만듭니다. 상단바에 제어 센터 아이콘을 하나 두고, 누르면 그 아래에 패널이 열립니다.

**기존 배터리, 볼륨, 네트워크 위젯과 각각의 패널은 그대로 둡니다.** Windows는 이 셋을 하나로 묶어 셋 중 아무거나 눌러도 같은 패널이 열리지만, bamti는 개별 항목과 개별 패널을 유지하면서 **한꺼번에 다루는 통합 패널을 하나 더 두는 것**이 목표입니다. 기존 위젯을 없애거나 묶지 마십시오.

### 0-1. 왜 Windows 패널을 그대로 띄우지 않는가

`Win+A`를 합성하면 Windows의 빠른 설정이 열립니다. 그러나 그 패널은 화면 오른쪽 **아래**, 작업 표시줄 위에 뜹니다. bamti는 작업 표시줄을 숨기고 상단바를 쓰므로 패널만 화면 반대편에서 튀어나옵니다. 위젯 보드 단추와 달리 이것은 위치가 어긋난 채로는 쓸모가 없습니다.

그래서 자체 패널을 그립니다.

### 0-2. 이번 단계에서 하지 않는 것

다음 두 가지는 **WinRT가 필요합니다.** 이 프로젝트는 지금 Win32와 COM(WRL)만 씁니다. WinRT를 들이는 판단은 따로 하겠으니 이번 단계에 넣지 마십시오.

| 미룬 것 | 필요한 API |
|---|---|
| Wi-Fi와 Bluetooth를 실제로 켜고 끄기 | `Windows.Devices.Radios` |
| 재생 중인 미디어 카드(제목, 앨범 아트, 이전/재생/다음) | `Windows.Media.Control` |

이번 단계에서 이 타일들은 **상태를 보여 주고, 누르면 해당 설정 페이지를 엽니다.**

---

## 1. 완료 조건

1. 상단바에 제어 센터 아이콘이 있고, 누르면 패널이 열립니다.
2. 패널에 타일 여섯 개가 2행 3열로 놓입니다.
3. 밝기와 볼륨 슬라이더가 있고, 볼륨은 실제로 소리를 바꿉니다.
4. 패널 아래쪽에 배터리 잔량과 설정 단추가 있습니다.
5. 패널을 열어 둔 동안 값이 살아 움직입니다.

---

## 2. 진입점

### 2-1. 상단바 아이콘

`TASK-SPOTLIGHT-WINSEARCH.md`에서 만든 검색 단추와 같은 방식입니다. `SegmentKind`에 갈래를 더하고, 시계 **왼쪽**에 놓으십시오. Windows에서 이 패널을 여는 자리가 시계 옆이므로 손이 기억하는 위치가 같아집니다.

아이콘은 `Segoe Fluent Icons`의 **`U+E713`**(설정 톱니)이 아니라 **`U+E9E9`**(제어 센터에 해당하는 슬라이더 모양)를 쓰십시오. 톱니는 패널 안쪽 설정 단추에 이미 씁니다. 두 곳이 같은 그림이면 무엇이 무엇인지 알 수 없습니다.

두 글리프를 화면에 띄워 보고 어느 쪽이 이미지의 아이콘과 닮았는지 눈으로 확인한 뒤 고르십시오. **고른 코드포인트를 커밋 메시지에 적으십시오.**

### 2-2. 설정으로 켜고 끈다

`WidgetSettings`에 `bool control_center = false;`를 더하고, 우클릭 메뉴에 항목을 넣으십시오. 기존 위젯 항목들과 같은 방식입니다.

---

## 3. 패널의 뼈대

### 3-1. 새 `PopupContent` 구현체

`src/control_center.hpp` / `.cpp`를 새로 만듭니다.

```cpp
class ControlCenterContent : public PopupContent {
 public:
  void Reset(ControlCenterHost host);
  void Refresh();                      // 값만 다시 읽는다

  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;
  bool StickyRow(int index) const override;
  void StickyInvoke(int index) override;
  bool DragRow(int index) const override;
  void DragTo(int index, POINT client, UINT dpi) override;
  void DragEnd(int index) override;
};
```

`StatusPanelContent`가 이 인터페이스를 어떻게 쓰는지 먼저 읽으십시오. 특히 **슬라이더의 드래그 훅(`DragRow`/`DragTo`/`DragEnd`)** 이 이미 만들어져 있습니다. 제어 센터의 슬라이더도 같은 방식으로 동작해야 하므로, 그 흐름을 그대로 따르십시오. 새로 설계하지 마십시오.

`FIX-SLIDER-ENDS.md`에서 고친 슬라이더 기하 계산도 그대로 씁니다. 같은 식을 두 곳에 복사하지 말고, `status_panel.cpp`의 익명 이름공간에 있는 헬퍼를 공용 헤더로 옮겨 함께 쓰십시오.

### 3-2. 배치

패널 안쪽 여백은 기존 팝업과 같은 `kPanelPadDip`(12dip)을 쓰십시오. 첨부된 화면의 비율에 맞춘 값입니다.

| 부분 | 크기(DIP) |
|---|---|
| 패널 폭 | 320 고정 |
| 타일 | 3열, 열 사이 간격 8, 높이 56 |
| 타일 아래 이름 | 높이 18, 가운데 정렬 |
| 타일 행 사이 간격 | 12 |
| 구분선 | 위아래 여백 12, 두께 1 |
| 슬라이더 줄 | 높이 36, 왼쪽에 16dip 아이콘 |
| 아래 줄 | 높이 32 |

패널 폭을 내용에 따라 바꾸지 마십시오. 타일 격자는 폭이 고정이어야 줄이 맞습니다.

### 3-3. 히트 인덱스

`PopupContent`의 히트 판정은 정수 인덱스 하나입니다. 타일 여섯 개, 슬라이더 둘, 설정 단추 하나에 각각 인덱스를 주십시오. `StatusPanelContent`가 `hits_` 벡터로 하는 것과 같은 방식입니다.

타일 안의 화살표(`>`)는 **별도 인덱스로 나누지 마십시오.** 이번 단계에서는 타일 전체가 하나의 누름 대상이고, 눌리면 설정 페이지가 열립니다. 화살표는 그림으로만 그립니다. 2단계에서 왼쪽(토글)과 오른쪽(자세히)이 갈릴 때 나누십시오.

---

## 4. 타일 여섯 개

이번 단계에서는 전부 **상태 표시 + 설정 페이지 열기**입니다.

| 타일 | 상태 읽기 | 누르면 여는 곳 |
|---|---|---|
| Wi-Fi | 연결된 SSID (5절) | `ms-settings:network-wifi` |
| Bluetooth | 켜짐 여부만 (4-2) | `ms-settings:bluetooth` |
| 비행기 모드 | 읽지 않는다 | `ms-settings:network-airplanemode` |
| 절전 모드 | `SYSTEM_POWER_STATUS.SystemStatusFlag`의 최하위 비트 | `ms-settings:batterysaver` |
| 야간 모드 | 읽지 않는다 | `ms-settings:night-light` |
| 접근성 | 읽지 않는다 | `ms-settings:easeofaccess` |

`ShellExecuteW(nullptr, L"open", L"ms-settings:...", nullptr, nullptr, SW_SHOWNORMAL)`로 엽니다. 기존 위젯의 "전원 설정 열기"와 같은 경로이므로 그 코드를 보고 맞추십시오.

### 4-1. 켜짐과 꺼짐의 모양

첨부된 화면에서 켜진 타일은 강조색 배경에 흰 글씨이고, 꺼진 타일은 옅은 배경에 보통 글씨입니다. 같게 그리십시오. 강조색은 `DockIndicatorColor(dark)`를 쓰고, 옅은 배경은 `MenuItemHoverFill(dark, false)`을 쓰십시오. 새 색을 만들지 마십시오.

**상태를 읽지 않는 타일은 항상 꺼진 모양으로 그리십시오.** 모르는 것을 켜진 것처럼 그리면 거짓말이 됩니다.

### 4-2. Bluetooth 상태

라디오가 켜져 있는지만 알면 됩니다. WinRT 없이 알아내는 길은 `BluetoothFindFirstRadio`(`bthprops.cpp`, `bluetoothapis.h`)입니다. 라디오 핸들이 하나라도 열리면 어댑터가 있는 것이고, `BluetoothIsConnectable`로 켜져 있는지 짐작할 수 있습니다.

**이 방법이 실제로 맞는지 반드시 확인하십시오.** 어댑터가 있지만 꺼진 상태에서 무엇이 돌아오는지 직접 재어 보고, 구분되지 않으면 **상태 표시를 포기하고 항상 꺼진 모양으로 그리십시오.** 추측으로 켜짐을 표시하지 마십시오.

`bthprops.lib`를 `CMakeLists.txt`에 더해야 합니다.

---

## 5. Wi-Fi 이름

`wlanapi.h`를 씁니다. WinRT가 필요 없습니다.

```cpp
HANDLE handle = nullptr;
DWORD negotiated = 0;
if (WlanOpenHandle(2, nullptr, &negotiated, &handle) != ERROR_SUCCESS) { /* 없음 */ }

PWLAN_INTERFACE_INFO_LIST list = nullptr;
WlanEnumInterfaces(handle, nullptr, &list);
// list->InterfaceInfo[i].isState == wlan_interface_state_connected 인 것을 찾는다
// WlanQueryInterface(handle, &guid, wlan_intf_opcode_current_connection, ...) 로
// WLAN_CONNECTION_ATTRIBUTES 를 받아 wlanAssociationAttributes.dot11Ssid 를 읽는다
```

SSID는 **UTF-8 바이트 배열**입니다(`ucSSID`, `uSSIDLength`). `MultiByteToWideChar(CP_UTF8, ...)`로 바꾸십시오. 그냥 `wchar_t`로 캐스팅하면 한글 SSID가 깨집니다.

`WlanFreeMemory`와 `WlanCloseHandle`을 반드시 부르십시오. 이 API는 호출자가 해제해야 하는 버퍼를 여럿 돌려줍니다.

연결되어 있지 않으면 타일 이름에 `연결 안 됨`을 쓰고 꺼진 모양으로 그립니다. 무선 어댑터가 아예 없으면 **타일을 감추지 말고** 꺼진 모양으로 두십시오. 격자가 무너집니다.

`wlanapi.lib`를 `CMakeLists.txt`에 더해야 합니다.

### 5-1. 비용

`WlanOpenHandle`부터 닫기까지의 시간을 재서 로그에 한 번 남기십시오. 패널을 열 때마다 도는 코드입니다.

```cpp
Log(L"cc", L"wlan query took %.2f ms", ms);
```

**5ms를 넘으면** 패널이 열릴 때 눈에 띄게 늦습니다. 그때는 값을 미리 읽어 두는 쪽으로 옮기고, 그 사실을 보고하십시오.

---

## 6. 슬라이더 둘

### 6-1. 볼륨

`src/widgets/volume.cpp`의 `VolumeControl`을 그대로 씁니다. **새로 만들지 마십시오.**

지금 `VolumeControl`은 `BuiltinWidgets`가 소유하고 작업자 스레드에서만 만집니다. 제어 센터는 UI 스레드에서 돌아갑니다. **한 객체를 두 스레드에서 만지지 마십시오.**

대신 기존 위젯의 이벤트 경로를 타십시오. 제어 센터의 볼륨 슬라이더를 끌면 `StatusEvent`를 만들어 `bamti.widget/volume`의 `volume_level` 행에 `slide` 이벤트를 보내는 것입니다. `BuiltinWidgets::OnEvent`가 이미 그것을 받아 처리합니다.

```cpp
StatusEvent ev;
ev.id = "bamti.widget/volume";
ev.event = "slide";
ev.row_id = "volume_level";
ev.value = v;
host_.dispatch(ev);
```

**볼륨 위젯이 꺼져 있어도 동작해야 합니다.** 지금 `SampleVolume`은 `settings_.volume`이 꺼져 있으면 곧바로 빠져나가고 장치를 놓아 버립니다. 제어 센터가 켜져 있으면 볼륨을 계속 읽도록 조건을 넓히십시오.

```cpp
enabled = (settings_.volume || settings_.control_center) && active_ && sink_ != nullptr;
```

게시(`Publish`)는 `settings_.volume`일 때만 하고, 읽기는 둘 중 하나만 켜져도 하도록 나누십시오. 상단바에 볼륨 항목이 뜨는 것과 제어 센터가 값을 아는 것은 별개입니다.

### 6-2. 밝기

**먼저 이 컴퓨터에서 밝기를 다룰 수 있는지 재십시오.** 확인한 바로는 WMI의 `WmiMonitorBrightness`가 현재 밝기를 `0%`로 보고합니다. 이 값이 맞다면 화면이 꺼져 있어야 하므로, 보고가 틀렸거나 해당 인스턴스가 실제 화면이 아닙니다.

두 갈래를 순서대로 시도하십시오.

1. **DDC/CI** — `dxva2.dll`의 `GetPhysicalMonitorsFromHMONITOR`, `GetMonitorBrightness`, `SetMonitorBrightness`. 외부 모니터에서 동작합니다. `dxva2.lib` 링크가 필요합니다.
2. **WMI** — `root\wmi`의 `WmiMonitorBrightness`(읽기)와 `WmiMonitorBrightnessMethods::WmiSetBrightness`(쓰기). 노트북 내장 화면에서 동작합니다.

각각에 대해 **읽은 값과 걸린 시간을 로그로 남기고**, 어느 쪽이 이 컴퓨터에서 실제로 동작하는지 판정하십시오.

```cpp
Log(L"cc", L"brightness ddcci=%d value=%lu took %.2f ms", ok, value, ms);
Log(L"cc", L"brightness wmi=%d value=%lu took %.2f ms", ok, value, ms);
```

**둘 다 실패하면 밝기 슬라이더 줄을 통째로 감추십시오.** 움직여도 아무 일이 없는 슬라이더를 두지 마십시오. 감춘 경우 패널 높이를 그만큼 줄여야 합니다.

DDC/CI는 모니터와 주고받는 데 수십에서 수백 밀리초가 걸리는 일이 있습니다. **UI 스레드에서 부르지 마십시오.** 패널이 그동안 얼어붙습니다. 값을 미리 읽어 두고, 쓰기는 작업자 스레드로 넘기십시오. `BuiltinWidgets`가 이미 작업자 스레드와 대기 중인 동작(`pending_level_` 같은 것)을 다루는 구조를 갖고 있으므로 같은 방식을 따르십시오.

측정 결과와 고른 방법을 이 문서 끝에 적으십시오.

---

## 7. 아래 줄

왼쪽에 배터리 아이콘과 잔량 퍼센트, 오른쪽에 설정 톱니를 놓습니다.

배터리 아이콘은 `TASK-BAR-ICON-ART.md`에서 만든 그리기 함수를 그대로 부르십시오. 색도 같습니다. 상단바와 제어 센터에서 배터리가 다르게 생기면 안 됩니다.

설정 톱니는 `Segoe Fluent Icons`의 `U+E713`이고, 누르면 `ms-settings:`를 엽니다.

배터리가 없는 컴퓨터에서는 왼쪽을 비우십시오. 줄 자체는 남깁니다.

---

## 8. 열어 둔 동안 값이 움직여야 한다

`MenuBar::RefreshOpenPanel`이 이미 열려 있는 상태 패널을 주기적으로 새로 고칩니다. 제어 센터도 같은 경로를 타게 하십시오.

새로 고칠 때 **슬라이더를 끄는 중이면 그 값은 덮어쓰지 마십시오.** `StatusPanelContent`가 `drag_row_`로 그것을 막고 있습니다. 같은 보호를 넣으십시오. 이것을 빠뜨리면 손잡이를 끄는 동안 값이 튀어 되돌아갑니다.

Wi-Fi 조회와 밝기 조회는 새로 고칠 때마다 하지 마십시오. **2초에 한 번이면 충분합니다.** 볼륨과 배터리는 이미 위젯이 읽고 있으므로 그 값을 받아 쓰십시오.

---

## 9. 하지 말아야 할 것

- 배터리, 볼륨, 네트워크 위젯을 하나로 합치지 마십시오. 개별 항목과 개별 패널은 그대로 남습니다.
- `Win+A`를 합성하지 마십시오.
- `VolumeControl` 객체를 UI 스레드에서 직접 만지지 마십시오.
- 야간 모드를 레지스트리로 켜고 끄려 하지 마십시오. 공개된 방법이 아니고 형식이 문서화되어 있지 않습니다. 설정 페이지를 여는 것으로 끝내십시오.
- 비행기 모드를 코드로 켜려 하지 마십시오. 공개 API가 없습니다.
- WinRT 헤더를 포함하지 마십시오. 이번 단계의 범위 밖입니다.
- DDC/CI 호출을 UI 스레드에서 하지 마십시오.
- 레지스트리에 쓰지 마십시오. 검증 절차에도 넣지 마십시오.

---

## 10. 검증

1. Release 빌드가 경고 없이 통과합니다.
2. 우클릭 메뉴에서 제어 센터를 켜면 상단바 시계 왼쪽에 아이콘이 생깁니다.
3. 아이콘을 누르면 그 아래에 패널이 열립니다. 다시 누르거나 바깥을 누르면 닫힙니다.
4. 타일 여섯 개가 2행 3열로 놓이고, 이름이 각 타일 아래에 있습니다.
5. Wi-Fi 타일에 지금 연결된 네트워크 이름이 보입니다. 한글이 섞인 이름도 깨지지 않습니다.
6. Wi-Fi를 끊으면 `연결 안 됨`으로 바뀌고 타일이 꺼진 모양이 됩니다.
7. 타일을 하나씩 눌러 각각 맞는 설정 페이지가 열리는지 봅니다. 여섯 개 전부 확인하십시오.
8. 볼륨 슬라이더를 끌면 소리가 실제로 바뀌고, Windows 소리 설정의 값도 함께 움직입니다.
9. 상단바의 볼륨 위젯을 **끈 상태에서도** 제어 센터의 볼륨 슬라이더가 동작합니다.
10. 밝기 슬라이더가 보인다면 끌었을 때 화면 밝기가 바뀝니다. 보이지 않는다면 로그에서 6-2절의 판정 근거를 확인합니다.
11. 아래 줄의 배터리 잔량이 상단바의 배터리 항목과 같은 값입니다.
12. 설정 톱니를 누르면 설정 앱이 열립니다.
13. 패널을 열어 둔 채 다른 앱에서 볼륨을 바꿉니다. 패널의 슬라이더가 2초 안에 따라옵니다.
14. 슬라이더를 끄는 동안 값이 되돌아가지 않습니다.
15. 밝은 테마와 어두운 테마를 모두 확인합니다.

---

## 11. 커밋

```
feat: 제어 센터 패널의 뼈대를 만든다
feat: 제어 센터에 Wi-Fi와 전원 상태를 보여 준다
feat: 제어 센터에 볼륨과 밝기 슬라이더를 넣는다
feat: 상단바에 제어 센터 단추를 더한다
```

---

## 12. 측정 기록

작업을 마치면 아래를 채우십시오.

| 항목 | 값 |
|---|---|
| `wlan query` 소요 시간 | 75.65 ms (5 ms 초과. 작업자 스레드에서 2초마다 미리 읽음) |
| 밝기 제어 방법 (DDC/CI / WMI / 없음) | DDC/CI (둘 다 성공. 지시서 순서대로 DDC/CI를 씀. WMI는 70%) |
| 밝기 읽기 소요 시간 | DDC/CI 64.08 ms, WMI 44.68 ms |
| Bluetooth 상태 구분 가능 여부 | 아니오. `radio=1 connectable=1` 한 상태만 관측. 켜짐/꺼짐을 갈라 보지 못해 항상 꺼진 모양 |
| 제어 센터 아이콘으로 고른 코드포인트 | U+E9E9 |
| 패널 열기부터 그리기까지 걸린 시간 | 10.81 ms (wlan 미리 읽기 후). UI에서 조회하던 때는 119.46 ms |
