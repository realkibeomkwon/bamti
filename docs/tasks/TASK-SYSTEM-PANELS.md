# TASK-SYSTEM-PANELS — 볼륨과 블루투스를 네트워크 패널과 같은 디자인 언어로 다시 만든다

이 문서는 2026-09-05 시점의 작업 지시서이며, 현재 코드의 설명이 아니라 당시의 기록이다.

## 배경

`db2539f`에서 네트워크 위젯의 패널을 맥 네트워크 메뉴의 실측 수치대로 다시 그렸다. 그 결과 상단바에서 패널을 여는 항목이 두 갈래로 갈라져 있다.

| 항목 | 좌클릭 경로 | 패널 구현 | 모양 |
| --- | --- | --- | --- |
| 네트워크 | `MenuBar::ToggleNetworkPanel` → `ShowControlCenter(rect, ControlCenterPage::kWifi)` | `ControlCenterContent::RenderNetworkPage` | 맥 메뉴 모양 |
| 볼륨 | `MenuBar::OpenStatusPanel` → `StatusPanelContent` | `src/status_panel.cpp` | 옛 카드 모양 |
| 블루투스 | 없음(Windows 트레이 아이콘을 `TrayMirror`가 그대로 비춘다) | 없음 | Windows 기본 |

목표는 세 가지다.

1. **볼륨 패널을 네트워크 패널과 같은 디자인 언어로 다시 만들고, 출력 장치를 목록에서 고를 수 있게 한다.** 지금은 크기와 음소거만 조절할 수 있고 장치 선택은 아예 불가능하다.
2. **블루투스를 자체 위젯으로 만든다.** 아이콘을 Direct2D로 직접 그리고, 좌클릭하면 네트워크와 같은 방식으로 패널이 열리게 한다.
3. **세 패널이 같은 치수 토큰과 같은 그리기 헬퍼를 공유하게 한다.** 수치를 각 렌더러에 복사해 두면 곧 어긋난다.

## 목표 화면

사용자가 첨부한 맥 화면 두 장이 기준이다.

**볼륨 패널**

```
┌────────────────────────────────────┐
│ 사운드                              │   제목만, 토글 없음
│ 🔈 ━━━━━━━●─────────────  🔊       │   슬라이더, 양옆에 스피커 아이콘
│ ──────────────────────────────────  │   구분선
│ 출력                                │   섹션 헤더
│ (●) Mac mini Speakers              │   원 아이콘 + 이름
│ (●) DELL U4025QW                   │
│ (●) 기범의 AirPods Pro   54% ▭  ›  │   연결된 장치는 강조색 원, 배터리와 꺾쇠
│ ──────────────────────────────────  │
│ 기범의 AirPods Pro 설정…            │   장치가 있을 때만
│ 사운드 설정…                        │
└────────────────────────────────────┘
```

**블루투스 패널**

```
┌────────────────────────────────────┐
│ Bluetooth                    (◯━)  │   제목 + 라디오 토글
│ ──────────────────────────────────  │
│ (●) 기범의 AirPods Pro   54% ▭     │   연결된 장치는 강조색 원
│ (◌) BT5.0 KB                       │   연결이 끊긴 장치는 흐리게
│ (◌) 기범's Magic Trackpad          │
│ (◌) 기범의 Powerbeats Pro #2       │
│ ──────────────────────────────────  │
│ Bluetooth 설정…                     │
└────────────────────────────────────┘
```

## 설계 원칙

패널마다 다른 수치를 쓰지 않는다. 네트워크 패널이 이미 확정한 치수를 **공용 토큰**으로 끌어내고, 세 패널이 모두 그 토큰과 공용 그리기 헬퍼를 통해서만 그린다. 새 패널을 더할 때 복사해야 하는 코드가 없어야 한다.

---

## 1단계 — 패널 디자인 토큰과 그리기 헬퍼를 분리한다

### 새 파일 `src/panel_style.hpp`

`src/control_center.cpp`의 익명 이름공간에 있는 `kNet*` 상수를 `panel_style.hpp`로 옮기고, 이름에서 `Net`을 떼어 패널 전반의 토큰임을 드러낸다. 값은 그대로 유지한다.

```cpp
namespace bamti::panel {

inline constexpr int kWidthDip = 308;        // kNetPageWidthDip
inline constexpr int kInsetDip = 14;         // kNetInsetDip
inline constexpr int kTopPadDip = 9;         // kNetTopPadDip
inline constexpr int kHeaderHDip = 24;       // kNetHeaderHDip
inline constexpr int kHeaderGapDip = 9;      // kNetHeaderGapDip
inline constexpr int kDivHDip = 1;           // kNetDivHDip
inline constexpr int kDivGapDip = 8;         // kNetDivGapDip
inline constexpr int kSectionHDip = 17;      // kNetSectionHDip
inline constexpr int kRowHDip = 32;          // kNetRowHDip
inline constexpr int kSettingsHDip = 22;     // kNetSettingsHDip
inline constexpr int kBottomPadDip = 10;     // kNetBottomPadDip
inline constexpr int kToggleWDip = 54;       // kNetToggleWDip
inline constexpr int kToggleHDip = 24;       // kNetToggleHDip
inline constexpr int kBackWDip = 24;         // kNetBackWDip
inline constexpr int kCircleDip = 26;        // kNetCircleDip
inline constexpr float kKnobDip = 19.0f;     // kNetKnobDip
inline constexpr float kKnobInsetDip = 2.5f; // kNetKnobInsetDip
inline constexpr int kTextLeftDip = 49;      // RenderNetworkPage의 DipToPx(49, dpi)
inline constexpr int kNoteHDip = 16;         // kNetEthHDip, 이더넷 줄에 쓰던 높이

}  // namespace bamti::panel
```

### 새 파일 `src/panel_style.cpp`

`RenderNetworkPage` 안의 람다를 자유 함수로 끌어낸다. 각 함수는 렌더 타깃과 브러시, DPI, 다크 여부를 받는다. `ControlCenterContent`의 멤버를 참조하지 않게 하여 어느 패널에서든 부를 수 있어야 한다.

