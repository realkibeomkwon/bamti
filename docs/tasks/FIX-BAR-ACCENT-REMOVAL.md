# 작업 지시서: 상단바에서 accent policy 를 걷어내고 배경 농도를 0.5 로 낮춘다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`TASK-BAR-LAYERED-WINDOW.md`(커밋 `cd5f888`)의 후속입니다. **그 지시서 3-4절의 "accent policy 는 남겨 두십시오"가 틀렸으므로 그 지시를 취소합니다.**

건드리는 파일은 `src/menu_bar.cpp`, `src/theme.cpp` 두 개입니다.

---

## 1. 무엇이 원인이었는가 (측정으로 확정)

레이어드 창 전환은 성공했습니다. `bamti.MenuBar` 의 ExStyle 이 `0x08080088` 로 바뀌어 `WS_EX_LAYERED` 가 켜졌고, 알파 합성도 정상으로 동작합니다.

**그런데도 상단바 픽셀이 `(24,24,24)` 로 완전히 균일했습니다.** 벽지에 그라데이션이 있는데도 x 좌표를 옮겨 가며 재도 값이 조금도 달라지지 않았습니다.

코드를 고치지 않고 실행 중인 창에 accent 설정만 바꿔 가며 상단바 픽셀을 실측했습니다.

| 설정 | x=600 | x=1400 | x=2200 | x=3000 |
| --- | --- | --- | --- | --- |
| accent 끔 (state 0) | 48,48,49 | 50,50,50 | 54,54,55 | 55,54,55 |
| 블러 (state 3) + 틴트 알파 0x01 | 24,24,24 | 24,24,24 | 24,24,24 | 24,24,24 |
| 아크릴 (state 4) + 틴트 알파 0x01 | 24,24,24 | 24,24,24 | 24,24,24 | 24,24,24 |
| accent 끔 (state 0, 원복) | 48,48,49 | 50,50,50 | 54,54,55 | 55,54,55 |

같은 시각 그 네 지점의 벽지 원본값은 각각 109, 125, 134, 133 이었습니다.

**accent 를 끈 값은 `24 + 벽지값 × 0.22` 와 정확히 일치합니다.**

```
24 + 109 × 0.22 = 48   실측 48
24 + 125 × 0.22 = 51   실측 50
24 + 134 × 0.22 = 53   실측 54
24 + 133 × 0.22 = 53   실측 55
```

여기서 24 는 `BarFillColor` 다크 `(0.12, 0.12, 0.12, 0.78)` 을 미리 곱한 값이고, 0.22 는 남은 투과율입니다. **레이어드 합성은 처음부터 계산대로 동작하고 있었습니다.**

### 결론

`SetWindowCompositionAttribute` 의 accent policy 는 **창 뒤를 불투명하게 채웁니다.** 그 위에 우리 반투명 배경이 얹히므로, 뒤가 검정인 것과 같은 결과가 나옵니다. 미리 곱해진 값 `(24,24,24)` 가 그대로 화면에 남은 이유가 이것입니다.

**틴트 알파를 0x99 에서 0x01 까지 낮춰도 결과가 같습니다.** 아크릴이든 예전 방식 블러든 마찬가지입니다. **accent policy 는 레이어드 창과 함께 쓸 수 없으므로 완전히 제거해야 합니다.**

---

## 2. accent policy 관련 코드를 전부 지운다

`src/menu_bar.cpp` 에서 다음을 모두 삭제하십시오.

1. `MenuBar::ApplyBackdrop` 마지막 줄의 `ApplyAccentPolicy(hwnd_, dark_);` 호출
2. 익명 이름공간의 `AccentState` 열거형 (83행 부근)
3. `kBarAccentState` 상수와 그 위의 주석 (93행 부근)
4. `AccentPolicy`, `WindowCompositionAttributeData` 구조체
5. `SetWindowCompositionAttributeFn` 별칭
6. `LoadSetWindowCompositionAttribute()` 함수
7. `ApplyAccentPolicy()` 함수

**일곱 개를 모두 지워야 합니다.** 호출만 지우고 정의를 남기면 익명 이름공간에서 쓰이지 않는 함수가 되어 `C4505` 경고가 납니다. Release 빌드는 경고 없이 통과해야 합니다.

`ApplyBackdrop` 에 남는 것은 다음 네 가지입니다. **이것들은 그대로 두십시오.**

```cpp
void MenuBar::ApplyBackdrop() {
  if (hwnd_ == nullptr) {
    return;
  }

  const BOOL dark = dark_ ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));

  const int backdrop = dwm::kBackdropNone;
  DwmSetWindowAttribute(hwnd_, dwm::kSystemBackdropType, &backdrop, sizeof(backdrop));

  const int corner = dwm::kCornerDoNotRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));

  const MARGINS margins{0, 0, 0, 0};
  DwmExtendFrameIntoClientArea(hwnd_, &margins);
}
```

`[bar] composition` 로그 줄도 함께 사라집니다. 정상입니다.

---

## 3. 배경 농도를 0.5 로 낮춘다

지금 상단바는 벽지를 22 퍼센트만 통과시킵니다. 맥 메뉴바는 벽지가 그보다 훨씬 많이 남아 보이므로 알파를 낮춥니다.

