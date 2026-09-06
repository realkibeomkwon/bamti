# 작업 지시서: Win+X 메뉴에서 긴 항목이 잘리지 않게 한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/menu_bar.cpp`, `src/menu_style.cpp`입니다.

**`TASK-START-MENU-UNIFY.md`보다 먼저 하십시오.** 둘 다 `src/menu_bar.cpp`를 건드리는데 이쪽이 훨씬 작습니다.

---

## 1. 무엇을 바꾸는가

상단바 왼쪽 끝 Windows 아이콘을 **오른쪽으로 눌러** 뜨는 Win+X 메뉴에서 `Windows 모바일 센터` 항목의 글자가 잘립니다.

이 메뉴의 폭을 넓히십시오. **가장 긴 항목이 온전히 보일 만큼만입니다.** 메뉴 전체를 넓적하게 만들라는 뜻이 아닙니다.

---

## 2. 원인 후보가 두 가지입니다

읽어서 확인한 사실을 먼저 적습니다.

**`src/menu_style.cpp`의 `MeasureMenuRows`가 폭을 이렇게 정합니다.**

```cpp
int width = text_w + m.pad * 2 + m.check_w + m.arrow_w;
width = (std::max)(width, DipToPx(kMenuMinWidthDip, dpi));   // 160
width = (std::min)(width, DipToPx(max_width_dip, dpi));      // 기본 280
```

`text_w`는 항목 가운데 가장 넓은 글자의 폭입니다.

**`RenderMenuRows`가 글자를 그릴 때 쓰는 폭은 정확히 `text_w`입니다.** 행 상자가 `{pad, y, width - pad, ...}`이고 거기서 다시 왼쪽 `check_w`와 오른쪽 `arrow_w`를 빼기 때문에, 남는 폭이 `width - pad*2 - check_w - arrow_w`, 즉 `text_w`가 됩니다.

**`DrawPopupText`는 폭이 모자라면 글자를 잘라 생략 부호를 붙입니다.**

```cpp
DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
factory->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf());
```

여기서 후보가 갈립니다.

### 후보 A: 재는 폭과 그리는 폭이 딱 붙어 있다

`MeasureMenuRows`는 잰 값에 `+0.5f`를 더해 정수로 끊고, 그리기는 그 정수 폭으로 다시 레이아웃을 만듭니다. **여유가 한 픽셀도 없습니다.** 부동소수 반올림이 불리한 쪽으로 떨어지면 마지막 글자가 생략 부호로 바뀝니다.

이 후보가 유력한 이유가 있습니다. **다른 메뉴에서는 이 문제가 보이지 않습니다.** `표시 항목`, `트레이 아이콘` 같은 짧은 항목만 있는 메뉴는 계산 폭이 최소 폭 160 DIP에 못 미쳐 하한이 적용되고, 그래서 글자 자리에 여유가 남습니다. Win+X 메뉴는 항목이 길어 계산 폭이 그대로 쓰이는 **거의 유일한 메뉴**입니다.

### 후보 B: 상한 280 DIP에 걸린다

`MenuBar::ShowStartContextMenu`(2169행 부근)는 `bar_menu_->Reset(...)`을 부르는데, `BarMenuContent::Reset`이 `max_width_dip_`을 기본값 280으로 되돌립니다. 그 뒤에 `SetMaxWidthDip`을 부르지 않으므로 이 메뉴의 상한은 280 DIP입니다.

참고로 트레이 서브메뉴는 `bar_submenu_->SetMaxWidthDip(360)`으로 이미 올려 두었습니다(2109행).

---

## 3. 먼저 측정해서 갈라내십시오

`MeasureMenuRows`가 정한 값을 한 번 남기십시오. 판정이 끝나면 이 로그는 걷어냅니다.

```cpp
Log(L"menu", L"measure rows=%d text_w=%d width=%d min=%d max=%d dpi=%u",
    static_cast<int>(rows.size()), text_w, width,
    DipToPx(kMenuMinWidthDip, dpi), DipToPx(max_width_dip, dpi), dpi);
```