```cpp
namespace bamti::panel {

// 배경 호버. corner::HoverPx로 곡률을 잡는다.
void FillHover(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const RECT& rc, UINT dpi, bool dark);

// 좌우 인셋을 뺀 폭으로 그리는 1 DIP 구분선. 색은 fg의 알파 0.10이다.
void DrawDivider(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark);

// 54x24 알약 토글. enabled가 거짓이면 트랙과 노브를 알파 0.40으로 낮춘다.
void DrawToggle(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const RECT& rc, bool on, bool enabled, bool dark);

// 26 DIP 원 안에 Fluent 글리프를 넣는다. active면 원은 AccentFillColor,
// 글리프는 AccentOnColor이고, 아니면 BadgeOffFill과 fg다.
void DrawRowCircle(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fluent,
                   ID2D1SolidColorBrush* brush, float cx, float cy, UINT dpi, bool dark, bool active,
                   const wchar_t* glyph);

// 섹션 헤더("출력", "알려진 네트워크")를 muted 색 regular12로 그린다.
void DrawSectionHeader(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fmt,
                       ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, const wchar_t* title);

// "네트워크 설정…" 같은 하단 항목. 높이 kSettingsHDip, hot이면 호버를 깐다.
void DrawSettingsRow(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fmt,
                     ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, bool hot,
                     const wchar_t* label);

}  // namespace bamti::panel
```

### 세로 배치 누산기

`MakeNetworkPage`는 `y`를 더해 가며 각 요소의 위치를 정하는데, 볼륨과 블루투스도 같은 절차를 밟는다. 같은 코드를 세 벌 쓰지 말고 작은 누산기를 둔다.

```cpp
namespace bamti::panel {

class Stack {
 public:
  Stack() : y_(kTopPadDip) {}
  int Take(int h) { const int at = y_; y_ += h; return at; }   // 요소를 놓고 그 위치를 돌려준다
  void Gap(int h) { y_ += h; }
  int y() const { return y_; }
  int Finish() { y_ += kBottomPadDip; return y_; }             // 전체 높이
 private:
  int y_;
};

}  // namespace bamti::panel
```

`MakeNetworkPage`를 이 누산기로 다시 쓰되 **계산 결과는 한 픽셀도 달라지면 안 된다.** 바꾸기 전후로 대표 조합(이더넷 유무 × 어댑터 유무 × 라디오 상태 × 알려진 망 개수 × 다른 망 개수)의 `height`와 각 `*_y` 값을 견주어 같은지 확인한다.

### 리팩터링 검증

이 단계는 **화면이 전혀 달라지지 않아야 한다.** 네트워크 패널을 열어 이전 스크린샷과 겹쳐 보고 어긋난 곳이 없는지 확인한다.

---

## 2단계 — 볼륨 패널을 다시 만든다

### 2-1. 출력 장치 계층 `src/audio_devices.hpp` / `.cpp`

`src/widgets/volume.cpp`의 `VolumeControl`은 기본 장치의 크기와 음소거만 다룬다. 장치 열거와 전환은 별도 파일로 만든다.

```cpp
namespace bamti {

enum class AudioForm { kUnknown, kSpeakers, kHeadphones, kHeadset, kDisplay, kDigital };

struct AudioEndpoint {
  std::wstring id;         // IMMDevice::GetId, 기본 장치 전환에 쓴다
  std::wstring name;       // PKEY_Device_FriendlyName
  AudioForm form = AudioForm::kUnknown;
  bool is_default = false;
};

// eRender, DEVICE_STATE_ACTIVE 장치를 모은다. 기본 장치가 목록의 어디에 있든
// is_default로 표시하고, 정렬은 이름 오름차순으로 안정되게 한다.
std::vector<AudioEndpoint> EnumAudioOutputs();

// 기본 출력 장치를 바꾼다. eConsole, eMultimedia, eCommunications 세 역할을 모두 바꾼다.
bool SetDefaultAudioOutput(const std::wstring& device_id);

}  // namespace bamti
```

**기본 장치 전환에는 공개 API가 없다.** `IPolicyConfig`라는 비공개 COM 인터페이스를 쓴다. 프로젝트에 아래 값을 직접 선언해야 한다.

- `CLSID_CPolicyConfigClient` = `{870af99c-171d-4f9e-af0d-e63df40c2bc9}`
- `IID_IPolicyConfig` = `{f8679f50-850a-41cf-9c72-430f290290c8}`
- `SetDefaultEndpoint`는 vtable에서 13번째 항목이고 시그니처는 `HRESULT(PCWSTR device_id, ERole role)`이다.

**이 값과 vtable 자리는 반드시 실기에서 확인하라.** 비공개 인터페이스이므로 Windows 판올림에서 달라질 수 있다. `CoCreateInstance`가 실패하거나 `SetDefaultEndpoint`가 실패하면 조용히 거짓을 돌려주고, 호출한 쪽은 아무 일도 없었던 것처럼 두되 `Log(L"audio", ...)`에 한 번만 남긴다. 앱이 죽으면 안 된다.

`PKEY_Device_FriendlyName`과 `PKEY_AudioEndpoint_FormFactor`는 `functiondiscoverykeys_devpkey.h`와 `mmdeviceapi.h`에 있다. 폼팩터는 목록 아이콘을 고르는 데 쓴다.

| 폼팩터 | 아이콘 글리프 |
| --- | --- |
| `Speakers`, `Subwoofer` | `\xE7F5` (스피커) |
| `Headphones`, `Headset` | `\xE7F6` (헤드폰) |
| `DigitalAudioDisplayDevice`, `HDMI` | `\xE7F4` (모니터) |
| 그 밖 | `\xE767` (기본 볼륨 글리프) |

**글리프 코드는 Segoe Fluent Icons에서 실제로 그 모양이 나오는지 눈으로 확인하라.** 모양이 다르면 맥 화면에 가까운 다른 글리프로 바꾸고, 무엇으로 바꿨는지 보고하라.

장치가 바뀌었을 때 목록을 새로 고치려면 `IMMNotificationClient`를 등록하는 편이 정확하다. 다만 패널이 열려 있는 동안에만 필요하므로, 우선은 네트워크 패널이 쓰는 방식대로 **패널이 열릴 때와 `RefreshPageLists`의 주기(2초)마다 다시 열거**하는 것으로 충분하다. 열거 비용을 `Log(L"audio", L"enum took %.2f ms", ...)`로 한 번 재고, 5 ms를 넘으면 그 사실을 로그에 남긴다.

### 2-2. 볼륨 페이지

