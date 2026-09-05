# 수정 지시서: 네트워크 패널의 Wi-Fi 토글과 비밀번호 연결을 고친다

`TASK-NETWORK-WIDGET.md` 구현물의 검수에서 찾은 결함 두 개를 고칩니다.
건드리는 파일은 `src/control_center.cpp`, `src/control_center.hpp`이고, 3절에서 입력 창을 만들면
새 파일 `src/wifi_password.cpp`와 `src/wifi_password.hpp`가 늘어납니다.

레이아웃은 사용자 확인을 마쳤습니다. **6절 수치와 그리는 코드는 건드리지 마십시오.**

---

## 1. 측정으로 확인한 것

`~/.bamti/bamti.log`에서 잰 값입니다. 추측이 아닙니다.

```
2026-09-05 09:54:56.134 [cc] wifi radio set 0 err=87
2026-09-05 09:56:20.849 [cc] wifi radio set 0 err=87
2026-09-05 09:55:13.296 [cc] wifi connect [LG_StickVacuum]da0f err=0
2026-09-05 09:55:16.470 [cc] wifi connect KT_GiGA_0C3A err=0
```

여기서 두 가지가 확정됩니다.

1. **토글의 히트 판정은 정상입니다.** 로그가 남았으므로 클릭은 `Invoke`까지 들어왔습니다.
   실패한 것은 `WlanSetInterface` 호출이고 오류 87은 `ERROR_INVALID_PARAMETER`입니다.
2. **연결 호출은 성공을 돌려주고 있습니다.** `err=0`은 연결이 됐다는 뜻이 아니라 요청을 접수했다는
   뜻입니다. `WlanConnect`는 비동기이고, 자격 증명이 없는 망에서는 접수만 되고 곧 조용히 실패합니다.

---

## 2. 결함 1: 라디오 토글

### 2-1. 원인

`ControlCenterContent::Invoke`의 `kPageToggle` 분기가 이렇게 부릅니다.

```cpp
WLAN_RADIO_STATE state{};
state.dwNumberOfPhys = 1;
state.PhyRadioState[0] = phy;
WlanSetInterface(handle, &wifi_iface_, wlan_intf_opcode_radio_state, sizeof(state), &state, nullptr);
```

`wlan_intf_opcode_radio_state`에 **`WlanSetInterface`로 넘겨야 하는 자료형은 `WLAN_PHY_RADIO_STATE`
하나**입니다. `WLAN_RADIO_STATE`는 `WlanQueryInterface`가 돌려주는 쪽의 자료형입니다. 크기와 내용이
모두 어긋나 있어서 87이 나옵니다.

`phy.dwPhyIndex`를 채우지 않은 것도 문제입니다. 값이 0으로 남아 있는데, 이 인터페이스에 유효한 PHY
인덱스가 0이라는 보장이 없습니다.

### 2-2. 수정

`WlanQueryInterface`로 현재 `WLAN_RADIO_STATE`를 먼저 읽어 유효한 PHY 인덱스를 얻고, 그 인덱스마다
`WLAN_PHY_RADIO_STATE` 하나씩을 `WlanSetInterface`에 넘기십시오.

```cpp
DWORD size = 0;
PWLAN_RADIO_STATE cur = nullptr;
WlanQueryInterface(handle, &wifi_iface_, wlan_intf_opcode_radio_state, nullptr, &size,
                   reinterpret_cast<PVOID*>(&cur), nullptr);
```

- `cur->dwNumberOfPhys`만큼 돌면서 `cur->PhyRadioState[i].dwPhyIndex`를 그대로 옮겨 담고,
  `dot11SoftwareRadioState`만 원하는 값으로 바꿔서 넘깁니다.
- `dot11HardwareRadioState`는 설정할 수 없습니다. 물리 스위치의 상태이므로 읽기 전용입니다.
  이 값이 `dot11_radio_state_off`이면 소프트웨어로 켜도 라디오는 꺼진 채입니다.
  그때는 토글을 그리되 누르면 아무 일도 일어나지 않으므로, **하드웨어가 꺼져 있으면 토글을 흐리게
  그리고 누름을 무시하십시오.**
