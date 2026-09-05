# 작업 지시서: 상단바 우클릭 메뉴의 항목을 정리하고 서브메뉴로 묶는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-MENU-STYLE.md` 다음에 하십시오. **순서를 지켜야 합니다.** 그 지시서가 `BarMenuContent`의 행 타입을 `MenuRow`로 바꾸고, 이 지시서는 그 위에서 항목 구성을 바꿉니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, `src/bar_menu.hpp`, `src/bar_menu.cpp`입니다.

---

## 1. 먼저 고칠 결함: 명령 번호가 겹쳐 있다

`src/menu_bar.cpp` 익명 이름 공간에 두 상수가 같은 값을 쓰고 있습니다.

```cpp
constexpr UINT kWidgetControlCenterCmd = 20;
constexpr UINT kTrayPeekCmd = 20;
```

두 항목은 `ShowContextMenu`가 **같은 메뉴에 함께 넣습니다.** `WM_COMMAND` 처리부는 `if` 문을 순서대로 늘어놓았을 뿐 `else`로 갈라 두지 않았으므로, 어느 쪽을 눌러도 제어 센터 표시가 뒤집히고 **동시에** 알림 영역 잠시 표시가 시작됩니다.

`kTrayPeekCmd`를 24로 옮기십시오. 21, 22, 23은 이미 쓰고 있으므로 비어 있는 첫 값이 24입니다.

이 결함은 아래의 항목 개편과 별개로 그 자체가 버그입니다. **개편 전에 먼저 고치고, 고쳤다는 사실을 커밋 메시지에 남기십시오.**

---

## 2. 지금 메뉴의 문제

`MenuBar::ShowContextMenu`가 만드는 열네 줄이 성격 구분 없이 한 덩어리로 늘어서 있습니다. 상단바에 무엇을 보일지 고르는 항목, 트레이 미러의 동작을 바꾸는 항목, 한 번 실행하고 마는 명령, 앱 자체의 설정이 뒤섞여 있습니다.

없애야 할 항목은 다음과 같습니다.

1. **`위젯 보드 단추 (이 PC에서 사용할 수 없습니다)`** — 이 컴퓨터에는 `MicrosoftWindows.Client.WebExperience` 패키지가 없어 항상 꺼진 채로 자리만 차지합니다(`FIX-WIDGET-BOARD-GATE.md`의 실측). `IsWidgetBoardAvailable()`이 거짓이면 **행 자체를 넣지 마십시오.** 참일 때만 넣습니다. 명령과 설정 필드(`WidgetSettings::widget_board`)는 그대로 둡니다.
2. **`트레이 아이콘 가로채기(실험)`** — 실험 항목입니다. 메뉴에서 뺍니다. `WidgetSettings::tray_backend` 필드와 `kTrayInterceptCmd`의 처리 경로는 **지우지 말고 그대로 두십시오.** 설정 파일을 고쳐서 여전히 바꿀 수 있어야 하고, 나중에 설정 페이지가 생기면 그리로 옮깁니다.
3. **`ShowTrayIconMenu`의 설명 세 줄** — `앱 메뉴는 알림 영역 잠시 표시로 엽니다`, `숨긴 아이콘도 미러합니다...`, `지금은 글리프만 표시합니다`는 개발 중에 상황을 적어 둔 안내문입니다. 지웁니다.

---

## 3. 새 구성

최상위를 네 덩어리로 줄입니다.

```
표시 항목                    ▸
트레이 아이콘                ▸
──────────────
bamti 설정
로그인 시 bamti 시작     ✓
──────────────
bamti 종료
```

`표시 항목` 서브메뉴입니다. 상단바에 무엇을 보일지만 고릅니다.

```
배터리          ✓
CPU             ✓
네트워크        ✓
블루투스        ✓
볼륨            ✓
제어 센터       ✓
위젯 보드 단추          ← IsWidgetBoardAvailable()이 참일 때만
```

`트레이 아이콘` 서브메뉴입니다. 지금 트레이 서브메뉴가 하던 일에 미러 설정을 함께 담습니다.

```
Everything                  ✓
KakaoTalk                   ✓
Tailscale: Connected…       ✓
(미러 중인 아이콘들)
──────────────
트레이 미러                 ✓
시스템 아이콘도 표시        ✓
숨긴 아이콘도 표시          ✓
──────────────
알림 영역 잠시 표시
```

미러 중인 아이콘이 없으면 목록 자리에 지금처럼 `미러 중인 아이콘이 없습니다`를 꺼진 행으로 하나 넣고, 그 아래 구분선과 설정 항목은 그대로 보입니다.

`종료`는 `bamti 종료`로 바꿉니다. 트레이 아이콘 목록에 남의 앱 이름이 늘어서는 메뉴이므로, 무엇을 끄는 종료인지 이름에 드러나야 합니다.

### `bamti 설정`에 대하여

**이번에는 항목만 넣습니다.** 실제 설정 창은 다음 작업입니다.

- 새 명령 `constexpr UINT kSettingsCmd = 25;`를 더합니다.
- 행은 **켜진 상태로** 넣습니다. 흐리게 두면 영영 안 되는 기능처럼 보입니다.
- `WM_COMMAND`에서 `kSettingsCmd`를 받으면 `Log(L"bar", L"settings page not implemented yet");` 한 줄만 남기고 아무 창도 열지 마십시오. 그 자리에 `// TODO: 설정 페이지` 주석을 답니다.

---

## 4. 서브메뉴를 여러 개 지원해야 한다

