# 작업 지시서: 앱을 종료해도 상단바 트레이에 낡은 보충 항목이 되살아나는 문제를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/tray_mirror.cpp` 와 `src/tray_mirror.hpp` 두 개입니다.

---

## 1. 증상

카카오톡을 **완전히 종료**하면 상단 메뉴바 트레이에 `K` 한 글자짜리 항목이 새로 생깁니다. 카카오톡이 살아 있는 동안에는 없다가, 종료한 뒤에 나타납니다.

같은 방식으로 UIA 보충을 거친 모든 앱에서 똑같이 일어납니다. 실제로 로그에는 Tailscale 도 보충 대상으로 올라와 있습니다.

---

## 2. 무엇이 원인인가 (코드로 확정)

### 2-1. 보충 목록이 시작 15초 뒤에 얼어붙는다

`TrayMirror::RefreshUiaFill` 은 `DoRound` 의 인자 `refresh_fill` 이 참일 때만 호출됩니다. `WorkerLoop` 에서 `refresh_fill` 이 참이 되는 조건은 두 가지뿐입니다.

```cpp
if (!fill_started) {
  refresh_fill = true;
  fill_started = true;
}
if (backend->TakeStartupFillPulse() > 0) {
  refresh_fill = true;
}
```

첫째 조건은 프로세스를 통틀어 **최초 한 번**만 참입니다. 둘째 조건은 가로채기 백엔드가 시작 직후 0초, 2초, 6초, 15초에 올리는 재브로드캐스트 펄스입니다.

즉 **bamti 를 시작하고 15초가 지나면 `fill_icons_` 는 두 번 다시 열거되지 않고 그대로 고정됩니다.**

### 2-2. 얼어붙은 목록이 앱이 죽는 순간 되살아난다

`MergeUiaFill` 은 매 라운드 호출되며, 고정된 `fill_icons_` 에서 `InterceptOwns` 가 거짓인 항목만 골라 병합합니다.

- 카카오톡이 **살아 있는 동안**에는 가로채기가 같은 툴팁을 들고 있으므로 `SameFillItem` 이 참이 되고, `InterceptOwns` 도 참이라 병합되지 않습니다. 그래서 화면에 보이지 않습니다.
- 카카오톡을 **종료하면** 가로채기 항목이 사라지므로 `InterceptOwns` 가 거짓으로 바뀝니다. 그 순간 15초 시점에 찍어 둔 낡은 항목이 병합되어 화면에 나타납니다.

증상이 "종료했을 때 생긴다"인 이유가 이것입니다. 새로 생기는 것이 아니라, 가려져 있던 낡은 항목이 드러나는 것입니다.

### 2-3. `K` 한 글자로 보이는 이유

`TrayMirror::Publish` 는 아이콘 비트맵이 없으면 툴팁의 첫 글자로 대체합니다.

```cpp
if (!icon.png.empty()) {
  item.icon.kind = IconKind::kPng;
  item.icon.bytes = icon.png;
} else {
  item.icon.kind = IconKind::kGlyph;
  item.icon.glyph = FirstGlyph(icon.tip);
```

UIA 백엔드는 아이콘 비트맵을 채우지 않으므로 `"KakaoTalk"` 의 첫 글자만 남습니다. **이 대체 자체는 이번 지시서에서 고치지 않습니다.** 2-1 을 고치면 낡은 항목이 사라지므로 증상도 함께 없어집니다.

---

## 3. 무엇을 고치는가

`fill_icons_` 를 주기적으로 다시 열거하십시오.

### 3-1. 주기 상수와 상태를 더한다

`src/tray_mirror.cpp` 의 상수 자리(`kDiagEnumMinMs` 근처)에 더합니다.

```cpp
constexpr ULONGLONG kFillRefreshMs = 5000;
```

`WorkerLoop` 안의 지역 변수로 마지막 갱신 시각을 둡니다. `fill_started` 와 같은 자리에 두면 됩니다.

```cpp
ULONGLONG last_fill_refresh = 0;
```

### 3-2. 갱신 조건을 넓힌다

```cpp
if (intercept && diag_uia != nullptr) {
  const ULONGLONG now = GetTickCount64();
  if (!fill_started) {
    refresh_fill = true;
    fill_started = true;
  }
  if (backend->TakeStartupFillPulse() > 0) {
    refresh_fill = true;
  }
  if (last_fill_refresh == 0 || now - last_fill_refresh >= kFillRefreshMs) {
    refresh_fill = true;
  }
  if (refresh_fill) {
    last_fill_refresh = now;
  }
}
```

`OnExplorerRestart` 등에서 `fill_started` 를 거짓으로 되돌리는 자리가 이미 있습니다. 같은 자리에서 `last_fill_refresh` 도 0 으로 되돌리십시오.

### 3-3. 사라진 항목을 로그로 남긴다

지금은 `fill_logged_` 때문에 새로 들어온 항목만 한 번 기록되고, 빠지는 항목은 아무 흔적도 남지 않습니다. 검증할 수 없으므로 제거 로그를 더하십시오. `MergeUiaFill` 에서 이전 `fill_keys_` 와 새 `keys` 를 비교해, 빠진 키에 대해 한 줄씩 남깁니다.

```
[tray] fill drop key=0x... tip="..."
```

빠진 키는 `fill_logged_` 에서도 지워서, 그 앱이 다시 뜨면 등장 로그가 다시 남게 하십시오.

### 3-4. 열거 비용을 잰다

UIA 열거는 느립니다. `RefreshUiaFill` 이 `Enumerate` 에 쓴 시간을 재서, `kSlowEnumMs`(200ms)를 넘긴 경우에만 한 줄 남기십시오.

```
[tray] fill enum slow ms=%llu count=%zu
```

**5초 주기로 이 줄이 계속 쌓이면 주기가 너무 짧다는 뜻입니다.** 그때는 여기서 멈추고 보고하십시오. 스스로 주기를 늘리지 마십시오.

---

## 4. 검증

측정값으로 판정하십시오. 반환값이나 "그럴 것이다"로 판정하지 마십시오.

1. bamti 를 CMake 로 빌드해 실행하고 **60초 이상** 기다립니다. 시작 직후 15초 창을 확실히 벗어나야 합니다.
2. 카카오톡을 트레이 메뉴에서 완전히 종료합니다.
3. 로그에 5초 안에 다음 줄이 나와야 합니다.
   ```
   [tray] fill drop key=0x... tip="KakaoTalk"
   ```
4. 상단 메뉴바에 `K` 항목이 남아 있는지는 **사용자에게 확인을 부탁하십시오.** 사용자 화면에 입력을 합성하지 마십시오.
5. 카카오톡을 다시 실행하고, 가로채기가 잡지 못하는 회차에서 보충이 다시 들어오는지 확인합니다. `[tray] fill key=...` 줄이 다시 나와야 합니다.
6. `[tray] fill enum slow` 줄이 쌓이지 않는지 확인합니다.

---

## 5. 건드리지 말 것

- 가로채기 백엔드의 재브로드캐스트 일정(0초, 2초, 6초, 15초)과 `TakeStartupFillPulse`
- `kSlowEnumMs` 와 `kSlowStreakStop` 의 자동 정지 로직
- `Publish` 의 글리프 대체 경로
- `InterceptOwns` 와 `SameFillItem` 의 대조 규칙
- `DrainInvoke` 의 fill 라우팅