`ControlCenterContent::Page`에 `kVolume`을 더한다. `ControlCenterPage`(공개 열거)에도 `kVolume`을 더해 `MenuBar`가 이 페이지를 지정해 열 수 있게 한다.

**세로 배치** (`MakeVolumePage`, 1단계의 `Stack`을 쓴다)

| 순서 | 요소 | 높이 |
| --- | --- | --- |
| 1 | 제목 "사운드" | `kHeaderHDip` (24) |
| 2 | 간격 | `kHeaderGapDip` (9) |
| 3 | 슬라이더 줄 | 28 |
| 4 | 간격 | `kDivGapDip` (8) |
| 5 | 구분선 | `kDivHDip` (1) |
| 6 | 간격 | `kDivGapDip` (8) |
| 7 | 섹션 헤더 "출력" | `kSectionHDip` (17) |
| 8 | 장치 행 × n | `kRowHDip` (32) |
| 9 | 간격 | `kDivGapDip` (8) |
| 10 | 구분선 | `kDivHDip` (1) |
| 11 | 간격 | `kDivGapDip` (8) |
| 12 | "…설정…" 행 × 1~2 | `kSettingsHDip` (22) |
| 13 | 하단 패딩 | `kBottomPadDip` (10) |

폭은 `panel::kWidthDip`(308)이다. 장치가 하나도 없으면 8번 자리에 "출력 장치가 없습니다"를 muted 색으로 한 줄(`kRowHDip`) 넣고 섹션 헤더는 생략한다. 목록은 최대 8개까지만 보이고 그 이상은 자른다(`kAudioListMax = 8`).

**슬라이더**

- 좌우 끝에 스피커 아이콘을 둔다. 왼쪽은 `\xE992`(작은 소리), 오른쪽은 `\xE995`(큰 소리)이며 크기는 14 DIP, 색은 fg다.
- 트랙은 두 아이콘 사이를 채우고 좌우로 각각 8 DIP 띄운다. 높이 6 DIP, 곡률은 `corner::PillPx`.
- 채워지지 않은 부분은 `BadgeOffFill(dark)`, 채워진 부분은 `AccentFillColor(dark)`.
- 노브는 지름 18 DIP 흰 원이다. 색은 `AccentOnColor(dark)`. 트랙 중심선 위에 놓고, 노브가 트랙 밖으로 나가지 않도록 이동 범위를 노브 반지름만큼 좁힌다.
- 음소거 상태이면 트랙 채움과 노브를 알파 0.40으로 낮춘다.

**드래그**

`DragRow`/`DragTo`/`DragEnd`는 이미 제어 센터 홈의 볼륨 슬라이더가 쓰는 경로가 있다(`kVolume` 히트 아이디). 볼륨 페이지의 슬라이더도 같은 히트 아이디를 재사용하여 `DragTo`가 `SliderTrackRect`가 아니라 **현재 페이지에 맞는 트랙 사각형**을 쓰도록 분기한다. 값 변경은 지금처럼 `host_.dispatch`로 `bamti.widget/volume`에 `row_id="volume_level"` 이벤트를 보낸다.

**장치 행**

- 원 아이콘은 `panel::DrawRowCircle`로 그린다. `active`는 그 장치가 기본 출력 장치일 때 참이다.
- 이름은 `panel::kTextLeftDip`(49)에서 시작하고 `regular14_`로 그린다.
- 기본 장치가 아닌 행의 이름도 fg 색으로 그린다. 맥 화면에서 선택되지 않은 장치도 흐리지 않다.
- 오른쪽에 배터리 백분율을 넣을 수 있으면 넣는다(3단계의 배터리 조회를 공유한다). 못 얻으면 그 자리를 비운다.
- 행을 누르면 `SetDefaultAudioOutput(id)`를 부르고, 성공하면 목록을 곧바로 다시 열거하여 강조 표시를 옮긴다. 패널은 닫지 않는다.

**하단 항목**

- 기본 출력 장치가 블루투스 오디오이면 첫 줄에 "<장치 이름> 설정…"을 넣고, 누르면 `ms-settings:bluetooth`를 연다. 판정이 애매하면 이 줄은 넣지 않아도 된다.
- 마지막 줄은 "사운드 설정…"이고 `ms-settings:sound`를 연다. 기존 `PendingAction::kSoundSettings` 경로를 그대로 쓴다.

### 2-3. 상단바 볼륨 좌클릭 경로

`MenuBar::OpenStatusPanel`에서 네트워크가 하는 것과 같이 볼륨을 갈라낸다.

```cpp
void MenuBar::OpenStatusPanel(const StatusHit& hit) {
  if (hit.id == kNetworkItemId && ShowControlCenter(hit.rect, ControlCenterPage::kWifi)) {
    return;
  }
  if (hit.id == kVolumeItemId && ShowControlCenter(hit.rect, ControlCenterPage::kVolume)) {
    return;
  }
  ...
```

`kVolumeItemId`는 `"bamti.widget/volume"`이며 `menu_bar.cpp`에 상수로 선언한다(지금은 575행에 문자열이 직접 박혀 있으니 그 자리도 상수로 바꾼다). `ToggleNetworkPanel`과 같은 모양의 `ToggleVolumePanel`을 만들어 이미 열려 있으면 닫히게 한다. `ControlCenterContent::ShowsNetwork`와 짝이 되는 판정이 필요하므로, `ShowsNetwork` 대신 **`ControlCenterPage CurrentPage() const`** 를 두어 호출하는 쪽이 페이지를 직접 견주게 바꾸는 편이 낫다. `ShowsNetwork`를 쓰는 자리(`menu_bar.cpp` 1686행, 2033행)를 모두 옮긴다.

휠로 볼륨을 조절하는 경로(`WM_MOUSEWHEEL`)는 그대로 둔다.

`SampleVolume`이 만드는 `StatusPanel`은 이제 상단바에서 쓰이지 않는다. 다만 파이프로 연결된 외부 소비자가 있을 수 있으니 **지우지 말고 그대로 발행한다.**

---

## 3단계 — 블루투스 위젯을 만든다

### 3-1. 블루투스 계층 `src/bt_devices.hpp` / `.cpp`

