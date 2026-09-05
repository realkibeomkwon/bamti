# 작업 지시서: 오버플로 단추 판정과 숨겨진 아이콘 측정

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-TRAY-PEEK.md`를 적용한 뒤 다시 검수하면서 탐침을 한 번 더 돌렸고, 그 결과로 두 가지가 확정되었습니다. 하나는 아직 남아 있는 결함이고, 다른 하나는 5단계의 전제를 뒤집을 수 있는 관측입니다.

측정에 쓴 것은 `bamti.exe --probe-tray`이며 보고서는 `%USERPROFILE%\.bamti\probe-tray.txt`(2026-08-29 11:35)입니다.

`FIX-TRAY-PEEK.md`의 1번, 2번, 4번은 정상적으로 고쳐진 것을 확인했습니다. `Rehide()`와 `EnsureHidden()`이 `Peeking()`을 존중하고, 시한 자동 만료도 들어 있습니다.

---

## 1. 오버플로 단추 판정이 아직 정확하지 않다

### 무엇이 확정되었는가

`0b02ad6`과 `1733329`로 자식 `Image` 조건을 넣었지만, 이 조건만으로는 오버플로 단추를 가리지 못합니다. 이번 탐침에서 시스템 아이콘 일곱 개의 자식 구조를 직접 확인했습니다.

| 순서 | 이름 | ClassName | 자식 Image |
|---|---|---|---|
| 0 | 숨겨진 아이콘 표시 | `SystemTray.NormalButton` | **없음** |
| 1 | 트레이 입력 표시기 한/영 전환 | `SystemTray.NormalButton` | 있음 |
| 2 | 네트워크 | `SystemTray.AccentButton` | **없음** |
| 3 | 볼륨 | `SystemTray.OmniButtonCenter` | **없음** |
| 4 | 배터리 | `SystemTray.AccentButton` | **없음** |
| 5 | 시계 | `SystemTray.OmniButton` | **없음** |
| 6 | 바탕 화면 보기 | `SystemTray.ShowDesktopButton` | **없음** |

즉 자식 `Image`를 가진 시스템 아이콘은 입력 표시기 하나뿐입니다. 현재 `OverflowOrder`는 "자식 `Image`가 없는 첫 `SystemTrayIcon`"을 오버플로 단추로 보므로, **오버플로 단추가 사라진 환경에서는 네트워크 아이콘을 오버플로로 오인해 미러에서 지웁니다.** 사용자가 Windows 설정에서 모든 아이콘을 항상 표시하도록 바꾸면 바로 이 상태가 됩니다. 그 설정은 우리가 문서에서 권하고 있는 설정이기도 합니다.

지금 이 컴퓨터는 오버플로 단추가 존재하고 그것이 목록의 첫 항목이므로 우연히 맞게 동작합니다. 로그의 `items=4`는 일곱 개에서 오버플로 단추와 시계와 바탕 화면 보기를 뺀 값으로, 기대한 대로입니다.

### 고치는 방법

표에서 보듯 `ClassName`이 갈라 줍니다. `SystemTray.NormalButton`인 것은 오버플로 단추와 입력 표시기뿐이고, 이 둘은 자식 `Image` 유무로 정확히 갈립니다. 따라서 조건을 하나 더합니다.

```cpp
// 오버플로 단추는 가장 왼쪽 SystemTrayIcon 중
// ClassName이 NormalButton이면서 자식 Image가 없는 버튼이다.
// 다른 시스템 아이콘은 AccentButton, OmniButton, OmniButtonCenter,
// ShowDesktopButton이므로 이 조건에 걸리지 않는다.
if (icon.system_icon && icon.class_name == kOverflowButtonClass && !icon.has_image_child) {
  return icon.order;
}
```

`kOverflowButtonClass`는 `L"SystemTray.NormalButton"`으로 파일 상단에 둡니다.

`has_image_child`를 얻지 못했을 때의 기본값이 참인 것은 그대로 두십시오. 못 얻으면 아무것도 오버플로로 보지 않게 되고, 그것이 안전한 방향입니다.

### 검증

- [ ] 지금 상태에서 미러 항목 수가 그대로 4개입니다.
- [ ] Windows 설정에서 "모든 아이콘을 항상 표시"를 켜 오버플로 단추가 사라진 뒤에도 네트워크와 볼륨과 배터리와 입력 표시기가 전부 미러에 남아 있습니다.
- [ ] 그 상태에서 다시 오버플로 단추가 생기면 그것만 제외됩니다.

---

## 2. 오버플로 안의 아이콘이 UIA 트리에 나타난다

### 무엇이 관측되었는가

`TASK-TRAY-MIRROR.md` 1절 6번은 "오버플로 창은 닫혀 있는 동안 UIA 자식이 비어 있으므로 숨겨진 아이콘은 열거할 수 없다"를 전제로 삼았습니다. 이 전제는 `PROBE-TRAY.md`(2026-08-28)의 관측에 근거했습니다.

이번 탐침에서는 다르게 나왔습니다. 오버플로 창을 열지 않았는데도 서드파티 아이콘 여섯 개가 이름과 함께 열거되었습니다.

```
Button  name=""                                        AutomationId="NotifyItemIcon"  rect(0,0,0,0)  IsOffscreen=yes
Button  name="Everything"                              AutomationId="NotifyItemIcon"  rect(0,0,0,0)  IsOffscreen=yes
Button  name="KakaoTalk"                               AutomationId="NotifyItemIcon"  rect(0,0,0,0)  IsOffscreen=yes
Button  name="오피스키퍼"                               AutomationId="NotifyItemIcon"  rect(0,0,0,0)  IsOffscreen=yes
Button  name="Tailscale: Connected. Click for options." AutomationId="NotifyItemIcon" rect(0,0,0,0)  IsOffscreen=yes
Button  name="Bluetooth 장치"                          AutomationId="NotifyItemIcon"  rect(0,0,0,0)  IsOffscreen=yes
```

이름은 정확하고 각 버튼에 자식 `Image`도 있습니다. 좌표는 전부 `(0,0,0,0)`이고 `IsOffscreen`이 참입니다.

이것이 중요한 이유는, 이 컴퓨터에서 **화면에 보이는 서드파티 아이콘이 하나도 없기 때문**입니다. 서드파티 아이콘 여섯 개가 전부 오버플로에 들어가 있고, 그래서 지금 미러가 보여 주는 것은 시스템 아이콘 네 개뿐입니다. 트레이 미러의 본래 목적인 "서드파티 트레이 앱을 상단바에서 쓰기"가 이 환경에서는 충족되지 않습니다.

미러가 이 여섯 개를 발행하지 않는 이유는 열거 루트가 `Shell_TrayWnd`의 자식인 `DesktopWindowContentBridge` 하나이고, 오버플로 아이콘은 별도 최상위 창인 `TopLevelWindowForOverflowXamlIsland` 아래에 있기 때문입니다.

### 이번에 할 일은 측정뿐이다

오버플로 아이콘을 미러할지 말지는 설계 판단이므로 이 지시서에서 결정하지 않습니다. 판단에 필요한 사실만 측정해 주십시오. **구현은 하지 마십시오.**

`--probe-tray`에 다음을 더합니다. 기존 출력은 그대로 두고 절을 하나 추가하십시오.

1. `TopLevelWindowForOverflowXamlIsland`의 자식 `DesktopWindowContentBridge`를 루트로 삼아 `AutomationId`가 `NotifyItemIcon`인 버튼을 열거하고, 이름과 좌표와 `IsOffscreen`과 `RuntimeId`를 적습니다.
2. 그 열거에 걸린 시간을 밀리초로 적습니다. 미러가 루트를 하나 더 보게 되면 늘어날 비용입니다.
3. 각 버튼의 `Invoke` 패턴 사용 가능 여부를 적습니다. **호출하지는 마십시오.** 조회만 합니다.
4. 같은 측정을 3초 간격으로 다섯 번 반복해 `RuntimeId`가 회차마다 유지되는지 적습니다. 좌표가 전부 `(0,0,0,0)`이라 순서를 좌표로 정할 수 없으므로, 무엇으로 순서를 정할 수 있는지가 판단의 관건입니다. 열거 순서 자체가 회차마다 같은지도 함께 적으십시오.

결과는 `PROBE-TRAY-OVERFLOW.md`로 저장소 루트에 남기고, 마지막에 다음 세 줄에 대한 답을 적으십시오.

- 오버플로 아이콘을 안정적인 식별자로 추적할 수 있는가.
- 열거 루트를 하나 더 늘렸을 때 회차 비용이 얼마나 늘어나는가.
- `Invoke` 패턴이 사용 가능하다고 보고되는가.

여기까지가 이번 범위입니다. 미러 구현은 이 측정 결과를 보고 다음 지시서에서 정합니다.

---

## 3. 하지 말아야 할 것

- 오버플로 아이콘을 미러하는 코드를 이번에 넣지 마십시오. 측정만 합니다.
- 탐침에서 `Invoke`를 호출하지 마십시오. 시스템 상태를 바꾸지 않는다는 탐침의 원칙을 지킵니다.
- 오버플로 창을 열지 마십시오. 닫힌 상태에서 무엇이 보이는지가 측정의 목적입니다.
- 기존 `--probe-tray` 출력 형식을 바꾸지 마십시오. 절을 더하기만 합니다.

---

## 4. 커밋

| 순서 | 작업 | 커밋 메시지 |
|---|---|---|
| 1 | 오버플로 단추 판정에 ClassName 조건 추가 | `fix: 오버플로 단추를 NormalButton으로 좁혀 가른다` |
| 2 | 탐침에 오버플로 열거 절 추가 | `feat: 오버플로 아이콘 열거를 탐침으로 측정한다` |
| 3 | 측정 결과 기록 | `docs: 오버플로 아이콘 열거 측정 결과를 남긴다` |