- 조회 결과는 `WlanFreeMemory`로 반드시 돌려주십시오.
- 성공과 실패를 지금처럼 로그로 남기되, PHY 인덱스와 개수도 함께 남기십시오.

```
Log(L"cc", L"wifi radio set %d phys=%lu idx=%lu err=%lu", ...);
```

### 2-3. 켜고 끈 뒤의 갱신

라디오 상태 변경은 즉시 반영되지 않습니다. 지금은 `RefreshPageLists(true)`를 곧바로 불러서 바뀌기 전
상태를 다시 읽습니다. 4절에서 붙이는 알림을 받아 갱신하거나, 그것이 어려우면 `list_due_`를 지금보다
짧게(300ms 정도) 잡아 다음 틱에서 다시 읽게 하십시오. **`Sleep`으로 기다리지 마십시오.** UI 스레드입니다.

---

## 3. 결함 2: 비밀번호를 받아 연결한다

### 3-1. 지금 무엇이 빠져 있는가

`WlanConnect`에는 자격 증명을 묻는 UI가 없습니다. 비밀번호 창이 뜨지 않는 것은 버그가 아니라 이 API의
성질입니다. 프로필이 없는 보안 망에 붙으려면 **우리가 비밀번호를 받아 프로필을 만들어 두고 나서**
연결해야 합니다.

지금 코드는 프로필이 없을 때 `wlan_connection_mode_discovery_secure`로 부르는데, 이 방식은 이미
저장된 자격 증명이 있을 때만 성공합니다. 없으면 접수만 되고 끝납니다.

### 3-2. 입력 창

패널 안의 그 행 자리에서 바로 입력받는 것이 목표입니다. 다만 `PopupSurface`는
`WS_EX_NOACTIVATE`로 만들어져 있어(`popup_surface.cpp:215`) 키보드 포커스를 받지 못합니다.
**팝업에서 이 스타일을 벗기지 마십시오.** 바깥 클릭으로 닫히는 동작이 이 스타일에 묶여 있고,
`FIX-MENU-DISMISS.md`에서 이미 그 문제를 다뤘습니다.

대신 **비밀번호를 받을 때만 작은 활성 창을 하나 띄우고, 그 창을 그 행 위에 정확히 겹치십시오.**
사용자 눈에는 그 행이 입력란으로 바뀐 것처럼 보입니다. Spotlight가 이미 같은 방식을 씁니다
(`spotlight.cpp:1241` 부근의 `AttachThreadInput` + `SetForegroundWindow` + `SetFocus(edit_)`).

- 자식 컨트롤은 진짜 `EDIT`를 쓰고 `ES_PASSWORD`를 주십시오. 붙여넣기와 IME와 커서가 공짜로 따라옵니다.
  직접 그린 입력란은 붙여넣기가 안 되고, 비밀번호는 붙여넣기를 많이 씁니다.
- 창의 배경과 모서리는 패널의 행과 같게 맞추십시오. 곡률은 `corner::HoverPx(행 높이, dpi)`입니다.
- Enter는 연결, Esc는 취소입니다. 창을 닫으면 팝업의 원래 행으로 돌아갑니다.
- 이 창이 뜨면 팝업이 포커스를 잃습니다. **팝업이 그 이유로 닫히지 않게 하십시오.**
  `PopupSurface`에 이미 `SetAllied`로 짝 창을 예외 처리하는 길이 있습니다(`popup_surface.cpp:320`,
  `408`). 그 경로를 그대로 쓸 수 없으면 같은 판정을 하나 더 만드십시오.
- **비밀번호를 로그에 남기지 마십시오.** SSID와 결과 코드만 남깁니다. 화면 캡처를 보고서에 넣을 때도
  입력 중인 값이 찍히지 않게 하십시오.
- 메모리에서도 오래 들고 있지 마십시오. 프로필을 만든 직후 버퍼를 덮어쓰고 비웁니다.

### 3-3. 프로필을 만든다

`WlanSetProfile`에 XML을 넘겨 프로필을 만든 뒤 `wlan_connection_mode_profile`로 연결하십시오.