```cpp
namespace bamti {

struct BtRadioState {
  bool present = false;    // 어댑터가 있는가
  bool on = false;         // 라디오가 켜져 있는가
  bool can_toggle = false; // 소프트웨어로 켜고 끌 수 있는가
};

struct BtDeviceInfo {
  std::wstring name;
  std::wstring address;    // 12자리 16진수, 배터리 조회의 열쇠
  bool connected = false;
  bool paired = false;
  int battery = -1;        // 0~100, 모르면 -1
  BLUETOOTH_DEVICE_INFO raw{};
};

BtRadioState QueryBtRadio();
bool SetBtRadio(bool on);
std::vector<BtDeviceInfo> EnumBtDevices();

}  // namespace bamti
```

**라디오 토글은 지금 코드가 틀렸다.** `control_center.cpp` 2035~2044행은 `BluetoothEnableDiscovery`와 `BluetoothEnableIncomingConnections`를 부르는데, 이 둘은 검색 허용과 수신 연결 허용을 바꿀 뿐 라디오를 켜고 끄지 않는다. 실제 토글은 WinRT의 `Windows.Devices.Radios.Radio`를 써야 한다.

이 저장소는 C++/WinRT를 쓰지 않으므로 **ABI 헤더와 `RoGetActivationFactory`로 직접 부른다.**

- `#include <windows.devices.radios.h>`, `#include <roapi.h>`, `#include <wrl/wrappers/corewrappers.h>`
- `RoInitialize(RO_INIT_MULTITHREADED)`는 이미 `CoInitializeEx`를 부르는 스레드에서는 생략하고, `CoInitializeEx(nullptr, COINIT_MULTITHREADED)`로 갈음할 수 있는지 확인하라.
- `ABI::Windows::Devices::Radios::IRadioStatics`를 얻어 `GetRadiosAsync`로 목록을 받고, `Kind`가 `RadioKind_Bluetooth`인 항목에 `SetStateAsync(RadioState_On | RadioState_Off)`를 보낸다.
- 비동기 결과를 기다리는 데는 `IAsyncOperation`의 `put_Completed`에 이벤트를 걸고 `WaitForSingleObject`로 짧게(1500 ms) 기다린다. **UI 스레드를 막으면 안 되므로 토글은 위젯 워커 스레드나 스레드풀에서 실행하고, 결과가 오면 패널을 다시 그린다.**
- `runtimeobject` 라이브러리를 `CMakeLists.txt`와 `bamti.vcxproj`의 링크 목록에 더한다.
- `RadioAccessStatus`가 `Allowed`가 아니면 `can_toggle`을 거짓으로 두고 토글을 흐리게 그린다. Wi-Fi 토글이 `wifi_hw_radio_on_`을 다루는 방식과 같다.

WinRT 경로를 세우기 어렵다고 판단되면 **토글을 흐리게 두고 누르면 `ms-settings:bluetooth`를 여는 것**으로 물러서라. 다만 그렇게 물러섰다면 반드시 보고하라. 잘못된 API를 계속 부르는 지금 상태로 두는 것만은 안 된다.

**장치 열거**는 지금 `RefreshPageLists`가 하는 것과 같은 `BluetoothFindFirstRadio`/`BluetoothFindFirstDevice` 경로를 쓰되 `bt_devices.cpp`로 옮긴다. 정렬은 연결된 장치를 앞으로 보내고(지금의 `stable_partition` 유지), 그 다음은 이름 오름차순이다. 최대 8개까지 보인다.

**배터리 백분율**은 `SetupDiGetClassDevs`로 블루투스 장치 인터페이스를 훑고 `SetupDiGetDeviceProperty`에 아래 키를 넘겨 읽는다.

- `DEVPKEY_Bluetooth_DeviceAddress` (`{2BD67D8B-8BEB-48D5-87E0-6CDA3428040A} 1`, `DEVPROP_TYPE_STRING`) 으로 장치를 짝지운다.
- 배터리는 `{104EA319-6EE2-4701-BD47-8DDBF425BBE5} 2` (`DEVPROP_TYPE_BYTE`) 를 쓴다.

**이 배터리 키는 문서화되어 있지 않다.** 실기에서 AirPods 같은 장치로 값이 실제로 나오는지 확인하고, 나오지 않으면 배터리 표시를 통째로 빼고 그 사실을 보고하라. 값을 얻지 못한 장치는 백분율 자리를 비우면 되고, 이것이 오류로 취급되어서는 안 된다.

### 3-2. 블루투스 아이콘을 직접 그린다

`VectorIcon`에 `kBluetooth`를 더하고 `ClockRenderer::DrawVectorIcon`에 갈래를 더한다. `DrawEthernetIcon`이 이미 보여 주는 방식, 곧 **`ID2D1PathGeometry`에 선분을 쌓아 `DrawGeometry`로 그리는 방식**을 그대로 따른다.

블루투스 룬(ᛒ)은 세로축 하나와 두 개의 꺾인 선으로 이루어진다. 12 × 18 좌표계에서 아래 다섯 점을 잇는다.

```
      (6, 0)          윗 꼭짓점
        │
 (1,4.5)┼(11,4.5)     위쪽 날개 교차
        │
      (6, 9)          가운데
        │
 (1,13.5)┼(11,13.5)   아래쪽 날개 교차
        │
      (6, 18)         아래 꼭짓점
```

한 획으로 잇는다. `(1, 4.5) → (6, 0) → (6, 18) → (11, 13.5) → (1, 4.5)`가 아니라, 맥 아이콘처럼 **`(1, 13.5) → (11, 4.5) → (6, 0) → (6, 18) → (11, 13.5) → (1, 4.5)`** 순서로 이어야 두 날개가 가운데 축과 제대로 만난다. 선 굵기는 높이의 `1.7 / 18`이고, `EnsureStroke()`가 만드는 둥근 끝 스트로크 스타일을 쓴다.

상단바 아이콘 크기는 높이 14 DIP, 폭은 그 `12/18`인 약 9.3 DIP다. `bar_layout.cpp`가 이더넷에 폭을 따로 주는 것처럼(`kEthernetIconDip`, `kEthernetIconHeightDip`) `kBluetoothIconDip`, `kBluetoothIconHeightDip`을 `bar_layout.hpp`에 더하고 `clock_renderer.cpp`의 폭 분기(710행 근처)에 갈래를 더한다.

색은 라디오 상태에 따른다. 켜져 있으면 `ClockTextColor(dark)`, 꺼져 있으면 `ScaleAlpha(ClockTextColor(dark), 0.45f)`다. Wi-Fi 아이콘이 `value < 0.5f`로 가르는 방식과 같게 `icon.value`에 켜짐 여부를 실어 보낸다.

