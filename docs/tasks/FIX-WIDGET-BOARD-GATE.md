# 수정 지시서: 위젯 보드가 없는 환경에서 단추를 감춘다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-VOLUME-WIDGET.md`를 끝낸 다음에 이어서 하십시오. 두 작업이 `src/widgets/builtin.cpp`와 `src/menu_bar.cpp`의 같은 구역을 건드리므로 순서를 지켜야 충돌이 없습니다.

---

## 1. 결함

내장 위젯의 "위젯 보드 단추"(`bamti.widget/board`)는 누르면 `SendInput`으로 Win+W를 합성합니다.

```cpp
void OpenWidgetBoard() {
  INPUT in[4]{};
  in[0].ki.wVk = VK_LWIN;
  in[1].ki.wVk = 'W';
  ...
  SendInput(4, in, sizeof(INPUT));
}
```

이 단축키는 Windows 11의 위젯 보드가 그 컴퓨터에 실제로 있을 때만 뜻이 있습니다. 보드를 제공하는 패키지가 설치되어 있지 않거나 사용자가 위젯을 꺼 두었으면, 단추를 눌러도 아무 일이 일어나지 않습니다. 그런데도 단추는 상단바 자리를 차지하고, 켜고 끄는 메뉴 항목도 멀쩡히 켜지는 것처럼 보입니다. 눌러도 반응이 없는 단추는 고장으로 읽힙니다.

**이 컴퓨터가 바로 그 환경입니다.** 2026-08-31 측정입니다.

| 확인 대상 | 값 |
|---|---|
| `MicrosoftWindows.Client.WebExperience` 패키지 | 현재 사용자에게 **등록되어 있지 않음** |
| `HKCU\...\Explorer\Advanced\TaskbarDa` | **0** (위젯 꺼짐) |

Win+W를 눌러도 보드가 뜨지 않는 것이 정상 동작인 상태입니다. 그러므로 이 컴퓨터에서 고친 결과는 **단추가 보이지 않는 것**이어야 합니다.

---

## 2. 판정 기준

두 가지를 함께 봅니다. **둘 다 만족해야** 위젯 보드를 쓸 수 있다고 판정합니다.

### 2-1. 패키지가 등록되어 있는가

위젯 보드는 `MicrosoftWindows.Client.WebExperience` 패키지가 제공합니다. 패키지 패밀리 이름은 다음과 같습니다.

```
MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy
```

`appmodel.h`의 `FindPackagesByPackageFamilyName`으로 확인합니다. kernel32에 있으므로 별도 라이브러리를 링크할 필요가 없고, WinRT도 필요 없습니다.

```cpp
#include <appmodel.h>

bool WebExperienceInstalled() {
  UINT32 count = 0;
  UINT32 bytes = 0;
  const LONG rc = FindPackagesByPackageFamilyName(
      L"MicrosoftWindows.Client.WebExperience_cw5n1h2txyewy",
      PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT,
      &count, nullptr, &bytes, nullptr, nullptr);
  // 등록된 것이 있으면 버퍼가 모자라다는 응답과 함께 count가 채워진다.
  if (rc == ERROR_INSUFFICIENT_BUFFER) {
    return count > 0;
  }
  if (rc == ERROR_SUCCESS) {
    return count > 0;
  }
  return false;
}
```

버퍼를 실제로 받을 필요가 없습니다. 개수만 보면 됩니다. 반환값이 위 둘 말고 다른 오류이면 **없는 것으로 판정합니다.** 판정하지 못한 상태에서 단추를 살려 두면 원래 결함이 그대로 남기 때문입니다.

`PACKAGE_FILTER_HEAD | PACKAGE_FILTER_DIRECT`는 현재 사용자에게 등록된 주 패키지를 찾습니다. 다른 사용자 계정에만 설치된 패키지는 이 프로세스에서 쓸 수 없으므로 세지 않는 것이 맞습니다.

### 2-2. 사용자가 위젯을 켜 두었는가

```
HKCU\Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced
값 이름: TaskbarDa   형식: REG_DWORD
```

- `0`이면 사용자가 위젯을 껐습니다.
- `1`이면 켜져 있습니다.
- **값이 없으면 켜진 것으로 간주합니다.** 이 값은 사용자가 설정을 건드릴 때 비로소 생깁니다.
- 값이 있는데 `REG_DWORD`가 아니면 켜진 것으로 간주합니다.

