# 수정 지시서: 상단바 우클릭 메뉴가 다크 모드에서도 라이트로 표시된다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-DOCK-ICON-BLUR.md` 다음에 하십시오.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_bar.hpp`, 새로 만드는 `src/bar_menu.hpp`와 `src/bar_menu.cpp`, 그리고 `CMakeLists.txt`입니다.

---

## 1. 증상

시스템이 다크 모드일 때에도 상단바를 우클릭해서 여는 메뉴만 흰 배경에 검은 글자로 그려집니다. 같은 앱의 독 아이콘 우클릭 메뉴와 시계 우클릭 메뉴는 다크로 정상 표시됩니다.

---

## 2. 근본 원인

`MenuBar::ShowContextMenu`(`src/menu_bar.cpp:1813`)와 `MenuBar::ShowTrayIconMenu`(`src/menu_bar.cpp:1874`)만 Win32 시스템 메뉴를 쓰고 있습니다.

```cpp
const HMENU menu = CreatePopupMenu();
...
TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, screen.x, screen.y, hwnd_, nullptr);
```

`TrackPopupMenuEx`가 그리는 메뉴 창은 시스템이 소유하고 시스템 비주얼 스타일로 칠해집니다. 앱이 자기 창에 어떤 다크 설정을 걸어 두었든 이 메뉴에는 전달되지 않습니다. 그래서 `dark_` 값이 참이어도 메뉴만 라이트로 남습니다.

반면 독 메뉴는 `Dock::OpenDockMenu`(`src/dock.cpp:2279`)에서 `PopupSurface`에 `DockMenuContent`를 얹어 직접 그리고, 시계 메뉴는 `MenuBar::ShowClockMenu`(`src/menu_bar.cpp:1592`)에서 같은 방식으로 `ClockMenuContent`를 그립니다. 둘 다 `dark_`를 받아 색을 고르므로 테마를 따라갑니다.

`ShowContextMenu` 안에는 이미 다음 주석이 있습니다.

```cpp
// 7단계에서 항목 표시 설정 전체를 PopupSurface로 옮길 임시 메뉴다.
```

즉 이 교체는 원래 계획된 방향이며, 이번 작업이 그 자리를 채웁니다.

**Win32 메뉴를 다크로 칠하려는 우회는 쓰지 마십시오.** `uxtheme`의 서수 135번 `SetPreferredAppMode`나 `WM_UAHDRAWMENU` 같은 비공개 통로에 의존해야 하고, 그렇게 해도 메뉴 창의 테두리와 여백은 시스템이 칠하므로 완전한 다크가 되지 않습니다. 이 저장소에는 이미 잘 동작하는 자체 메뉴 인프라가 있으므로 그쪽으로 모으는 것이 맞습니다.

---

## 3. 만들 것: 범용 메뉴 콘텐츠

새 파일 `src/bar_menu.hpp`와 `src/bar_menu.cpp`에 `BarMenuContent`를 정의합니다. `PopupContent`를 구현하며, 행 목록을 받아 그리고 선택된 행의 명령 번호를 창으로 되돌려 보내는 것이 전부입니다.

### 3-1. 행 모델

```cpp
struct BarMenuRow {
  UINT id = 0;
  std::wstring text;
  bool separator = false;
  bool checked = false;
  bool enabled = true;
  bool submenu = false;
};
```

`DockMenuRow`(`src/dock.cpp:738`)와 같되 `enabled` 하나가 늘었습니다. 상단바 메뉴에는 `MF_GRAYED`로 표시하던 안내 문구가 여럿 있기 때문입니다.

### 3-2. 클래스

```cpp
class BarMenuContent : public PopupContent {
 public:
  void Reset(HWND target, bool dark);
  void Add(UINT id, std::wstring text, bool checked = false, bool enabled = true, bool submenu = false);
  void AddSeparator();
  void SetPopup(PopupSurface* popup);   // RowScreenRect 계산에 필요하다

  bool empty() const;
  int SubmenuIndex() const;             // submenu 가 참인 첫 행, 없으면 -1
  bool RowScreenRect(int index, RECT* out) const;

  int RowCount() const override;
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;

 private:
  RECT RowRect(int index, UINT dpi, int width) const;

  HWND target_ = nullptr;
  PopupSurface* popup_ = nullptr;
  bool dark_ = true;
  std::vector<BarMenuRow> rows_;
};
```