### 3-3. 블루투스 위젯 발행

`src/widgets/builtin.cpp`에 다음을 더한다.

- `constexpr char kBluetoothId[] = "bamti.widget/bluetooth";`
- `constexpr int kBluetoothPriority = 37;` (네트워크 36보다 앞이므로 상단바에서 네트워크 왼쪽에 놓인다. 실제 순서를 보고 필요하면 조정하라.)
- `constexpr ULONGLONG kBluetoothPeriodMs = 5000;`
- `void SampleBluetooth();`와 `bluetooth_due_`, `last_bt_on_`, `last_bt_name_`, `fp_bluetooth_` 멤버.

`SampleBluetooth`는 `SampleNetwork`와 같은 골격이다. `QueryBtRadio()`를 부르고, 어댑터가 없으면 `DropItem(kBluetoothId)`로 상단바에서 뺀다. 있으면 `SetVectorIcon(&item, VectorIcon::kBluetooth, on ? 1.0f : 0.0f, 0)`으로 아이콘을 싣고, 툴팁은 연결된 장치가 있으면 "Bluetooth · <장치 이름>", 없으면 "Bluetooth · 켜짐" 또는 "Bluetooth · 꺼짐"으로 한다. `StatusPanel`은 붙이지 않는다. 패널은 제어 센터가 그린다.

`SampleDue`와 `NextDeadlineLocked`, `ResetBaselines`에 새 주기를 엮는다. 발행 조건은 `settings_.bluetooth`이며, 제어 센터 홈의 블루투스 타일도 상태를 쓰므로 조회 조건은 `settings_.bluetooth || settings_.control_center`다.

`LiveForControlCenter`가 돌려주는 `ControlCenterLive`에 `bt_on`과 `bt_name`을 더해 제어 센터가 자기 스레드에서 다시 조회하지 않게 한다.

### 3-4. 블루투스 페이지를 다시 그린다

`RenderListPage`가 그리던 옛 블루투스 페이지를 버리고, 네트워크 패널과 같은 배치로 다시 그린다.

| 순서 | 요소 | 높이 |
| --- | --- | --- |
| 1 | 제목 "Bluetooth" + 오른쪽 토글 | `kHeaderHDip` (24) |
| 2 | 간격 | `kHeaderGapDip` (9) |
| 3 | 구분선 | `kDivHDip` (1) |
| 4 | 간격 | `kDivGapDip` (8) |
| 5 | 장치 행 × n | `kRowHDip` (32) |
| 6 | 간격 | `kDivGapDip` (8) |
| 7 | 구분선 | `kDivHDip` (1) |
| 8 | 간격 | `kDivGapDip` (8) |
| 9 | "Bluetooth 설정…" | `kSettingsHDip` (22) |
| 10 | 하단 패딩 | `kBottomPadDip` (10) |

폭은 `panel::kWidthDip`(308)이다. 목표 화면과 달리 섹션 헤더는 없다.

- 라디오가 꺼져 있으면 5번 자리에 "Bluetooth가 꺼져 있습니다"를 muted로 한 줄 넣는다. 어댑터가 없으면 "Bluetooth 어댑터가 없습니다"다.
- 장치가 하나도 없으면 "연결된 장치가 없습니다"를 넣는다.
- 각 행의 원 아이콘은 연결된 장치일 때 `active`다. 원 안 글리프는 장치 종류에 따른다. `BLUETOOTH_DEVICE_INFO::ulClassofDevice`의 상위 비트로 오디오, 키보드, 마우스를 가른 뒤 `\xE7F6`(헤드폰), `\xE765`(키보드), `\xE962`(마우스), `\xE702`(기본 블루투스 글리프)를 고른다. **글리프 모양은 눈으로 확인하라.**
- 연결이 끊긴 장치는 이름을 muted 색으로 그린다. 맥 화면에서 `BT5.0 KB`가 흐린 것과 같다.
- 배터리를 얻은 장치는 오른쪽 끝에 `regular12_` muted 색으로 백분율을 넣는다.
- 행을 누르면 지금처럼 `BluetoothAuthenticateDeviceEx`를 부른다. 이미 짝지어진 장치라면 연결을 시도하는 편이 자연스럽지만, 짝짓기 해제 없이 연결만 거는 공개 API가 마땅치 않다. **짝지어진 장치의 행을 누르면 `ms-settings:bluetooth`를 여는 것으로 두고, 더 나은 방법을 찾으면 보고하라.**
- 토글을 누르면 `SetBtRadio`를 부른다.

### 3-5. 상단바 좌클릭 경로

`kBluetoothItemId`(`"bamti.widget/bluetooth"`)를 `menu_bar.cpp`에 상수로 두고, `WM_LBUTTONUP`의 갈래에 네트워크와 같은 모양으로 더한다.

```cpp
if (hit->id == kBluetoothItemId) {
  ToggleBluetoothPanel(*hit);
  return 0;
}
```

`OpenStatusPanel`도 `ControlCenterPage::kBluetooth`로 갈라낸다.

### 3-6. Windows 블루투스 트레이 아이콘

`TrayMirror`가 비추는 Windows 블루투스 아이콘과 새 위젯이 함께 보이면 아이콘이 둘이 된다. 자동으로 숨기지는 마라. 트레이 키는 실행 주체마다 달라 잘못 짚으면 엉뚱한 아이콘을 지운다. **작업을 마친 뒤 사용자에게 트레이 메뉴에서 직접 숨기면 된다는 사실만 알린다.**

---

## 4단계 — 설정과 메뉴

`WidgetSettings`에 `bool bluetooth = false;`를 더한다.

- `Any()`에 더한다.
- `src/settings.cpp`의 직렬화(135행 근처)와 역직렬화(235행 근처)에 `"bluetooth"` 키를 더한다.
- `bar_order`의 기본 순서에 `"bamti.widget/bluetooth"`가 들어갈 자리를 정한다. 옛 설정 파일에는 이 아이디가 없으므로, 없으면 네트워크 왼쪽에 끼워 넣는다. 네트워크가 `wifi`에서 이관된 방식(264~281행)을 참고하라.
- `menu_bar.cpp`의 막대 메뉴에 `kWidgetBluetoothCmd`를 더하고 "블루투스" 항목을 넣는다(1970행 근처). 명령 번호는 쓰이지 않는 값을 고른다.
- 위젯 토글 처리(742~759행)에 갈래를 더한다.

