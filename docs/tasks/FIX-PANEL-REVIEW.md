# FIX-PANEL-REVIEW — 패널 통일 뒤 화면 확인에서 드러난 세 가지를 고친다

이 문서는 2026-09-05 시점의 작업 지시서이며, 현재 코드의 설명이 아니라 당시의 기록이다.

`TASK-SYSTEM-PANELS.md`의 구현을 실기에서 확인하다가 나온 문제다. 셋은 서로 기대지 않으므로 순서를 바꾸어도 된다.

## 먼저 확인된 사실

화면 확인에서 아래는 **정상으로 판명되었다.** 건드리지 마라.

- `IPolicyConfig`의 vtable 13번 슬롯이 맞다. 로그에 `IPolicyConfig QI hr=0x00000000 vtable_slot=13`이 남았고 출력 장치가 실제로 바뀌었다.
- 절전 모드의 되돌리기가 제대로 돈다. 로그에 `saver threshold 30 -> 100`과 `saver threshold 100 -> 30`이 차례로 남았다.
- 블루투스 배터리 키는 이 기기에서 값을 주지 않는다(`bt battery key produced no values; hiding percents`). 지시대로 백분율만 숨겼으므로 그대로 둔다.
- 볼륨 슬라이더 트랙(`kVolumeTrackHDip` 6)과 게이지 바(`kGaugeHDip` 10)의 두께 차이는 의도한 것이다. 슬라이더는 지름 18 DIP 노브가 트랙 위에 얹히므로 얇아야 노브와 대비가 살고, 게이지는 노브가 없어 그 자체로 읽혀야 하므로 두껍다. **통일하지 마라.**

---

## 1. 출력 장치를 바꾼 뒤 슬라이더가 흐리게 남는다

### 증상

볼륨 패널에서 다른 출력 장치를 고르면 슬라이더의 채움과 노브가 반투명해지고, 상단바 볼륨 항목이 "음소거"로 바뀐다. 실제로 음소거된 것은 아니다.

### 원인

장치를 바꾸는 자리(`control_center.cpp`의 `Page::kVolume` 목록 클릭)가 `audio_outs_`만 다시 열거하고 `PresentHost()`를 부른다.

```cpp
if (!dev.is_default && SetDefaultAudioOutput(dev.id)) {
  audio_outs_ = EnumAudioOutputs();
  ...
  PresentHost();
}
```

`BuiltinWidgets`의 `VolumeControl`은 이전 장치의 `IAudioEndpointVolume`을 그대로 쥐고 있다. 이것을 놓는 곳은 `SampleVolume`의 `volume_refresh_due_`뿐이고 주기가 `kVolumeRefreshMs`(20초)다. 그래서 최대 20초 동안 **이전 장치의 레벨과 음소거 값**을 읽는다. 화면의 반투명은 그렇게 남은 `muted_`가 참이라 생긴 것이며, `panel::DrawToggle`이나 슬라이더 그리기 코드의 잘못이 아니다.

### 고칠 방법

장치 전환이 성공하면 위젯 워커에게 **엔드포인트를 즉시 다시 잡으라고 알린다.**

`StatusEvent`로 `bamti.widget/volume`에 새 `row_id`(예: `volume_device`)를 보내고, `BuiltinWidgets::OnEvent`가 그것을 받으면 워커를 깨워 아래를 하게 한다.

1. `volume_->Release()`로 이전 엔드포인트를 놓는다.
2. `volume_refresh_due_`를 지금으로 당겨 다음 주기를 기다리지 않게 한다.
3. `volume_due_ = 0`으로 곧바로 `SampleVolume`을 돌린다.

`ControlCenterContent` 쪽은 `ApplyLive`가 새 값을 받으면 저절로 따라오므로 따로 손댈 것이 없다. 다만 전환 직후 한 번은 `PresentHost()`를 유지해 목록의 강조 표시가 바로 옮겨 가게 한다.