인증과 암호 방식은 `WLAN_AVAILABLE_NETWORK`의 두 필드로 판정합니다. 지금은 `bSecurityEnabled`만
읽고 있으므로 `WifiNetwork`에 두 값을 더해야 합니다.

| `dot11DefaultAuthAlgorithm` | XML `<authentication>` |
| --- | --- |
| `DOT11_AUTH_ALGO_RSNA_PSK` | `WPA2PSK` |
| `DOT11_AUTH_ALGO_WPA_PSK` | `WPAPSK` |
| `DOT11_AUTH_ALGO_80211_OPEN` (암호화 있음) | `open` + WEP |
| 그 밖 | 프로필을 만들지 말고 3-5절로 물러섭니다 |

| `dot11DefaultCipherAlgorithm` | XML `<encryption>` |
| --- | --- |
| `DOT11_CIPHER_ALGO_CCMP` | `AES` |
| `DOT11_CIPHER_ALGO_TKIP` | `TKIP` |
| `DOT11_CIPHER_ALGO_WEP`, `WEP40`, `WEP104` | `WEP` |

XML의 뼈대입니다. `<name>`과 `<hex>`와 `<keyMaterial>`만 채우면 됩니다.

```xml
<?xml version="1.0"?>
<WLANProfile xmlns="http://www.microsoft.com/networking/WLAN/profile/v1">
  <name>SSID</name>
  <SSIDConfig><SSID><hex>SSID를 16진수로</hex><name>SSID</name></SSID></SSIDConfig>
  <connectionType>ESS</connectionType>
  <connectionMode>manual</connectionMode>
  <MSM><security>
    <authEncryption>
      <authentication>WPA2PSK</authentication>
      <encryption>AES</encryption>
      <useOneX>false</useOneX>
    </authEncryption>
    <sharedKey>
      <keyType>passPhrase</keyType>
      <protected>false</protected>
      <keyMaterial>비밀번호</keyMaterial>
    </sharedKey>
  </security></MSM>
</WLANProfile>
```

- **SSID는 `<hex>`로 넣으십시오.** `<name>`만 넣으면 한글이나 특수 문자가 든 SSID에서 어긋납니다.
  UTF-8 바이트를 대문자 16진수로 이어 붙입니다.
- **`<keyMaterial>`에 넣기 전에 XML 특수 문자를 반드시 escape하십시오.** `&`, `<`, `>`가 든 비밀번호가
  흔합니다. escape를 빼먹으면 프로필이 깨지고 원인을 찾기 어렵습니다.
- `connectionMode`는 `manual`입니다. `auto`로 두면 사용자가 고르지 않은 망에 저절로 붙습니다.
- `WlanSetProfile`의 `dwFlags`는 0(모든 사용자)이 아니라 `WLAN_PROFILE_USER`를 쓰십시오.
  모든 사용자 프로필은 관리자 권한을 요구합니다.
- `bOverwrite`는 `TRUE`로 둡니다. 같은 SSID로 다시 시도할 때 옛 비밀번호가 남으면 계속 실패합니다.
- 반환된 `WLAN_REASON_CODE`가 `WLAN_REASON_CODE_SUCCESS`가 아니면
  `WlanReasonCodeToString`으로 문자열을 얻어 로그에 남기십시오.

### 3-4. 연결 결과를 안다

지금은 연결이 됐는지 실패했는지 알 방법이 없습니다. 목록만 다시 읽고 있어서 사용자에게 아무 신호가
가지 않습니다.

`WlanRegisterNotification`으로 `WLAN_NOTIFICATION_SOURCE_ACM`을 등록하고
`wlan_notification_acm_connection_complete`를 받으십시오. 알림의 `pData`는
`WLAN_CONNECTION_NOTIFICATION_DATA`이고 `wlanReasonCode`에 실패 사유가 들어옵니다.

- **콜백은 WLAN 서비스가 만든 스레드에서 옵니다.** 거기서 UI를 만지지 마십시오. 상태만 저장하고
  `PostMessage`로 UI 스레드에 알린 뒤 그쪽에서 다시 그립니다.
- 등록한 핸들은 팝업이 닫힐 때 `WlanRegisterNotification`에 `nullptr`을 넘겨 해제하고
  `WlanCloseHandle`까지 하십시오. 핸들을 열어 둔 채 두지 마십시오.