`RegGetValueW`로 읽으십시오. `RegOpenKeyEx` + `RegQueryValueEx` 조합보다 짧고, 형식 검사를 함께 해 줍니다.

```cpp
bool TaskbarWidgetsEnabled() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LSTATUS rc = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\Advanced",
      L"TaskbarDa", RRF_RT_REG_DWORD, nullptr, &value, &size);
  if (rc != ERROR_SUCCESS) {
    return true;   // 값이 없으면 켜져 있는 것으로 본다
  }
  return value != 0;
}
```

---

## 3. 어디에 넣는가

두 판정의 성격이 달라서 다시 확인하는 주기도 다릅니다.

| 판정 | 다시 확인하는 시점 |
|---|---|
| 패키지 등록 | **프로세스마다 한 번.** 설치와 제거는 드물고, 반영되려면 어차피 다시 로그온해야 합니다. |
| `TaskbarDa` | **30초마다.** 사용자가 설정 앱에서 언제든 바꿀 수 있습니다. |

`src/widgets/builtin.cpp`의 익명 이름 공간에 두 함수와 캐시를 둡니다.

```cpp
constexpr ULONGLONG kBoardCheckPeriodMs = 30000;

bool WidgetBoardAvailable() {
  static const bool package = WebExperienceInstalled();
  static ULONGLONG checked_at = 0;
  static bool enabled = false;
  if (!package) {
    return false;
  }
  const ULONGLONG now = GetTickCount64();
  if (checked_at == 0 || now - checked_at >= kBoardCheckPeriodMs) {
    enabled = TaskbarWidgetsEnabled();
    checked_at = now;
  }
  return enabled;
}
```

이 함수는 **작업자 스레드에서만** 부릅니다. 함수 지역 정적 변수의 초기화는 스레드 안전하지만 `checked_at`과 `enabled`는 그렇지 않으므로, 메뉴 스레드에서 같이 부르면 경합이 됩니다. 메뉴 쪽은 5절에서 따로 다룹니다.

### 3-1. 게시 억제

`WorkerLoop`의 게시 지점을 고칩니다.

```cpp
// 지금
if (s.widget_board) {
  PublishBoard();
}

// 고친 뒤
if (s.widget_board && WidgetBoardAvailable()) {
  PublishBoard();
} else if (s.widget_board) {
  DropItem(kBoardId);
}
```

`DropItem`을 함께 부르는 것이 중요합니다. 설정은 켜져 있는데 사용자가 도중에 위젯을 끄는 경우, 이미 올라간 항목이 그대로 남기 때문입니다. `DropItem`은 지문도 함께 비우므로 다시 쓸 수 있게 되면 정상으로 게시됩니다.

### 3-2. 동작 억제

`Execute`의 `kWidgetBoard` 갈래에서도 막습니다. 오버플로 팝업처럼 다른 경로로 클릭이 들어올 수 있습니다.

```cpp
case PendingAction::kWidgetBoard:
  if (!WidgetBoardAvailable()) {
    Log(L"widget", L"widget board unavailable; ignoring click");
    return;
  }
  OpenWidgetBoard();
  return;
```

로그가 클릭마다 쌓이지 않게, 이 경로는 `static bool logged`로 한 번만 남기십시오.

### 3-3. 시작 로그

`BuiltinWidgets::Start`에서 판정 결과를 한 번 남깁니다. 이 줄이 있어야 나중에 "왜 단추가 없느냐"를 로그만으로 가릴 수 있습니다.

```
[widget] board available=%d package=%d taskbar_da=%d
```

`taskbar_da`는 레지스트리를 읽은 결과 그대로 적습니다. 값이 없어서 기본값을 쓴 경우에는 `-1`로 적어 구분하십시오.

---

## 4. 판정 함수를 어디에 두는가

`src/widgets/builtin.cpp`의 익명 이름 공간에 둡니다. 새 파일을 만들지 마십시오. 쓰는 곳이 이 파일과 메뉴 한 군데뿐입니다.

메뉴에서도 써야 하므로 `builtin.hpp`에 조회 함수 하나만 공개합니다.

```cpp
// namespace bamti
bool IsWidgetBoardAvailable();
```