`SetDefaultAudioOutput`이 세 역할(`eConsole`, `eMultimedia`, `eCommunications`)을 모두 바꾸고 나서 Windows가 기본 장치를 실제로 갈아 끼우기까지 짧은 틈이 있다. 다시 잡기가 이른 탓에 여전히 옛 장치를 물면 곤란하므로, 워커에서 `Release()` 뒤 한 박자(200 ms 남짓) 쉬고 읽는다. **이 대기는 UI 스레드가 아니라 위젯 워커에서 한다.**

### 확인 방법

출력 장치를 바꾼 직후 슬라이더가 또렷하게 남고 상단바 표시가 백분율을 유지하면 된다. 로그에 `volume endpoint acquire took ... ms`가 전환 직후 한 번 더 남는 것으로도 확인할 수 있다.

---

## 2. 절전 모드 토글이 한 번 실패하면 영구히 사라진다

### 증상

전원 케이블을 뽑고 절전 모드 토글을 눌렀더니, 토글이 사라지고 "꺼짐" 텍스트로 바뀌었다. 다시 켤 방법이 없다.

### 원인

로그가 경위를 그대로 보여 준다.

```
15:06:07.251 [power] saver threshold 30 -> 100
15:06:12.885 [power] saver threshold 100 -> 30
15:06:13.896 [power] saver toggle abandoned (SystemStatusFlag did not follow) err=0
```

임계값을 100으로 올린 뒤 `Sleep(kFlagWaitMs)`, 곧 400 ms만 기다리고 `SystemStatusFlag`를 확인한다. 따라오지 않자 되돌린 다음 `MarkFailed`가 `g_toggle_ok`를 거짓으로 바꾼다. `BatterySaverToggleAvailable()`이 거짓이 되면 `RenderBatteryPage`가 토글 대신 텍스트를 그리므로, 사용자에게는 토글이 사라진 것으로 보인다.

두 가지가 겹쳤다.

- **기다림이 짧다.** 위 로그에서 100으로 쓴 시각과 되돌린 시각의 차이가 5.6초다. `PowerSetActiveScheme`이 계획을 다시 적용하는 데만도 그만큼 걸렸다는 뜻이며, Windows가 절전 모드를 실제로 켜기까지는 더 걸린다. 400 ms는 이 판정에 쓸 수 없는 길이다.
- **한 번 실패하면 되돌릴 길이 없다.** `g_toggle_ok`는 다시 참이 되지 않는다. 앱을 다시 켜야만 원상태가 된다.

덧붙여 이 기기는 배터리가 100%였다. 임계값 100은 "잔량 100% 이하에서 켜라"는 뜻이지만, Windows는 완전 충전에 가까운 상태에서 절전 모드를 켜지 않는 편이다. 곧 **이 방식 자체가 항상 듣는다는 보장이 없다.**

### 고칠 방법

**첫째, 판정을 늦추고 여러 번 본다.** `Sleep` 한 번으로 끝내지 말고 200 ms 간격으로 최대 3초까지 `SaverFlagOn()`을 다시 본다. 도중에 따라오면 곧바로 성공으로 본다. 워커 스레드이므로 3초를 기다려도 UI가 멈추지 않는다.

**둘째, 실패해도 토글을 없애지 않는다.** 이것이 이 항목의 핵심이다. `g_toggle_ok`를 한 번의 실패로 끄지 마라. 대신 아래처럼 나눈다.

- `SetBatterySaver`가 실패하면 그 회차만 거짓을 돌려주고, 임계값은 원래대로 되돌린다(이 부분은 지금도 맞게 되어 있다).
- 연달아 **세 번** 실패했을 때에만 `g_toggle_ok`를 끈다. 성공하면 실패 횟수를 0으로 되돌린다.
- `g_toggle_ok`가 꺼진 뒤에도 배터리 페이지의 그 줄은 **토글 모양을 유지하되 흐리게** 그리고, 누르면 `ms-settings:batterysaver`를 연다. 줄이 통째로 텍스트로 바뀌면 사용자는 기능이 사라졌다고 읽는다.

**셋째, 왜 실패했는지 남긴다.** 판정에 실패한 자리에서 그때의 배터리 잔량과 `ACLineStatus`를 함께 로그에 적는다.

```
Log(L"power", L"saver flag did not follow within %lu ms (pct=%d ac=%d threshold=%lu)", ...);
```

