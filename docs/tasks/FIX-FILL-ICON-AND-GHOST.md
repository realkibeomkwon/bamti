# 작업 지시서: 종료한 앱의 글자 항목이 상단바에 나타나지 않게 하고, 보충 항목에 진짜 아이콘을 붙인다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/tray_mirror.cpp`, `src/tray_mirror.hpp`, `src/tray_intercept.cpp`, `src/tray_intercept.hpp`, `src/task_list.cpp`, `src/task_list.hpp` 입니다.

---

## 1. 증상과 요구 사항

카카오톡을 완전히 종료하면 상단 메뉴바에 `K` 한 글자짜리 항목이 나타납니다.

`FIX-STALE-TRAY-FILL.md` 로 5초 주기 갱신을 넣었지만 **부족합니다.** 실측하면 이렇습니다.

```
19:22:10.873 [tray] fill key=0xA3306F84ABCBD01 exe=- tip="KakaoTalk"   ← K 가 나타난다
19:22:19.608 [tray] fill drop key=0xA3306F84ABCBD01 tip="KakaoTalk"    ← 8.7초 뒤에 사라진다
```

**사용자의 요구는 "빨리 사라지는 것"이 아니라 "한 번도 보이지 않는 것"입니다.** 지연을 줄이는 방향으로 접근하지 마십시오.

---

## 2. 무엇이 원인인가 (로그로 확정)

### 2-1. 보충 목록은 언제나 과거의 스냅숏이다

`fill_icons_` 는 마지막 UIA 열거 시점의 사진입니다. `MergeUiaFill` 은 매 라운드 이 사진을 꺼내, 가로채기가 들고 있지 않은 항목을 병합합니다.

카카오톡을 종료하면 이런 순서가 됩니다.

1. 가로채기 항목이 사라진다.
2. 그 순간 `InterceptOwns` 가 거짓이 되어, **직전 사진에 찍혀 있던 카카오톡이 드러난다.** ← 여기서 `K` 가 나타납니다.
3. 5초 뒤 UIA 를 다시 열거하면 explorer 도 아이콘을 지운 상태이므로 빠진다.

**주기를 아무리 줄여도 1번과 3번 사이의 창은 남습니다.** 구조를 바꿔야 합니다.

### 2-2. 글자로 보이는 이유

`TrayMirror::Publish` 는 아이콘 비트맵이 없으면 툴팁의 첫 글자로 대체합니다. UIA 백엔드는 비트맵을 채우지 않으므로 보충 항목은 **살아 있을 때도 언제나 글자 하나**입니다. 로그의 `exe=-` 가 그 증거입니다.

즉 보충 기능은 지금 "정체를 알 수 없는 글자"만 보여 주고 있습니다.

---

## 3. 무엇을 고치는가

핵심은 하나입니다. **보충 항목의 주인이 누구인지 알아내고, 그 주인이 살아 있을 때만 게시합니다.** 주인을 알면 아이콘도 따라옵니다.

### 3-1. 가로채기가 본 것을 사전으로 남긴다

`TrayMirror` 에 툴팁을 열쇠로 하는 사전을 둡니다.

```cpp
struct FillOwner {
  std::wstring exe_path;  // 전체 경로
  HWND owner = nullptr;
  DWORD pid = 0;
};
std::unordered_map<std::wstring, FillOwner> fill_owners_;
```

`DoRound` 에서 가로채기 목록 `raw` 를 훑을 때 채웁니다.

- `icon.owner` 가 유효하고 `icon.tip` 이 비어 있지 않은 항목만 기록합니다.
- 전체 경로는 `GetWindowThreadProcessId(icon.owner, &pid)` 로 PID 를 얻고 `QueryFullProcessImageNameW` 로 구합니다. `TrayIconInfo::owner_exe` 는 파일 이름뿐이라 아이콘 추출에 쓸 수 없습니다.
- **이 사전은 지우지 마십시오.** 프로세스 수명 동안 누적합니다. 가로채기가 한 번이라도 그 앱을 본 적이 있으면 계속 쓸 수 있어야 합니다.
- 같은 툴팁이 다시 들어오면 덮어씁니다.
- 비용이 걱정되면 이미 사전에 있고 `pid` 가 같은 툴팁은 건너뛰십시오.

### 3-2. 주인이 죽었으면 게시하지 않는다