`.cpp`에서 이 함수가 `WidgetBoardAvailable()`을 부르되, **메뉴 스레드에서 불러도 안전하도록** 캐시 접근을 뮤텍스나 원자 변수로 감싸십시오. 30초에 한 번 갱신되는 두 개의 값이므로 `std::atomic<ULONGLONG>`과 `std::atomic<bool>` 한 쌍이면 충분하고, 두 값이 잠깐 어긋나도 결과가 틀어지지 않습니다. 락을 새로 만들지 마십시오. `BuiltinWidgets::mu_`를 여기에 끌어 쓰지도 마십시오. 이 판정은 인스턴스 상태가 아닙니다.

---

## 5. 메뉴

`src/menu_bar.cpp`의 `ShowContextMenu`에서 위젯 보드 항목을 그릴 때 판정을 반영합니다.

```cpp
const bool board_ok = IsWidgetBoardAvailable();
UINT board_flags = MF_STRING | (s.widget_board ? MF_CHECKED : 0);
if (!board_ok) {
  board_flags |= MF_GRAYED;
}
AppendMenuW(menu, board_flags, kWidgetBoardCmd,
            board_ok ? L"위젯 보드 단추" : L"위젯 보드 단추 (이 PC에서 사용할 수 없습니다)");
```

`MF_GRAYED` 항목은 클릭해도 `WM_COMMAND`가 오지 않으므로 명령 처리 쪽은 손대지 않아도 됩니다.

**설정값 `widget_board`를 강제로 끄지 마십시오.** 지금 쓸 수 없다는 것과 사용자가 원하지 않는다는 것은 다릅니다. 나중에 패키지를 설치하거나 위젯을 다시 켜면 저장해 둔 선택이 그대로 살아나야 합니다.

---

## 6. 하지 말아야 할 것

- **`OpenWidgetBoard`의 `SendInput` 방식을 바꾸지 마십시오.** 공개된 호스트 API가 없다는 것은 `PLAN-TRAY-TO-TOPBAR.md`에서 이미 정리된 사항입니다. 이번 작업은 언제 누를 수 있는지를 가리는 것까지입니다.
- 패키지를 찾는 데 WinRT(`Windows.Management.Deployment.PackageManager`)를 쓰지 마십시오. 아파트먼트와 초기화 부담이 붙습니다.
- `HKLM` 정책 키(`Dsh\AllowNewsAndInterests` 등)까지 보지 마십시오. 조건이 늘수록 판정이 어긋날 때 원인을 가리기 어려워집니다. 2절의 두 가지로 충분하다는 판단이고, 부족하다는 근거가 나오면 그때 늘립니다.
- 판정을 캐시 없이 매 루프마다 하지 마십시오. `FindPackagesByPackageFamilyName`은 레지스트리 조회보다 훨씬 비쌉니다.
- 다른 세 위젯(배터리, CPU, 네트워크, 그리고 새로 넣은 볼륨)의 게시 조건을 건드리지 마십시오.

---

## 7. 검증

1. 빌드가 Debug와 Release에서 `/W4` 경고 없이 통과합니다.
2. **이 컴퓨터에서 위젯 보드 단추를 켭니다.** 상단바에 단추가 나타나지 않아야 합니다.
3. 우클릭 메뉴에서 "위젯 보드 단추" 항목이 회색이고, 라벨에 사용할 수 없다는 표시가 붙습니다.
4. 로그에 다음이 한 번 남습니다. 실제 줄을 그대로 보고에 옮겨 적으십시오.
   ```
   [widget] board available=0 package=0 taskbar_da=0
   ```
5. `TaskbarDa`를 `1`로 바꿔 봅니다(`reg add`로 값만 바꾸고 explorer는 재시작하지 마십시오). 패키지가 없으므로 **여전히 사용할 수 없다고 판정되어야 합니다.** 확인한 뒤 값을 `0`으로 되돌리십시오.
6. 배터리와 CPU와 네트워크와 볼륨 위젯이 예전대로 켜지고 꺼집니다.

패키지가 설치된 환경은 이 컴퓨터에서 만들 수 없습니다. **그 경우의 동작은 확인하지 못했다고 보고에 적으십시오.** 판정 함수를 임시로 뒤집어 확인해 보는 것은 괜찮지만, 그 임시 코드를 커밋에 남기지 마십시오.

---

## 8. 커밋

```
fix: 위젯 보드가 없는 환경에서 단추를 감춘다
```