잔량이 임계값보다 높아 Windows가 켜지 않는 상황과, API가 듣지 않는 상황을 나중에 갈라 볼 수 있어야 한다.

### 하지 말 것

- **절전 모드를 반복해서 켜고 끄며 시험하지 마라.** 전원 계획 값을 건드리는 일이다. 고친 뒤 한 번 켜고 한 번 끄는 것으로 끝낸다.
- 되돌리기 경로는 이미 맞게 되어 있다. `WriteThreshold(revert)`가 성공했을 때에만 백업을 지우는 지금 모양을 **바꾸지 마라.**
- 레지스트리에 직접 쓰지 마라.

### 확인 방법

토글을 눌러 실패하더라도 토글 모양이 남아 있고 다시 누를 수 있으면 된다. 설정 파일의 `saver_threshold_backup`이 `-1`로 돌아왔는지, 임계값이 원래 값(이 기기는 30)인지 `PowerReadDCValueIndex`로 확인한다.

---

## 3. Windows 블루투스 트레이 아이콘을 감춘다

### 증상

자체 블루투스 위젯과 Windows가 띄우는 트레이 아이콘이 나란히 보여 아이콘이 둘이다.

### 대상 식별

로그에 이렇게 남아 있다.

```
[tray] intercept item tip="Bluetooth 장치" exe=explorer.exe hwnd=0x10292 uid=0 guid=1 version=0
[tray] uia item tip="Bluetooth 장치" system=0 overflow=0
```

`explorer.exe`가 소유하고 GUID로 등록된 아이콘이다. `uid`가 0이고 `guid`가 1이므로 **등록 GUID가 이 아이콘의 안정된 열쇠**다.

### 고칠 방법

**먼저 GUID 값을 알아내라.** 지금 로그는 GUID를 썼는지 여부만 `guid=1`로 남기고 값을 남기지 않는다. `tray_intercept.cpp`(또는 아이콘을 받아들이는 자리)에서 등록 GUID를 한 번 로그에 찍어 값을 확인한 뒤, 그 값을 상수로 두고 거른다.

거르는 조건은 이렇다.

- `settings_.bluetooth`가 참일 때에만 감춘다. 위젯을 끄면 Windows 아이콘이 다시 보여야 한다.
- 판정은 등록 GUID로 한다. `TrayMirror`의 `KeyHidden`과 같은 자리에서 걸러도 되고, 아이콘을 받아들이는 단계에서 걸러도 된다. **`tray_hidden_keys`에 값을 밀어 넣지는 마라.** 그 목록은 사용자가 직접 숨긴 것을 담는 곳이고, 코드가 끼어들면 사용자가 나중에 되돌릴 때 헷갈린다.
- 두 경로 모두에서 걸러야 한다. 위 로그를 보면 `intercept` 백엔드와 `uia` 백엔드가 같은 아이콘을 각각 잡는다. 한쪽만 막으면 백엔드를 바꿨을 때 다시 나타난다.

**GUID를 얻지 못하면** 툴팁이 "Bluetooth 장치"이면서 소유 프로세스가 `explorer.exe`인 것을 거르는 방식으로 물러서라. 다만 이 방식은 표시 언어가 바뀌면 듣지 않으므로, 물러섰다는 사실과 확인한 GUID 값을 반드시 보고하라.

감춘 사실은 로그에 한 번만 남긴다.

```
Log(L"tray", L"hiding system bluetooth icon (widget on) guid={...}");
```

### 확인 방법

블루투스 위젯을 켠 상태에서 상단바에 블루투스 아이콘이 하나만 보이고, 위젯을 끄면 Windows 아이콘이 다시 보이면 된다. 다른 트레이 아이콘이 함께 사라지지 않았는지도 확인하라.

---

## 마무리

세 가지를 고친 뒤 `cmake --build out/cmake-debug --config Debug`가 경고 없이 통과하는지 확인하고, 무엇을 확인했고 무엇을 확인하지 못했는지 나누어 보고하라. 화면 확인이 필요한 것은 사용자에게 남겨 두면 된다.