- 실패하면 그 행 아래에 사유를 한 줄로 보여 주십시오. 비밀번호가 틀렸을 때가 가장 흔하고, 그때는
  입력란을 다시 열어 주는 편이 낫습니다.

### 3-5. 물러설 자리

인증 방식이 표에 없거나(기업용 802.1X, WPA3 SAE 등) 프로필 생성이 실패하면, 우리가 처리하지 말고
`ms-availablenetworks:`를 열어 Windows의 네트워크 목록에 넘기십시오. 그 경우에도 로그에 사유를
남기십시오.

### 3-6. 언제 입력란을 여는가

| 상황 | 하는 일 |
| --- | --- |
| 프로필이 있는 망 | 지금처럼 `wlan_connection_mode_profile`로 바로 연결 |
| 프로필이 없고 보안이 없는 망 | 지금처럼 `discovery_unsecure`로 바로 연결 |
| 프로필이 없고 보안이 있는 망 | 입력란을 열고, 받은 비밀번호로 프로필을 만든 뒤 연결 |
| 이미 연결된 망 | 아무 것도 하지 않음 (지금 동작 그대로) |

---

## 4. 검증

빌드는 Release 클린 빌드로 하고 경고가 없어야 합니다. **앱이 떠 있으면 링크가 막히므로
(`LNK1104`) 먼저 종료하십시오.**

1. **토글이 실제로 꺼지는지.** 토글을 누르고 로그에 `err=0`이 남는지, 그리고 Windows의 네트워크
   설정에서도 Wi-Fi가 꺼졌는지 함께 보십시오. 로그만 보고 판단하지 마십시오.
2. **다시 켜지는지.** 끈 뒤 다시 눌러 목록이 돌아오는지 보십시오.
3. **하드웨어 스위치.** `dot11HardwareRadioState`를 읽어 로그에 남기고, 이 기기에서 어떤 값인지
   보고하십시오. 끄는 스위치가 없는 기기라면 그 사실을 적으십시오.
4. **알려진 망 연결.** 프로필이 있는 망을 눌러 붙는지, 붙은 뒤 원의 색이 옮겨 가는지 보십시오.
5. **새 보안 망 연결.** 프로필이 없는 망을 눌러 입력란이 그 행 자리에 뜨는지, 비밀번호를 넣으면
   붙는지 보십시오. 붙일 수 있는 망이 없으면 일부러 틀린 비밀번호를 넣어 6번을 확인하십시오.
6. **틀린 비밀번호.** 사유가 화면에 뜨는지, 입력란이 다시 열리는지 보십시오.
7. **특수 문자.** `&`가 든 비밀번호로 한 번 시도해 프로필이 깨지지 않는지 보십시오.
   실제 망이 없으면 만들어진 XML을 로그가 아닌 임시 파일로 한 번 떨어뜨려 확인하고,
   확인한 뒤 그 파일을 지우고 코드도 지우십시오.
8. **입력란이 떠도 팝업이 닫히지 않는지.** 입력란에 포커스가 갈 때 패널이 사라지면 3-2절의 예외
   처리가 빠진 것입니다.
9. **Esc로 취소.** 입력란이 닫히고 패널은 열린 채 남아야 합니다.
10. **비밀번호가 로그에 없는지.** 검증을 마친 뒤 `~/.bamti/bamti.log`를 훑어 입력한 값이 어디에도
    남지 않았는지 확인하십시오.

레지스트리를 쓰는 검증은 넣지 마십시오. 되돌리지 못하고 사용자 설정을 날린 전례가 있습니다.

---

## 5. 보고할 것

- 토글을 켜고 끈 로그. `phys`와 `idx`와 `err` 값.
- 3번의 하드웨어 라디오 상태 값.
- 입력란이 뜬 화면과 연결에 성공한 화면.
- 틀린 비밀번호를 넣었을 때의 화면과 그때의 `WLAN_REASON_CODE` 문자열.
- 3-5절로 물러선 경우가 있었다면 그 인증 방식.
- 10번의 확인 결과.