Win+X 메뉴를 한 번 열고 `~/.bamti/bamti.log`의 `[menu] measure` 줄을 보십시오.

- `width`가 `max`와 같으면 → **후보 B가 맞습니다.**
- `width`가 `max`보다 작은데도 글자가 잘리면 → **후보 A가 맞습니다.**

**두 값과 판정 결과를 보고하십시오.** 어느 쪽이든 4절과 5절을 모두 적용하면 되지만, 무엇이 실제 원인이었는지는 기록에 남아야 합니다.

---

## 4. 재는 폭에 여유를 준다 (후보 A 대응)

`src/menu_style.cpp`에서 글자 폭을 올림으로 끊고 작은 여유를 더하십시오.

```cpp
// 재는 폭과 그리는 폭이 정확히 같으면 반올림이 불리하게 떨어질 때
// 마지막 글자가 생략 부호로 바뀐다. 여유 2 DIP 를 준다.
constexpr int kMenuTextSlackDip = 2;
```

`text_w`를 구할 때 `+ 0.5f` 후 잘라내는 대신 `std::ceil`을 쓰고, 최종 폭에 `DipToPx(kMenuTextSlackDip, dpi)`를 더하십시오.

**이 변경은 독 우클릭 메뉴를 포함한 모든 메뉴에 적용됩니다.** 짧은 항목만 있는 메뉴는 최소 폭 160 DIP에 걸려 있으므로 폭이 변하지 않고, 긴 항목이 있는 메뉴만 2 DIP 넓어집니다. 눈에 띄지 않아야 정상입니다. 독 메뉴 폭이 눈에 띄게 변하면 보고하십시오.

**여유를 4 DIP 이상으로 키우지 마십시오.** 목적은 반올림 경계를 넘기는 것이지 여백을 늘리는 것이 아닙니다.

---

## 5. 상한을 올린다 (후보 B 대응)

`MenuBar::ShowStartContextMenu`에서 `bar_menu_->Reset(hwnd_, dark_)` 다음 줄에 한 줄을 더하십시오.

```cpp
bar_menu_->SetMaxWidthDip(360);  // Win+X 항목 이름이 길다. 트레이 서브메뉴와 같은 값이다.
```

`kMenuMaxWidthDip` 기본값 280은 **그대로 두십시오.** 다른 메뉴의 상한까지 함께 올릴 이유가 없습니다.

360 DIP는 상한일 뿐이고 실제 폭은 여전히 가장 긴 항목에 맞춰집니다. 그러므로 "잘리지 않을 만큼만 넓힌다"는 요구가 지켜집니다.

---

## 6. 왜 이 이름이 이런가 (참고)

`Windows 모바일 센터`라는 이름은 이 컴퓨터의 `%LOCALAPPDATA%\Microsoft\Windows\WinX\Group*\desktop.ini`에서 옵니다. **이 파일이 낡아서 Windows 화면에 나오는 이름(`모바일 센터`)과 다릅니다.** `TASK-BAR-WINX-MENU.md`와 `FIX-WINX-LABELS.md`에서 확인한 사실이고, 운영 체제에서 새 이름을 가져올 경로가 없어 그대로 두기로 했습니다.

**이 지시서에서 이름을 손대지 마십시오.** 폭만 고칩니다.

---

## 7. 검증

1. Release 빌드가 경고 없이 통과해야 합니다.
2. 3절의 로그로 원인을 판정하고 보고한 뒤, **그 로그를 걷어내고 다시 빌드하십시오.** 메뉴를 열 때마다 남는 줄이라 그대로 두면 로그가 지저분해집니다.
3. 화면 확인은 사용자에게 부탁하십시오. **직접 스크린샷을 찍거나 입력을 합성하지 마십시오.** 확인 항목입니다.
   - Win+X 메뉴에서 `Windows 모바일 센터`가 생략 부호 없이 온전히 보이는가.
   - 다른 항목의 오른쪽 여백이 눈에 띄게 벌어지지 않았는가.
   - 독 우클릭 메뉴와 상단바 우클릭 메뉴의 폭이 예전과 같아 보이는가.
