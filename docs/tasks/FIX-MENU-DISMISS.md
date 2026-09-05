# 수정 지시서: 상단바 우클릭 메뉴가 바깥을 눌러도 닫히게 한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-SLIDER-ENDS.md` 다음에 하십시오. 파일은 겹치지 않지만 순서를 지켜야 검증이 섞이지 않습니다.

---

## 1. 결함

상단바를 우클릭하면 뜨는 메뉴가, 화면의 다른 곳을 눌러도 닫히지 않습니다. 상단바를 다시 눌러야만 사라집니다. 메뉴가 열린 채로 다른 창을 만지게 되므로 화면에 잔상처럼 남습니다.

`TrackPopupMenuEx`는 원래 바깥을 누르면 스스로 닫힙니다. 닫히지 않는 것은 **메뉴를 소유한 창이 포그라운드가 아니기 때문입니다.**

`src/menu_bar.cpp`의 창 생성입니다.

```cpp
hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TOPMOST, kMenuBarClass, L"bamti", WS_POPUP, 0, 0,
                        0, 0, nullptr, nullptr, instance, this);
```

`WS_EX_NOACTIVATE`가 붙어 있습니다. 상단바는 눌러도 활성화되지 않아야 하므로 이 스타일 자체는 옳습니다. 그러나 이 창이 소유한 팝업 메뉴는 포커스를 잃는 사건을 받지 못합니다. `TrackPopupMenu`가 바깥 클릭을 감지하는 경로가 소유 창의 활성 상태에 묶여 있기 때문입니다.

이것은 문서화된 동작이고 정해진 해법이 있습니다. MSDN의 `TrackPopupMenu` 항목에 적힌 그대로입니다.

> To display a context menu for a notification icon, the current window must be the foreground window before the application calls TrackPopupMenu. Otherwise, the menu will not disappear when the user clicks outside of the menu. ... the application must call PostMessage(hwnd, WM_NULL, 0, 0) after TrackPopupMenu returns.

---

## 2. 수정

`MenuBar::ShowContextMenu`와 `MenuBar::ShowTrayIconMenu` **두 곳 모두** 고칩니다. 두 함수가 같은 문제를 안고 있습니다.

`TrackPopupMenuEx` 호출 직전에 `SetForegroundWindow(hwnd_)`를 부르고, 호출이 돌아온 뒤에 `PostMessageW(hwnd_, WM_NULL, 0, 0)`를 부르십시오.

```cpp
  SetForegroundWindow(hwnd_);
  TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, screen.x, screen.y, hwnd_, nullptr);
  PostMessageW(hwnd_, WM_NULL, 0, 0);
  DestroyMenu(menu);
```

두 줄이 다입니다. 다른 구조를 만들지 마십시오.

### 2-1. 포그라운드가 되어도 상단바가 활성창처럼 보이지 않아야 한다

`WS_EX_NOACTIVATE` 창에 `SetForegroundWindow`를 부르면 대개 실패하거나, 성공해도 활성 모양으로 바뀌지 않습니다. 그것이 우리가 바라는 결과입니다. 반환값을 확인하지 말고, 실패해도 그대로 진행하십시오.

다만 다음 두 가지를 실제로 확인하십시오.

- 메뉴를 열었다가 닫은 뒤에 **직전에 쓰던 창이 다시 활성 상태로 돌아오는가.** 돌아오지 않고 바탕화면이 활성이 되면 사용자가 타이핑하던 자리를 잃습니다.
- 상단바의 배경이나 시계 색이 활성/비활성으로 깜빡이지 않는가.

돌아오지 않는다면, `SetForegroundWindow` 앞에서 `GetForegroundWindow()`를 기억해 두었다가 `TrackPopupMenuEx`가 돌아온 뒤에 되돌리십시오. **이 되돌리기는 실제로 문제가 관측되었을 때만 넣으십시오.**

### 2-2. 상태 패널 팝업은 이 지시서의 범위가 아니다

상태 항목을 왼쪽 클릭했을 때 뜨는 `PopupSurface`는 별개의 창이고 이미 자체적으로 닫힘을 처리합니다. 건드리지 마십시오. 그쪽에서 같은 증상이 보이면 별도로 보고하십시오.

---

## 3. 하지 말아야 할 것

- 상단바 창에서 `WS_EX_NOACTIVATE`를 떼지 마십시오. 상단바가 클릭으로 활성화되면 작업 중인 창의 포커스를 빼앗습니다.
- 메뉴를 닫으려고 전역 마우스 훅이나 타이머를 걸지 마십시오. 두 줄로 해결되는 문제입니다.
- `TPM_` 플래그를 바꾸지 마십시오. 메뉴가 뜨는 위치와는 무관한 문제입니다.
- 레지스트리에 쓰지 마십시오.

---

## 4. 검증

1. Release 빌드가 경고 없이 통과합니다.
2. 상단바 빈 곳을 우클릭해 메뉴를 엽니다.
3. **바탕화면**을 클릭합니다. 메뉴가 즉시 사라집니다.
4. 메뉴를 다시 열고 **다른 앱의 창**을 클릭합니다. 메뉴가 사라지고 그 창이 활성화됩니다.
5. 메뉴를 다시 열고 `Esc`를 누릅니다. 메뉴가 사라집니다.
6. 메뉴를 열어 아무 항목이나 고릅니다. 항목이 평소대로 동작합니다.
7. 메모장 같은 창에 커서를 두고 글자를 입력하다가, 상단바를 우클릭했다가 바깥을 눌러 메뉴를 닫습니다. 커서가 메모장으로 돌아가 계속 입력됩니다.
8. 미러된 트레이 아이콘을 우클릭해 뜬 메뉴에도 3~5번을 그대로 확인합니다.

---

## 5. 커밋

```
fix: 상단바 메뉴가 바깥을 눌러도 닫히게 한다
```

---

## 6. 검증 기록

시각: 2026-09-01. Release `build\Release\bamti.exe`.

`SetForegroundWindow`는 성공했고, 메뉴가 열린 동안 포그라운드는 `bamti.MenuBar`였다. Esc로 닫으면 상단바가 포그라운드로 남아 메모장 입력이 끊겼으므로, `TrackPopupMenuEx`가 돌아온 뒤 포그라운드가 여전히 상단바라면 직전 창을 되돌린다. 다른 창을 눌러 닫으면 그 창이 이미 포그라운드이므로 되돌리지 않는다.