### 3-3. 구현 규칙

**치수와 배치**는 `DockMenuContent::Measure`(`src/dock.cpp:849`)와 `RowRect`(`src/dock.cpp:966`)를 그대로 옮겨 씁니다. 상수도 같은 값을 쓰십시오. `src/dock.cpp`의 익명 네임스페이스에 있는 값들이므로 `src/bar_menu.cpp`의 익명 네임스페이스에 같은 이름으로 다시 선언하면 됩니다.

```cpp
constexpr int kMenuPadDip = 6;
constexpr int kMenuRowDip = 28;
constexpr int kMenuSepDip = 8;
constexpr int kMenuMinWidthDip = 168;
constexpr int kMenuMaxWidthDip = 280;
constexpr int kMenuTextPadDip = 12;
constexpr int kMenuCheckDip = 16;
constexpr int kMenuArrowDip = 14;
```

다만 상단바 메뉴에는 "숨긴 아이콘도 미러합니다. 클릭 반응이 없으면 알림 영역 잠시 표시로 여세요" 같은 긴 안내 문구가 있으므로, `kMenuMaxWidthDip`만 `360`으로 올리십시오. 그래도 넘치는 문구는 `DrawPopupText`가 잘라 줍니다.

**그리기**는 `DockMenuContent::Render`(`src/dock.cpp:874`)를 본뜨되 두 가지를 더합니다.

1. 비활성 행의 글자는 흐리게 칠합니다. 색은 `ClockTextColor(dark_)`의 알파를 `0.38f`로 줄인 값을 쓰십시오. `ClockFlyoutContent`가 같은 비율을 쓰고 있으므로(`src/clock_flyout.cpp:721`) 앱 안에서 일관됩니다. `ScaleAlpha`는 `src/clock_flyout.cpp`의 익명 네임스페이스에만 있으므로, 그 함수를 옮기지 말고 `src/bar_menu.cpp`에 같은 내용을 다시 두십시오.
2. 비활성 행에는 호버 채움을 그리지 않습니다.

체크 표시(`✓`)와 서브메뉴 화살표(`›`)는 독 메뉴와 같은 문자를 같은 자리에 그리십시오.

**히트 테스트**는 구분선과 비활성 행에서 `-1`을 돌려주어야 합니다. 그래야 마우스를 올려도 강조되지 않고 눌러도 아무 일이 일어나지 않습니다.

**실행**은 창에 메시지를 보내는 것으로 끝냅니다.

```cpp
void BarMenuContent::Invoke(int index) {
  if (target_ == nullptr || index < 0 || index >= static_cast<int>(rows_.size())) {
    return;
  }
  const BarMenuRow& row = rows_[static_cast<size_t>(index)];
  if (row.id == 0 || !row.enabled || row.separator) {
    return;
  }
  PostMessageW(target_, WM_COMMAND, MAKEWPARAM(row.id, 0), 0);
}
```

`SendMessageW`가 아니라 **`PostMessageW`여야 합니다.** `Invoke`는 팝업이 닫히는 처리 안에서 불리므로, 여기서 곧바로 설정을 바꾸고 다시 그리면 재진입이 생깁니다. `Dock`이 `pending_menu_cmd_`로 명령을 미루는 것과 같은 이유이며, `PostMessageW`를 쓰면 그 미룸이 저절로 이루어집니다.

이 설계의 이점은 `MenuBar::HandleMessage`의 `WM_COMMAND` 분기(`src/menu_bar.cpp:709`)를 한 줄도 고치지 않아도 된다는 것입니다. 명령 번호 상수와 처리 코드가 전부 그대로 살아 있습니다.

---

## 4. `MenuBar` 쪽 변경

### 4-1. 멤버 추가

`src/menu_bar.hpp`에 다음을 더하십시오. `clock_menu_` 옆에 두면 됩니다.

```cpp
std::unique_ptr<BarMenuContent> bar_menu_;
std::unique_ptr<BarMenuContent> bar_submenu_;
PopupSurface bar_submenu_popup_;
```

부모 메뉴는 이미 있는 `status_popup_`을 씁니다. 시계 메뉴가 같은 표면을 쓰고 있으므로 선례가 있습니다. 서브메뉴는 부모와 동시에 떠 있어야 하므로 표면을 하나 더 두어야 합니다.

선언도 함께 더하십시오.