지금 `BarMenuContent`는 서브메뉴를 **하나만** 가정합니다. `SubmenuIndex()`가 `submenu`가 참인 첫 행을 찾아 돌려주고, `MenuBar::OpenTraySubmenu`가 그 하나를 트레이 목록으로 채웁니다. 서브메뉴가 둘이 되므로 이 가정을 풀어야 합니다.

### 4-1. `BarMenuContent`

`SubmenuIndex()`는 지우고 둘을 더합니다.

```cpp
UINT SubmenuIdAt(int index) const;      // 그 행이 서브메뉴 행이면 행의 id, 아니면 0
int RowIndexOfCommand(UINT id) const;   // 없으면 -1
```

서브메뉴 행에도 고유한 명령 번호를 줍니다. 지금은 `bar_menu_->Add(0, L"트레이 아이콘", false, true, true);`처럼 0을 넘기고 있습니다.

```cpp
constexpr UINT kMenuWidgetsSubCmd = 30;
constexpr UINT kMenuTraySubCmd = 31;
```

`Invoke`는 지금처럼 **서브메뉴 행에서는 `WM_COMMAND`를 보내지 않아야 합니다.** `row.submenu`가 참이면 그대로 돌아가도록 조건을 더하십시오. 지금은 id가 0이라 우연히 걸러지고 있었을 뿐입니다.

`StickyRow`는 `rows_[index].submenu`를 그대로 돌려주므로 고칠 것이 없습니다.

### 4-2. `MenuBar`

`OpenTraySubmenu`와 `SyncTraySubmenu`와 `CloseTraySubmenu`의 이름을 각각 `OpenBarSubmenu`, `SyncBarSubmenu`, `CloseBarSubmenu`로 바꾸고, 여는 함수는 어느 서브메뉴인지 인자로 받습니다.

```cpp
void OpenBarSubmenu(UINT cmd);
void CloseBarSubmenu(const wchar_t* reason);
void SyncBarSubmenu();
UINT open_submenu_cmd_ = 0;   // 열려 있지 않으면 0
```

`SyncBarSubmenu`의 판정은 이렇게 바뀝니다.

1. 부모 팝업이 닫혔으면 서브메뉴도 닫고 끝냅니다. 지금과 같습니다.
2. 커서가 열려 있는 서브메뉴 창 안에 있으면 그대로 둡니다. 지금과 같습니다.
3. 아니면 부모의 `Hot()` 행을 보고 `SubmenuIdAt`으로 명령 번호를 얻습니다.
   - 0이면 서브메뉴를 닫습니다.
   - `open_submenu_cmd_`와 같으면 아무것도 하지 않습니다.
   - 다르면 **닫고 나서 새로 엽니다.** 이 갈래가 새로 필요한 부분입니다. 서브메뉴 행 사이를 오갈 때 내용이 바뀌어야 합니다.

`OpenBarSubmenu(cmd)`는 `cmd`에 따라 3절의 두 목록 중 하나를 채웁니다. 위치는 지금처럼 `RowScreenRect`로 얻은 행의 오른쪽 위 모서리에 `PopupSurface::Anchor::RightOf`로 붙입니다. 행 번호는 `RowIndexOfCommand(cmd)`로 얻습니다.

트레이 목록을 채울 때는 `TASK-BAR-MENU-STYLE.md` 3절대로 `bar_submenu_->SetMaxWidthDip(360)`을 부르고, `표시 항목`을 채울 때는 부르지 않습니다.

`tray_menu_keys_`는 트레이 서브메뉴를 열 때만 채웁니다. `표시 항목`을 열 때 지워지지 않도록 주의하십시오. `kTrayItemCmdBase` 이후의 명령은 이 배열의 첨자로 쓰이므로, 엉뚱한 시점에 비면 클릭이 무시됩니다.

---

## 5. 검증

1. **빌드.** Release 클린 빌드가 경고 없이 통과해야 합니다.
2. **번호 충돌.** `알림 영역 잠시 표시`를 고르면 잠시 표시만 되고 제어 센터 항목의 체크가 바뀌지 않아야 합니다. 반대로 `제어 센터`를 켜고 끌 때 알림 영역이 나타나지 않아야 합니다. 이 작업 전에는 둘 다 틀렸습니다.
3. **서브메뉴 전환.** 최상위에서 `표시 항목`과 `트레이 아이콘` 사이로 마우스를 왕복시키십시오. 서브메뉴 내용이 따라 바뀌고, 이전 서브메뉴가 남아 있지 않아야 합니다.
4. **서브메뉴 안으로 들어가기.** `표시 항목`에 마우스를 올려 서브메뉴가 열린 뒤, 오른쪽으로 이동해 서브메뉴 안의 항목을 누를 수 있어야 합니다. 도중에 닫히면 `over_sub` 판정을 잘못 옮긴 것입니다.
5. **토글이 실제로 먹는지.** 서브메뉴에서 배터리를 끄면 상단바에서 배터리가 사라지고, 메뉴를 다시 열었을 때 체크가 꺼져 있어야 합니다.
6. **트레이 아이콘 숨기기.** 트레이 서브메뉴의 아이콘 이름을 눌러 체크를 끄면 상단바에서 그 아이콘이 사라져야 합니다. 미러 목록과 그 아래 설정 항목의 명령 번호가 섞이지 않았는지 확인하는 절차입니다.
7. **`bamti 설정`.** 누르면 아무 일도 일어나지 않고 로그에 한 줄만 남아야 합니다. 메뉴는 닫혀야 합니다.
8. **위젯 보드.** 이 컴퓨터에서는 `표시 항목`에 위젯 보드 행이 아예 없어야 합니다.

레지스트리에 쓰는 확인 절차는 넣지 마십시오.
