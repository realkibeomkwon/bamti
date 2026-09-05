# 수정 지시서: 흰 선으로만 그려지는 트레이 아이콘과 중복된 시스템 아이콘

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-BAR-INPUT.md` 다음에 하십시오.

건드리는 파일은 `src/icon_cache.cpp`, `src/tray_backend.hpp`, `src/tray_intercept.cpp`, `src/tray_mirror.cpp`입니다.

---

## 1. 결함 하나: Windows 업데이트 아이콘이 라이트 테마에서 보이지 않는다

### 1-1. 증상

`MoNotificationUx.exe`가 등록한 "업데이트를 설치하려면 장치를 다시 시작해야 합니다" 아이콘이 흰 선으로만 그려집니다. 상단바가 밝은 라이트 테마에서는 흰 선이 배경에 묻혀 거의 보이지 않습니다.

### 1-2. 이미 있는 장치

`src/icon_cache.cpp`에 밝은 단색 아이콘을 전경색으로 다시 칠하는 경로가 이미 있습니다. `IconCache::Decode`의 PNG 분기입니다.

```cpp
    if (IsBrightMonochrome(image)) {
      RecolorKeepAlpha(image, ClockTextColor(dark_));
    }
```

트레이 미러는 아이콘을 `IconKind::kPng`로 올리므로(`src/tray_mirror.cpp`의 `TrayMirror::Publish`) 이 경로를 지납니다. 그런데도 흰색으로 남아 있다는 것은 `IsBrightMonochrome`이 `false`를 돌려주고 있다는 뜻입니다.

```cpp
bool IsBrightMonochrome(const BgraImage& image) {
  ...
    if (a < 160) { continue; }
    ++ink;
    const int mx = ...; const int mn = ...;
    if (mx - mn > 28) { ++colorful; continue; }
    if (mn >= 160) { ++bright; }
  ...
  if (ink < 8 || colorful > 0) { return false; }
  return bright * 10 >= ink * 9;
}
```

떨어질 수 있는 자리가 둘입니다.

1. `colorful > 0`: **채도가 있는 화소가 하나라도 있으면 무조건 포기합니다.** Windows 업데이트 아이콘은 다시 시작이 필요할 때 주황색 표시를 함께 그립니다. 그 표시 한 점 때문에 나머지 흰 선 전체가 그대로 남을 수 있습니다.
2. `bright * 10 >= ink * 9`: 불투명 화소의 90% 이상이 밝아야 합니다. 선이 가는 아이콘은 반투명 경계 화소의 비율이 높아서 이 문턱을 못 넘길 수 있습니다.

### 1-3. 먼저 측정한다

**문턱을 고치기 전에 실제 수치를 보십시오.** 추측으로 상수를 흔들지 마십시오.

`IsBrightMonochrome`이 판정 결과와 함께 통계를 돌려주게 바꾸고, `Decode`에서 한 줄 남기십시오. 아이콘마다 한 번만 찍히도록 `cache_key`를 조건으로 걸면 로그가 넘치지 않습니다.

```cpp
Log(L"icon", L"mono key=%llu ink=%d bright=%d colorful=%d mean_lum=%.0f mean_sat=%.0f result=%d",
    ...);
```

`mean_lum`은 알파 가중 평균 밝기(`max(r,g,b)`), `mean_sat`은 알파 가중 평균 채도(`max-min`)로 계산하십시오. 앱을 라이트 테마에서 띄우고 Windows 업데이트 아이콘이 상단바에 올라온 뒤 로그를 읽어, **어느 조건에서 떨어졌는지 확정한 다음** 1-4로 가십시오.

### 1-4. 판정을 다시 세운다

전부 아니면 전무인 지금 방식을 버리고, 다음 두 가지로 바꾸십시오.

**(가) 채도가 있는 화소를 조금 허용한다.** 배지 한 점 때문에 전체를 포기하지 않습니다.

```
불투명(a >= 160) 화소 중
  gray  = 채도(max-min) <= 28 인 화소
  color = 나머지