```cpp
void SyncTraySubmenu();
void OpenTraySubmenu();
void CloseTraySubmenu(const wchar_t* reason);
static void AfterBarPopupTick(void* ctx);
```

`bar_submenu_popup_`은 `MenuBar::Create`에서 `status_popup_`을 만드는 자리 바로 뒤에 같은 방식으로 `Create(instance, hwnd_)` 하십시오.

### 4-2. `ShowContextMenu` 교체

`CreatePopupMenu`부터 `DestroyMenu`까지를 전부 걷어내고 다음 형태로 바꿉니다. 항목의 순서, 문구, 체크 조건, 명령 번호는 **지금 코드와 한 글자도 다르지 않아야 합니다.**

```cpp
void MenuBar::ShowContextMenu(POINT screen) {
  if (fullscreen_occluded_) {
    return;
  }
  if (!bar_menu_) {
    bar_menu_ = std::make_unique<BarMenuContent>();
  }
  CloseTraySubmenu(L"reopen");
  bar_menu_->Reset(hwnd_, dark_);
  bar_menu_->SetPopup(&status_popup_);

  const WidgetSettings s = widgets_.settings();
  bar_menu_->Add(kWidgetBatteryCmd, L"배터리", s.battery);
  bar_menu_->Add(kWidgetCpuCmd, L"CPU", s.cpu);
  bar_menu_->Add(kWidgetNetworkCmd, L"네트워크", s.network);
  bar_menu_->Add(kWidgetVolumeCmd, L"볼륨", s.volume);
  bar_menu_->Add(kWidgetWifiCmd, L"Wi-Fi", s.wifi);
  bar_menu_->Add(kWidgetControlCenterCmd, L"제어 센터", s.control_center);
  const bool board_ok = IsWidgetBoardAvailable();
  bar_menu_->Add(kWidgetBoardCmd,
                 board_ok ? L"위젯 보드 단추" : L"위젯 보드 단추 (이 PC에서 사용할 수 없습니다)",
                 s.widget_board, board_ok);
  const WidgetSettings tray = tray_.settings();
  bar_menu_->Add(kTrayMirrorToggleCmd, L"트레이 미러", tray.tray_mirror);
  bar_menu_->Add(kTraySystemIconsCmd, L"시스템 아이콘도 표시", tray.tray_system_icons);
  bar_menu_->Add(kTrayOverflowIconsCmd, L"숨긴 아이콘도 표시", tray.tray_overflow_icons);
  bar_menu_->Add(kTrayInterceptCmd, L"트레이 아이콘 가로채기(실험)", tray.tray_backend == "intercept");
  bar_menu_->Add(0, L"트레이 아이콘", false, true, true);
  bar_menu_->Add(kTrayPeekCmd, L"알림 영역 잠시 표시");
  bar_menu_->Add(kAutostartCmd, L"로그인 시 bamti 시작", AutostartEnabled());
  bar_menu_->AddSeparator();
  bar_menu_->Add(kExitCommand, L"종료");

  cc_open_ = false;
  clock_open_ = false;
  open_panel_id_.clear();
  status_popup_.SetDark(dark_);
  status_popup_.SetAfterTick(&MenuBar::AfterBarPopupTick, this);
  if (!status_popup_.Open(bar_menu_.get(), screen, PopupSurface::Anchor::BelowAt)) {
    Log(L"bar", L"context menu open failed err=%lu", GetLastError());
  }
}
```

세 가지를 짚어 둡니다.

- **앵커는 `BelowAt`입니다.** 지금 코드의 `TPM_BOTTOMALIGN`은 메뉴가 클릭 지점 위로 열리라는 뜻이었고, 상단바에서는 화면 밖으로 나가므로 Windows가 매번 아래로 뒤집어 주고 있었습니다. 자체 표면에는 그런 자동 보정이 없으므로 처음부터 아래로 여는 것이 맞습니다.
- **`SetForegroundWindow` 호출과 이전 포그라운드 복원 코드는 지우십시오.** `PopupSurface`는 마우스 캡처로 닫힘을 감지하므로 포그라운드를 빼앗을 필요가 없습니다. `PLAN.md` 1-1절이 활성화 의존을 버린 이유를 설명하고 있습니다.
- **`cc_open_`, `clock_open_`, `open_panel_id_` 초기화가 필요합니다.** `status_popup_`을 제어 센터와 시계 플라이아웃이 공유하므로, 상태를 정리하지 않으면 메뉴가 뜬 뒤에도 앱이 패널이 열려 있다고 착각합니다. `ShowClockMenu`가 같은 처리를 하고 있으니 그대로 따르십시오.