`MergeUiaFill` 에서 보충 후보를 병합하기 전에 걸러냅니다.

```cpp
// 주인을 모르는 항목은 게시하지 않는다. 정체를 알 수 없는 글자 하나가 되기 때문이다.
const auto owner = fill_owners_.find(icon.tip);
if (owner == fill_owners_.end()) {
  continue;
}
// 주인 창이 사라졌으면 앱이 종료된 것이다. explorer 가 아이콘을 아직 지우지 않았을 뿐이다.
if (IsWindow(owner->second.owner) == FALSE) {
  continue;
}
DWORD now_pid = 0;
GetWindowThreadProcessId(owner->second.owner, &now_pid);
if (now_pid != owner->second.pid) {
  continue;  // 핸들이 재사용되었다.
}
```

`IsWindow` 는 즉시 반영되고 값이 쌉니다. **카카오톡을 종료하는 순간 거짓이 되므로 `K` 는 한 번도 나타나지 않습니다.**

### 3-3. 아이콘을 채운다

걸러내고 남은 보충 항목에 `icon.png` 를 채웁니다.

- `src/tray_intercept.cpp` 의 `IconToPng` 를 헤더로 노출해 재사용하십시오. 새로 구현하지 마십시오.
- 아이콘은 `PrivateExtractIconsW(exe_path, 0, ...)` 로 뽑습니다. `src/dock.cpp` 1906행이 같은 일을 하고 있으니 호출 방식을 참고하십시오.
- **exe 경로를 열쇠로 하는 캐시를 두십시오.** 매 라운드 추출하면 비쌉니다. 한 번 만든 png 는 계속 씁니다.
- 추출에 실패하면 그 항목은 게시하지 않습니다. 글자로 되돌아가지 마십시오.

---

## 4. 알아 둘 대가

이 수정으로 **가로채기가 한 번도 잡지 못한 앱은 보충으로도 나타나지 않게 됩니다.** 사전에 주인 정보가 없기 때문입니다.

사용자가 "글자는 보고 싶지 않다"고 명시했으므로 이 대가를 받아들입니다. 다만 **그런 앱이 실제로 몇 개인지 로그로 세십시오.**

```
[tray] fill skip unknown tip="..."
```

사전에 없어서 걸러낸 툴팁을 툴팁마다 한 번씩만 남기십시오. 이 줄이 자주 보이면 설계를 다시 봐야 하므로 **검증 결과에 반드시 포함해 보고하십시오.**

---

## 5. 검증

측정값으로 판정하십시오.

1. bamti 를 실행하고 카카오톡을 켭니다. 상단바 트레이에 카카오톡이 **아이콘으로** 보여야 합니다. 글자가 아니어야 합니다. 화면 확인은 사용자에게 부탁하십시오.
2. 카카오톡 창을 모두 닫아 트레이로 보냅니다. 항목이 그대로 있어야 합니다.
3. **카카오톡을 완전히 종료합니다.** 로그에 `[tray] fill key=` 가 새로 나오면 안 됩니다. `K` 가 한 번도 나타나지 않아야 합니다. 사용자에게 화면 확인을 부탁하십시오.
4. 카카오톡을 다시 실행합니다. 가로채기가 놓친 회차에서도 보충이 **아이콘과 함께** 들어오는지 확인합니다.
5. `[tray] fill skip unknown` 이 몇 개의 서로 다른 툴팁에서 나왔는지 세어 보고하십시오.
6. 5분 이상 돌리고 `fill enum slow` 와 CPU 점유율이 나빠지지 않았는지 확인하십시오. 직전 측정값은 코어 하나 기준 0.9퍼센트였습니다.

사용자 화면에 입력을 합성하지 마십시오. 레지스트리에 쓰지 마십시오.

---

## 6. 건드리지 말 것

- `kFillRefreshMs` 5초 주기와 `TakeStartupFillPulse` 펄스
- `InterceptOwns` 와 `SameFillItem` 의 대조 규칙
- `Publish` 의 글리프 대체 경로 자체. 가로채기 항목은 지금처럼 두고, 보충 항목만 게시 전에 걸러냅니다
- `InvokeByExe` 와 `Dock::RevealDockApp`
- `NeedProcessRecheck` 와 `InvalidateLiveProcessCache`
- `src/menu_bar.cpp` 전체