gray가 8개 미만이면 false
gray의 알파 가중 평균 밝기가 200 미만이면 false
gray * 100 < ink * 70 이면 false   // 회색 화소가 7할 미만이면 단색 아이콘으로 보지 않는다
그 외에는 true
```

**(나) 다시 칠할 때 채도가 있는 화소는 건드리지 않는다.** `RecolorKeepAlpha`가 지금은 알파가 0이 아닌 화소를 전부 덮어씁니다. 회색 화소만 전경색으로 바꾸고, 배지처럼 색이 있는 화소는 원래 색을 남기십시오. 그래야 주황색 경고 표시가 살아남고 흰 선만 어두워집니다.

```cpp
void RecolorGrayKeepAlpha(BgraImage& image, D2D1_COLOR_F color) {
  // 채도(max-min)가 28 이하인 화소만 color로 바꾼다. 나머지는 그대로 둔다.
}
```

문턱값 28과 200과 70은 1-3에서 읽은 실제 수치를 보고 조정하십시오. 위 숫자는 출발점입니다. **조정했으면 그 근거가 된 측정값을 지시서 아래쪽이나 커밋 메시지에 적으십시오.**

### 1-5. 주의

- `IconCache::SetDark`가 테마가 바뀔 때 캐시를 비우고 있습니다. 그대로 두십시오. 다시 칠한 결과가 테마에 묶여 있으므로 이 초기화가 없으면 색이 굳습니다.
- 이 경로는 `IconKind::kPng`에만 걸려 있습니다. `kHicon`과 `kFile`은 지금 트레이 미러가 쓰지 않으므로 **이번에는 건드리지 마십시오.**
- 어두운 테마에서는 `ClockTextColor(true)`가 거의 흰색이므로 결과가 지금과 같아야 합니다. 어두운 테마에서 아이콘이 달라 보이면 잘못 만든 것입니다.

### 1-6. 검증

1. 라이트 테마에서 Windows 업데이트 아이콘이 어두운 선으로 또렷하게 보여야 합니다.
2. 같은 아이콘의 주황색 표시가 주황색으로 남아 있어야 합니다.
3. 다크 테마로 바꾸면 다시 흰 선으로 보여야 합니다. 테마를 오가며 두 번씩 확인하십시오.
4. 색이 풍부한 서드파티 아이콘(카카오톡, Teams, Tailscale, Everything)은 **원래 색 그대로** 남아야 합니다. 하나라도 단색으로 뭉개지면 (가)의 70% 문턱이 너무 낮은 것입니다.
5. 이미 잘 나오던 볼륨과 배터리 아이콘이 그대로인지 확인하십시오. 이 둘은 `IconKind::kVector`라 이 경로를 지나지 않지만, 회귀가 없는지는 봐야 합니다.

---

## 2. 결함 둘: 배터리와 볼륨 아이콘이 두 개씩 보인다

### 2-1. 확인 결과 — 없어도 되는 것이 맞습니다

로그로 확인했습니다. 우클릭해도 반응이 없는 쪽은 **explorer가 자기 자신을 위해 등록한 옛 방식의 알림 아이콘**입니다.

`~/.bamti/bamti.log`에서 뽑은 근거입니다.

```
[tray] intercept item tip="스피커: 87%" exe=explorer.exe hwnd=0x10266 uid=100 guid=1 version=0
[tray] intercept item tip=""          exe=explorer.exe hwnd=0x1022A uid=1225 guid=1 version=0
[tray] intercept invoke skipped key=0x74A4267B74A0D032 uid=100 tip="스피커: 87%" reason=no_callback
```

`reason=no_callback`이 핵심입니다. `src/tray_intercept.cpp`의 `Invoke`가 콜백 메시지가 0인 항목을 걸러 내고 있습니다.

```cpp
    if (owner == nullptr || callback == 0 || IsWindow(owner) == FALSE) {
      ... reason = L"no_callback";
```

이 아이콘들은 `NOTIFYICONDATA`에 콜백 메시지를 담지 않고 등록되어 있습니다. 즉 **누르든 우클릭하든 보낼 곳이 아예 없습니다.** 앞으로도 반응하게 만들 수 없습니다.

기능은 이미 bamti의 내장 위젯이 대신하고 있습니다. 볼륨 위젯은 슬라이더와 음소거를 주고, 배터리 위젯은 값에 따라 색이 바뀌는 아이콘과 패널을 줍니다. 제어 센터도 같은 값을 보여 줍니다. 그러므로 **눌러도 아무 일이 없는 쪽을 상단바에서 빼는 것이 맞습니다.**

### 2-2. 수정 — 가로채기 백엔드 경로

`src/tray_backend.hpp`의 `TrayIconInfo`에 소유자 실행 파일 이름을 더합니다. 이미 `callback_message`와 `owner`는 있습니다.

```cpp
  std::wstring owner_exe;  // 가로채기 백엔드만 채운다. UIA 백엔드는 비워 둔다.
```

`src/tray_intercept.cpp`는 등록을 받을 때 이미 `OwnerExeName(owner)`를 불러 로그에 찍고 있습니다. 그 값을 그대로 `info.owner_exe`에 넣으십시오. **열거할 때마다 다시 부르지 마십시오.** `OpenProcess`가 아이콘 수만큼 매 주기 돌면 상주 비용이 늘어납니다. 등록과 수정 시점에 한 번만 채웁니다.

`src/tray_mirror.cpp`의 `TrayMirror::Include`에 조건을 더합니다. 시계와 바탕 화면 단추를 빼는 자리 바로 아래가 좋습니다.

```cpp
  // explorer가 콜백 없이 등록한 옛 시스템 아이콘(볼륨, 전원)은 누를 수 없다.
  // 같은 기능을 내장 위젯과 제어 센터가 이미 담당한다.
  if (icon.callback_message == 0 && EqualsNoCase(icon.owner_exe, L"explorer.exe")) {
    return false;
  }
```

대소문자 무시 비교는 `_wcsicmp`를 쓰십시오.

**`callback_message == 0` 하나만으로 거르지 마십시오.** 작업 관리자가 등록하는 CPU 계기 아이콘도 콜백이 없지만(`key=0x554CBB105A9AF52E`) 그것은 살아 있는 표시라서 값이 있습니다. 소유자가 explorer일 때로 좁혀야 합니다.

콜백은 나중에 `NIM_MODIFY`로 붙을 수 있습니다. `Include`는 열거할 때마다 다시 평가되므로 그때는 자연히 다시 나타납니다. 별도 처리는 필요 없습니다.

### 2-3. 수정 — UIA 백엔드 경로

지금 설정은 `tray_backend: "intercept"`이지만 UIA로 되돌릴 수 있으므로 같이 막습니다. UIA 백엔드에서 같은 아이콘들의 클래스 이름은 `PROBE-TRAY.md`에 실측되어 있습니다.

```
볼륨    AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButtonCenter"
네트워크 AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"
배터리  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"
시계    AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButton"
입력기  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"
```

시계와 바탕 화면 단추를 이미 클래스 이름으로 빼고 있으니, 같은 자리에 두 클래스를 더하십시오.

```cpp
constexpr wchar_t kQuickSettingsClass[] = L"SystemTray.AccentButton";
constexpr wchar_t kQuickVolumeClass[] = L"SystemTray.OmniButtonCenter";
```

`SystemTray.AccentButton`은 네트워크와 배터리가 함께 쓰므로 둘 다 빠집니다. **의도한 결과입니다.** 네트워크도 내장 위젯이 있습니다. 입력기(`SystemTray.NormalButton`), 숨겨진 아이콘 표시, 알림, 개인 정보 아이콘은 그대로 남아야 합니다.

### 2-4. 설정에 남은 순서 정리

`~/.bamti/settings.json`의 `bar_order`에 사라질 아이콘의 아이디가 남아 있습니다(`bamti.tray/74a4267b74a0d032`가 볼륨입니다). 순서 목록에 있으나 실제로 없는 항목은 무시되어야 합니다. `MenuBar`의 순서 적용 코드가 이미 그렇게 동작하는지 확인하고, 아니면 그렇게 고치십시오. **설정 파일을 코드가 임의로 지우게 만들지는 마십시오.**

### 2-5. 검증

1. 상단바에 배터리 아이콘이 하나만, 볼륨 아이콘도 하나만 보여야 합니다.
2. 남은 배터리와 볼륨 아이콘은 우클릭과 좌클릭에 모두 반응해야 합니다.
3. 한/영 입력기 아이콘은 그대로 남아 있어야 합니다.
4. 로그에 `invoke skipped ... reason=no_callback`이 더 이상 찍히지 않아야 합니다. 눌릴 아이콘 자체가 사라졌기 때문입니다.
5. 설정에서 트레이 미러를 껐다 켜도 중복이 돌아오지 않아야 합니다.
6. 작업 관리자를 트레이로 내렸을 때 CPU 계기 아이콘은 계속 보여야 합니다.

---

## 3. 보고할 것

- 1-3에서 읽은 `mono ...` 로그 원문과, 그 값을 보고 문턱을 어떻게 정했는지.
- 라이트/다크 두 테마에서 업데이트 아이콘과 서드파티 아이콘을 찍은 화면.
- 중복 제거 뒤 상단바에 남은 항목 목록.