---

## 검증

### 빌드

```powershell
cmake --build out/cmake-debug --config Debug
```

경고 없이 통과해야 한다. `/W4`이므로 쓰지 않는 매개변수와 부호 비교에 주의하라.

### 화면 확인

앱을 띄우고 아래를 눈으로 확인한다. **각 패널의 스크린샷을 남기고 목표 화면과 견주어 어긋난 곳을 보고하라.**

1. 네트워크 패널이 1단계 이전과 한 픽셀도 다르지 않다.
2. 볼륨 패널의 폭이 308이고 네트워크 패널과 나란히 놓았을 때 인셋, 행 높이, 구분선 위치, 글꼴 크기가 모두 같다.
3. 볼륨 슬라이더를 끌면 소리가 따라 바뀌고, 노브가 트랙 밖으로 나가지 않는다.
4. 볼륨 패널에서 다른 출력 장치를 누르면 강조 표시가 그 줄로 옮겨 가고, 소리가 실제로 그 장치에서 난다.
5. 블루투스 아이콘이 상단바에 직접 그린 모양으로 나오고, 다크 모드와 라이트 모드에서 모두 또렷하다.
6. 블루투스 아이콘을 좌클릭하면 패널이 열리고, 다시 누르면 닫힌다.
7. 블루투스 패널의 장치 목록이 실제 짝지어진 장치와 맞고, 연결된 장치가 위에 온다.
8. 세 패널의 모서리 곡률이 모두 같다(`CornerDip`이 `corner::kHeroDip`).

### 하지 말 것

- **블루투스 라디오를 실제로 끄는 검증은 하지 마라.** 블루투스 키보드나 마우스를 쓰고 있으면 조작이 끊긴다. 토글을 누른 뒤 상태 조회가 바뀌는지는 로그로 확인하고, 실제 끄기는 사용자에게 부탁하라.
- **레지스트리에 쓰지 마라.** 기본 오디오 장치 전환은 `IPolicyConfig`로만 한다.
- **사용자 화면에 입력을 합성하지 마라.** 클릭이 필요한 확인은 준비만 해 두고 사용자에게 부탁하라.

### 성능

`Log(L"cc", ...)`에 아래를 한 번씩 남기고 넘는 것이 있으면 보고하라.

- 오디오 장치 열거 시간 (5 ms 이하)
- 블루투스 장치 열거 시간 (10 ms 이하)
- 배터리 조회 시간 (10 ms 이하)
- 패널 열기부터 첫 표시까지 (`ShowControlCenter`의 `open to present` 로그, 60 ms 이하)

블루투스 장치 열거와 배터리 조회가 느리면 UI 스레드에서 부르지 말고 위젯 워커에서 미리 받아 `ControlCenterLive`에 실어 보내라.

---

## 위험 요소

| 항목 | 위험 | 대응 |
| --- | --- | --- |
| `IPolicyConfig` | 비공개 인터페이스라 판올림에서 깨질 수 있다 | 실패를 조용히 삼키고 로그만 남긴다. 앱이 죽지 않게 한다 |
| WinRT 라디오 토글 | 이 저장소에 WinRT 기반이 없다 | 세우기 어려우면 설정 앱을 여는 것으로 물러서고 보고한다 |
| 블루투스 배터리 키 | 문서화되어 있지 않다 | 값이 안 나오면 표시를 빼고 보고한다 |
| Fluent 글리프 코드 | 지시서의 코드가 다른 모양일 수 있다 | 눈으로 확인하고 바꾼 것을 보고한다 |
| 1단계 리팩터링 | 네트워크 패널이 어긋날 수 있다 | 배치 값을 바꾸기 전후로 견주어 확인한다 |

## 작업 순서

1단계를 끝내고 네트워크 패널이 그대로인지 확인한 다음 2단계로 넘어가라. 2단계와 3단계는 서로 기대지 않으므로 순서를 바꾸어도 된다. 각 단계를 마칠 때마다 빌드하고, 무엇을 확인했는지 보고하라.

---

# 덧붙임 — 배터리와 CPU 패널도 같은 디자인 언어로 옮긴다

2026-09-05에 사용자가 더한 요구다. 앞의 1단계부터 4단계까지를 그대로 두고, 아래 5단계와 6단계를 이어서 진행한다.

## 5단계 — 배터리 패널과 CPU 패널

### 지금 상태

배터리와 CPU는 `BuiltinWidgets::SampleBattery`와 `SampleCpu`가 `StatusPanel`을 만들어 붙이고, `StatusPanelContent`(`src/status_panel.cpp`)가 그린다. 이쪽 치수는 네트워크 패널과 전혀 다르다.

| 항목 | `StatusPanelContent` | `panel::` 토큰 |
| --- | --- | --- |
| 패널 여백 | 12 | 14 |
| 폭 | 280~360 가변 | 308 고정 |
| 행 높이 | 항목마다 18~28 | 32 |
| 블록 간격 | 18 | 8 |
| 창 곡률 | `corner::kOverlayDip` (8) | `corner::kHeroDip` (16) |

여백과 곡률과 행 높이가 모두 어긋나므로, 네트워크 패널 옆에 나란히 놓으면 다른 프로그램의 창처럼 보인다.

### 방침

**배터리와 CPU도 `ControlCenterContent`의 전용 페이지로 옮긴다.** `Page`에 `kBattery`와 `kCpu`를 더하고, `ControlCenterPage`에도 같은 값을 더한다. 볼륨이 2단계에서 옮겨 가고 나면 내장 위젯 가운데 `StatusPanelContent`에 남는 것이 없어야 한다.

**목록에서 무언가를 고르는 기능은 넣지 않는다.** 배터리와 CPU에는 고를 대상이 없다. 배치와 간격과 여백과 곡률만 맞추는 작업이다. 다만 절전 모드 토글은 예외이며 아래 5-3에서 따로 다룬다.

### 5-1. 배터리 페이지