`src/theme.cpp` 의 `BarFillColor` 를 다음과 같이 바꾸십시오.

```cpp
D2D1_COLOR_F BarFillColor(bool dark) {
  // 맥 메뉴바처럼 벽지가 절반쯤 비치게 한다. 독(DockFillColor)보다 옅다.
  if (dark) {
    return D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.50f);
  }
  return D2D1::ColorF(0.98f, 0.98f, 0.98f, 0.54f);
}
```

바꾸는 것은 **알파 두 개뿐입니다.** 색상값 `0.12` 와 `0.98` 은 그대로 두십시오.

라이트 테마를 0.54 로 두는 것은 지금의 0.78 대 0.82 관계를 유지한 것입니다. 라이트 테마는 어두운 글자를 밝은 배경 위에 얹으므로 조금 더 불투명해야 글자가 읽힙니다.

**`DockFillColor` 는 절대 건드리지 마십시오.** 독과 메뉴의 농도는 지금 그대로 둡니다.

---

## 4. 검증

### 4-1. 빌드

Release 빌드가 **경고 없이** 통과해야 합니다. **CMake 로 빌드하십시오.** `bamti.vcxproj` 는 소스 열네 개가 빠져 있어 msbuild 로는 링크되지 않습니다.

```
cmake -S D:\repos\bamti -B <빌드 디렉터리>
cmake --build <빌드 디렉터리> --config Release
```

### 4-2. 화면 픽셀 (이것이 성공 판정입니다)

**반환값이나 로그로 판정하지 마십시오.** 앞선 세 번의 실패가 모두 그 지점에서 났습니다.

상단바 안쪽에서 x 좌표를 넓게 벌려 네 지점 이상의 RGB 를 재고, 같은 x 의 상단바 아래쪽에서 벽지 원본값도 함께 재십시오. `System.Drawing` 의 `Graphics.CopyFromScreen` 뒤에 `GetPixel` 이면 됩니다.

**판정 기준은 두 가지입니다.**

1. **값이 x 마다 달라져야 합니다.** 값이 균일하면 뒤가 여전히 막혀 있다는 뜻이므로 실패입니다.
2. **다크 테마에서 `15 + 벽지값 × 0.50` 에 가까워야 합니다.** 오차 두세 단위는 벽지 압축과 반올림 때문이므로 정상입니다.

벽지값이 109 에서 134 사이인 지금 화면이라면 상단바가 대략 70 에서 82 사이로 나와야 합니다. 지금은 48 에서 55 입니다.

### 4-3. 마우스 입력

알파 0.50 은 128 에 해당하므로 히트 테스트에는 문제가 없어야 합니다. 그래도 한 번 확인하십시오.

- 시작 단추, 트레이 아이콘, 시계, 제어 센터를 클릭할 수 있는가.
- 호버 강조가 뜨는가.

### 4-4. 사람 눈으로 봐야 하는 것

**직접 스크린샷을 찍거나 입력을 합성하지 말고 사용자에게 부탁하십시오.**

- 벽지가 비치는 농도가 적당한가. 진하거나 옅으면 알파를 조정한다.
- **다크 테마에서 시계 글자와 트레이 아이콘이 읽히는가.** 알파를 0.78 에서 0.50 으로 낮췄으므로 밝은 벽지 위에서 대비가 떨어집니다. 흰 벽지 기준으로 계산하면 명암비가 대략 3 대 1 까지 내려가는데, 본문 글자의 권장값 4.5 대 1 에 못 미칩니다. **읽기 어렵다는 답을 받으면 알파를 0.60 으로 올려 다시 재십시오.**
- 라이트 테마에서도 같은 것을 확인한다.
- 위쪽 모서리가 각져 있는가.

---

## 5. 건드리지 말 것

- `DockFillColor`, `DockStrokeColor` 를 비롯한 독과 메뉴의 색과 알파를 바꾸지 마십시오.
- `dwm::kCornerDoNotRound` 를 바꾸지 마십시오.
- 남아 있는 `DwmSetWindowAttribute` 호출 세 개를 지우지 마십시오. `kUseImmersiveDarkMode` 는 여전히 필요합니다.
- `clock_renderer.cpp` 의 그리기 로직, 아이콘, 글자색을 바꾸지 마십시오. `Clear(BarFillColor(dark))` 는 그대로 둡니다.
- 레이어드 창 전환(`WS_EX_LAYERED`, `UpdateLayeredWindow`)을 되돌리지 마십시오. **그 부분은 정상으로 동작하고 있습니다.**

---

## 6. 참고: 블러를 포기한다

맥 메뉴바에는 뒤를 흐리게 만드는 블러가 걸려 있지만, 이 변경 뒤의 상단바에는 블러가 없어 벽지가 선명하게 비칩니다.

**지금 시점에서 블러를 되살릴 방법은 없습니다.** 레이어드 창에는 DWM 시스템 백드롭도 accent policy 도 붙지 않는다는 것을 세 번의 측정으로 확인했습니다. 블러가 필요해지면 상단바를 `DirectComposition` 이나 WinUI 컴포지션 표면으로 다시 짓는 별개의 작업이 되므로, 이 지시서에서는 다루지 않습니다.