`tray_menu_keys_`를 채우는 일은 서브메뉴를 만들 때로 옮깁니다.

### 4-3. 트레이 아이콘 서브메뉴

`Dock::SyncOptionsSubmenu`, `Dock::OpenOptionsSubmenu`, `Dock::CloseOptionsSubmenu`(`src/dock.cpp:2324` ~ `2384`)를 그대로 본떠 만드십시오. 호버가 서브메뉴 행에 있거나 마우스가 서브메뉴 창 안에 있으면 열고, 둘 다 아니면 닫는 구조입니다.

```cpp
void MenuBar::AfterBarPopupTick(void* ctx) {
  if (ctx != nullptr) {
    static_cast<MenuBar*>(ctx)->SyncTraySubmenu();
  }
}
```

`OpenTraySubmenu`는 다음 행을 채웁니다. 문구와 명령 번호는 지금 코드와 같아야 합니다.

```cpp
tray_menu_keys_.clear();
const std::vector<TrayMirror::MenuItem> entries = tray_.MenuItems();
if (entries.empty()) {
  bar_submenu_->Add(0, L"미러 중인 아이콘이 없습니다", false, false);
} else {
  const size_t n = (std::min)(entries.size(), kTrayHiddenKeysMax);
  tray_menu_keys_.reserve(n);
  for (size_t i = 0; i < n; ++i) {
    bar_submenu_->Add(kTrayItemCmdBase + static_cast<UINT>(i), entries[i].label, entries[i].shown);
    tray_menu_keys_.push_back(entries[i].key);
  }
  if (entries.size() > kTrayHiddenKeysMax) {
    bar_submenu_->Add(0, L"이하 생략", false, false);
  }
}
```

여는 방식도 독과 같습니다.

```cpp
RECT row{};
if (!bar_menu_->RowScreenRect(bar_menu_->SubmenuIndex(), &row)) {
  return;
}
const POINT anchor{row.right, row.top};
status_popup_.SetAllied(&bar_submenu_popup_);
bar_submenu_popup_.SetDark(dark_);
if (!bar_submenu_popup_.Open(bar_submenu_.get(), anchor, PopupSurface::Anchor::RightOf, false)) {
  status_popup_.SetAllied(nullptr);
  Log(L"bar", L"tray submenu open failed err=%lu", GetLastError());
}
```

마지막 인자 `false`는 서브메뉴가 마우스 캡처를 가져가지 않게 하는 값입니다. 독에서도 같은 값을 씁니다. 이 값을 바꾸면 부모 메뉴가 곧바로 닫히므로 그대로 두십시오.

### 4-4. `ShowTrayIconMenu` 교체

같은 방식으로 옮기되 서브메뉴가 없으므로 훨씬 단순합니다. 안내 문구 세 줄은 `enabled`를 거짓으로 넣으십시오.

```cpp
bar_menu_->Add(kTrayPeekCmd, L"알림 영역 잠시 표시");
bar_menu_->Add(kTrayHideIconCmd, L"이 아이콘 숨기기");
bar_menu_->Add(kTrayMirrorOffCmd, L"트레이 미러 끄기");
bar_menu_->AddSeparator();
bar_menu_->Add(0, L"앱 메뉴는 알림 영역 잠시 표시로 엽니다", false, false);
bar_menu_->Add(0, L"숨긴 아이콘도 미러합니다. 클릭 반응이 없으면 알림 영역 잠시 표시로 여세요", false, false);
bar_menu_->Add(0, L"지금은 글리프만 표시합니다", false, false);
```

`tray_menu_id_ = id;` 는 그대로 유지해야 합니다. `kTrayHideIconCmd` 처리가 이 값을 씁니다.

이 메뉴에서는 `SetAfterTick`을 걸지 마십시오. 서브메뉴가 없으므로 필요 없고, 앞서 걸어 둔 것이 남아 있으면 엉뚱하게 불립니다. `ShowTrayIconMenu`에서는 `status_popup_.SetAfterTick(nullptr, nullptr)`로 지우고 여십시오.

### 4-5. 테마 전환 반영

`dark_ = ShellUsesDarkMode();`가 실행되는 자리(`src/menu_bar.cpp:283`과 `442`) 뒤에서, 메뉴가 열려 있으면 색을 다시 칠하게 하십시오.