```
┌────────────────────────────────────┐
│ 배터리                        85%  │   제목 + 오른쪽에 백분율
│ ──────────────────────────────────  │
│  ▓▓▓▓▓▓▓▓▓▓▓▓▓░░░░░░░░░░░          │   게이지 바
│  2시간 15분 남음                    │   보조 줄, 남은 시간을 모르면 생략
│ ──────────────────────────────────  │
│ 절전 모드                    (◯━)  │   토글 행
│ 전원                        연결됨  │   키값 행
│ ──────────────────────────────────  │
│ 전원 설정…                          │
└────────────────────────────────────┘
```

| 순서 | 요소 | 높이 |
| --- | --- | --- |
| 1 | 제목 "배터리" + 오른쪽 백분율 | `panel::kHeaderHDip` (24) |
| 2 | 간격 | `panel::kHeaderGapDip` (9) |
| 3 | 게이지 바 | 10 |
| 4 | 남은 시간 줄 (있을 때만) | `panel::kNoteHDip` (16) |
| 5 | 간격 | `panel::kDivGapDip` (8) |
| 6 | 구분선 | `panel::kDivHDip` (1) |
| 7 | 간격 | `panel::kDivGapDip` (8) |
| 8 | 절전 모드 토글 행 | `panel::kRowHDip` (32) |
| 9 | 전원 키값 행 | `panel::kRowHDip` (32) |
| 10 | 간격 | `panel::kDivGapDip` (8) |
| 11 | 구분선 | `panel::kDivHDip` (1) |
| 12 | 간격 | `panel::kDivGapDip` (8) |
| 13 | "전원 설정…" | `panel::kSettingsHDip` (22) |
| 14 | 하단 패딩 | `panel::kBottomPadDip` (10) |

폭은 `panel::kWidthDip`(308)이다. 1단계의 `panel::Stack`으로 `MakeBatteryPage`를 만든다.

- 제목 오른쪽의 백분율은 `semibold14_`로 fg 색이며 오른쪽 정렬이다.
- 게이지 바는 좌우 인셋을 뺀 폭 전체를 쓴다. 높이 10 DIP, 곡률은 `corner::PillPx`. 바탕은 `BadgeOffFill(dark)`, 채움은 `BatteryFillRgb(dark, level, ac)`가 돌려주는 색이다. 이 함수는 이미 `theme.hpp`에 있으니 그대로 쓴다.
- 남은 시간 줄은 `regular12_` muted 색이다. `RemainText`가 빈 문자열을 돌려주거나 전원이 연결되어 있으면 이 줄을 통째로 뺀다.
- 키값 행은 왼쪽에 `regular14_` fg 색 라벨, 오른쪽 끝에 `regular14_` muted 색 값이다. 아이콘 원은 넣지 않는다. 왼쪽 시작 위치는 `panel::kInsetDip`(14)이며 `panel::kTextLeftDip`(49)이 아니다. 원 아이콘이 없기 때문이다.
- 키값 행에는 호버를 깔지 않는다. 누를 수 없는 행이기 때문이다.

### 5-2. CPU 페이지

```
┌────────────────────────────────────┐
│ CPU                           23%  │
│ ──────────────────────────────────  │
│  ▓▓▓▓▓░░░░░░░░░░░░░░░░░░░          │
│ ──────────────────────────────────  │
│ 사용자                        15%  │
│ 커널                           8%  │
│ 논리 프로세서                   16  │
│ ──────────────────────────────────  │
│ 작업 관리자…                        │
└────────────────────────────────────┘
```

배터리 페이지와 같은 배치이며, 보조 줄과 토글 행이 없고 키값 행이 셋이다. 게이지 채움 색은 `CpuFillRgb(dark, level)`을 쓴다. 하단 항목은 "작업 관리자…"이고 기존 `PendingAction::kTaskManager` 경로를 그대로 쓴다.

CPU 사용률은 5초마다 바뀐다. 패널이 열려 있는 동안 값이 갱신되어야 하므로, `ControlCenterLive`에 `cpu_ok`, `cpu_usage`, `cpu_user`, `cpu_kernel`, `cpu_nproc`를 더해 `ApplyLive`가 받아 가게 한다. 배터리도 마찬가지로 `battery_ok`, `battery_level`, `battery_ac`, `battery_charging`, `battery_remain_text`, `battery_saver_on`을 더한다. **제어 센터가 자기 스레드에서 `GetSystemPowerStatus`나 `ReadCpuTimes`를 다시 부르지 않게 하라.** 값은 위젯 워커가 이미 가지고 있다.

### 5-3. 절전 모드 토글

배터리 페이지의 절전 모드 행은 읽기만 하는 키값이 아니라 **눌러서 켜고 끌 수 있는 토글**이다. 토글 모양과 크기는 `panel::DrawToggle`로 그리며 Wi-Fi와 블루투스의 토글과 같다. 다만 행의 오른쪽 끝에 놓이므로 헤더의 토글과 위치만 다르다.

현재 상태를 읽는 것은 이미 되어 있다. `SYSTEM_POWER_STATUS::SystemStatusFlag`의 최하위 비트가 절전 모드다(`control_center.cpp` 1047행, `builtin.cpp` 876행).

**문제는 켜고 끄는 쪽이다. Windows에는 절전 모드를 즉시 켜고 끄는 공개 API가 없다.** 아래 순서로 시도하고, 어디까지 되었는지 반드시 보고하라.

**1순위 — `powrprof.dll`의 전원 계획 API**

절전 모드가 발동하는 배터리 임계값을 100으로 올리면 켜지고, 0으로 내리면 꺼진다. 레지스트리를 직접 쓰지 않고 문서화된 함수로 처리한다.

```cpp
// SUB_ENERGYSAVER
constexpr GUID kSubEnergySaver = {0xde830923, 0xa562, 0x41af, {0xa0, 0x86, 0xe3, 0xa2, 0xc6, 0xba, 0xd2, 0xda}};
// ESBATTTHRESHOLD
constexpr GUID kEsBattThreshold = {0xe69653ca, 0xcf7f, 0x4f05, {0xaa, 0x73, 0xcb, 0x83, 0x3f, 0xa9, 0x0a, 0xd4}};
```

`PowerGetActiveScheme`으로 활성 계획을 얻고, `PowerReadDCValueIndex`로 현재 임계값을 읽고, `PowerWriteDCValueIndex`로 새 값을 쓴 뒤 `PowerSetActiveScheme`으로 적용한다. 이 GUID와 함수 이름은 **반드시 실기에서 확인하라.**

**되돌리기가 이 작업에서 가장 중요하다.**

- 절전 모드를 켜기 전에 읽은 원래 임계값을 `WidgetSettings`에 `int saver_threshold_backup = -1;`로 저장하고 설정 파일에 적는다. 메모리에만 두면 앱이 죽었을 때 사용자의 전원 설정이 100으로 남는다.
- 끌 때는 저장해 둔 값으로 되돌린다. 저장된 값이 없으면 Windows 기본값인 20으로 되돌린다.
- 앱이 시작될 때 `saver_threshold_backup`이 `-1`이 아니면, 지난번에 되돌리지 못하고 끝났다는 뜻이다. 그 값으로 되돌리고 `-1`로 지운다.
- 값을 쓰기 전후로 `Log(L"power", L"saver threshold %d -> %d", old, next)`를 남긴다.

**AC 전원에 연결되어 있으면 이 방법은 듣지 않는다.** 절전 모드는 배터리로 돌 때만 발동하기 때문이다. AC에 연결된 동안에는 토글을 흐리게 그리고, 누르면 아무 일도 하지 않는다. Wi-Fi 토글이 `wifi_hw_radio_on_`을 다루는 방식과 같다.

**2순위 — 물러서기**

`PowerWriteDCValueIndex`가 권한 부족으로 실패하거나, 값을 바꿔도 `SystemStatusFlag`가 따라오지 않으면 이 경로를 버린다. 토글 대신 "절전 모드" 키값 행에 현재 상태만 보이고, 그 행을 누르면 `ms-settings:batterysaver`를 연다. 물러섰다면 **무엇이 어떻게 실패했는지 반드시 보고하라.**

**금지**

- **레지스트리에 직접 쓰지 마라.** `HKLM\SYSTEM\CurrentControlSet\Control\Power` 아래를 건드리는 방법이 인터넷에 널리 돌아다니지만 쓰지 않는다. 되돌리지 못하면 사용자의 전원 설정이 망가진다.
- 관리자 권한을 요구하는 경로로 가지 마라. 이 앱은 사용자 권한으로 돈다.

### 5-4. 상단바 좌클릭 경로

`kBatteryItemId`(`"bamti.widget/battery"`)와 `kCpuItemId`(`"bamti.widget/cpu"`)를 `menu_bar.cpp`에 상수로 두고, `OpenStatusPanel`에서 각각 `ControlCenterPage::kBattery`와 `ControlCenterPage::kCpu`로 갈라낸다. 2단계에서 볼륨을 갈라낸 것과 같은 모양이며, 다시 누르면 닫히는 토글 동작도 같다.

갈래가 다섯이 되므로 `if` 를 늘어놓지 말고 표로 만들어라.

```cpp
// 상단바 항목과 제어 센터 페이지의 짝. 여기에 없는 항목만 StatusPanelContent로 간다.
struct WidgetPage {
  const char* id;
  ControlCenterPage page;
};
constexpr WidgetPage kWidgetPages[] = {
    {kNetworkItemId, ControlCenterPage::kWifi},
    {kVolumeItemId, ControlCenterPage::kVolume},
    {kBluetoothItemId, ControlCenterPage::kBluetooth},
    {kBatteryItemId, ControlCenterPage::kBattery},
    {kCpuItemId, ControlCenterPage::kCpu},
};
```

### 5-5. `SampleBattery`와 `SampleCpu`의 `StatusPanel`

두 함수가 만드는 `StatusPanel`은 이제 상단바에서 쓰이지 않는다. 볼륨과 마찬가지로 **지우지 말고 그대로 발행한다.** 파이프로 연결된 외부 소비자가 읽고 있을 수 있다.

---

## 6단계 — `StatusPanelContent`의 곡률과 여백을 맞춘다

5단계를 마치면 `StatusPanelContent`는 파이프로 들어온 외부 패널만 그린다. 그래도 상단바에서 열리는 창이므로 다른 패널과 나란히 보인다. 배치까지 뜯어고칠 필요는 없고 겉모습만 맞춘다.

- `CornerDip()`을 재정의해 `corner::kHeroDip`을 돌려준다. 지금은 기본값 `corner::kOverlayDip`이라 모서리가 덜 둥글다.
- `kPanelPadDip`을 12에서 `panel::kInsetDip`(14)으로 바꾼다.
- `kPanelMinWidthDip`을 280에서 `panel::kWidthDip`(308)로 바꾼다. `kPanelMaxWidthDip`(360)은 그대로 둔다. 외부 패널은 내용의 폭을 미리 알 수 없으므로 가변 폭을 남긴다.
- 호버 곡률이 `corner::HoverPx`를 쓰는지 확인하고, 아니면 그렇게 바꾼다.

**행 높이와 블록 간격은 건드리지 마라.** 외부 패널은 게이지와 토글과 버튼을 임의로 섞어 보내므로, 행 높이를 32로 못 박으면 오히려 어긋난다.

---

## 덧붙인 부분의 검증

빌드와 화면 확인은 앞의 검증 절과 같고, 아래를 더한다.

1. 배터리 패널과 CPU 패널의 폭이 308이고, 네트워크 패널과 나란히 놓았을 때 여백과 구분선 위치와 모서리 곡률이 모두 같다.
2. 다섯 패널(네트워크, 볼륨, 블루투스, 배터리, CPU)을 차례로 열어 스크린샷을 남기고, 겹쳐 보아 어긋난 곳이 없는지 확인한다.
3. CPU 패널을 열어 둔 채로 10초 이상 두었을 때 숫자와 게이지가 갱신된다.
4. 배터리가 없는 기기에서는 배터리 항목이 상단바에 나오지 않는다(`DropItem` 경로가 그대로 산다).

### 절전 모드 토글에서 하지 말 것

- **절전 모드를 실제로 켜고 끄는 검증을 반복하지 마라.** 전원 계획 값을 건드리는 작업이므로 한 번 켜고 한 번 끈 뒤, 임계값이 원래 값으로 돌아왔는지 `PowerReadDCValueIndex`로 확인하는 것으로 끝낸다.
- 확인 뒤에는 `saver_threshold_backup`이 `-1`로 지워졌는지 설정 파일을 열어 보고 확인하라.
- AC 전원에 연결된 상태로 검증했다면 토글이 흐리게 나오는 것까지만 확인하고, 실제 동작 확인은 사용자에게 부탁하라.