```cpp
if (status_popup_.IsOpen() && bar_menu_ != nullptr) {
  bar_menu_->Reset(hwnd_, dark_);   // 행을 다시 채우기는 번거로우므로 dark 만 갱신하는 setter 를 두어도 좋다
  status_popup_.SetDark(dark_);
  status_popup_.Present();
}
```

행 목록을 통째로 다시 만들기가 번거로우면 `BarMenuContent::SetDark(bool)`를 따로 두고 그것만 부르십시오. 그편이 깔끔합니다.

---

## 5. 빌드 파일

`CMakeLists.txt`의 소스 목록에 `src/bar_menu.cpp`를 더하십시오. `src/popup_surface.cpp` 다음 줄이 자연스럽습니다.

`bamti.vcxproj`는 이미 `clock_flyout.cpp`와 `icon_cache.cpp` 같은 최근 파일이 빠져 있어 실제 빌드에 쓰이지 않습니다. **건드리지 마십시오.**

---

## 6. 검증

1. `out/cmake-debug` 구성으로 빌드가 경고 없이 통과해야 합니다. `/W4` 설정이므로 미사용 인자 경고도 남기지 마십시오.
2. 앱을 실행하고 상단바 빈 곳을 우클릭했을 때, 메뉴가 독 아이콘 우클릭 메뉴와 같은 어두운 배경과 같은 모서리 곡률로 그려져야 합니다.
3. 체크 표시가 붙는 항목이 교체 전과 같아야 합니다. 배터리, CPU, 네트워크, 볼륨, Wi-Fi, 제어 센터, 트레이 미러, 시스템 아이콘, 숨긴 아이콘, 트레이 가로채기, 로그인 시 시작이 각각 현재 설정과 맞는지 확인하십시오.
4. "위젯 보드 단추 (이 PC에서 사용할 수 없습니다)" 항목이 흐리게 보이고, 마우스를 올려도 강조되지 않으며, 눌러도 아무 일이 일어나지 않아야 합니다.
5. "트레이 아이콘" 행에 마우스를 올리면 오른쪽으로 서브메뉴가 열리고, 벗어나면 닫혀야 합니다. 서브메뉴 항목을 누르면 해당 아이콘의 표시 여부가 실제로 바뀌어야 합니다.
6. 각 항목을 눌러 설정이 실제로 반영되는지 확인하십시오. 특히 "볼륨"을 껐다 켰을 때 상단바에서 항목이 사라졌다 나타나야 합니다.
7. ESC 키와 메뉴 바깥 클릭으로 닫혀야 하고, 닫힌 뒤 상단바가 다시 정상 반응해야 합니다.
8. 상단바의 트레이 아이콘을 우클릭했을 때 뜨는 메뉴도 같은 기준으로 확인하십시오.
9. **다크 모드와 라이트 모드 전환은 직접 하지 마십시오.** 레지스트리를 건드리면 사용자 설정을 되돌리지 못할 수 있습니다. 두 모드에서의 확인이 필요하면 사용자에게 설정 앱에서 전환해 달라고 부탁하십시오.
10. 화면을 눈으로 확인해야 하는 항목은 입력을 합성하지 말고 사용자에게 부탁하십시오.

---

## 7. 하지 말아야 할 것

- `uxtheme`의 비공개 서수 API(`SetPreferredAppMode`, `AllowDarkModeForWindow`)를 부르지 마십시오.
- 메뉴를 오너 드로우(`MF_OWNERDRAW` + `WM_DRAWITEM`)로 바꾸는 절충안을 택하지 마십시오. 항목만 어두워지고 메뉴 창의 테두리와 여백은 라이트로 남습니다.
- `src/dock.cpp`의 `DockMenuContent`를 공용 클래스로 끌어내는 리팩터링을 함께 하지 마십시오. 독 메뉴는 창 목록, 드래그, 고정 항목이 얽혀 있어 이번 범위에서 건드리면 회귀 위험만 커집니다. 코드가 겹치는 것은 알고 있으며, 나중에 정리할 후보로만 남깁니다.
- `WM_COMMAND` 처리 코드와 명령 번호 상수를 바꾸지 마십시오. 그대로 재사용하는 것이 이 설계의 핵심입니다.
- 메뉴 항목의 문구를 다듬지 마십시오. 이번 작업은 표시 방식만 바꿉니다.
