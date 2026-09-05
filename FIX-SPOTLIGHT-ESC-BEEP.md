# 작업 지시서 21: Spotlight를 ESC로 닫을 때 경고음이 난다

한 줄로 끝나는 수정이지만, 원인이 눈에 잘 띄지 않는 자리에 있어 설명을 남깁니다.

---

## 1. 원인

`src/spotlight.cpp`의 `EditProc`(1264행)은 `WM_KEYDOWN`에서 ESC를 처리하고 메시지를 삼킵니다.

```cpp
  if (msg == WM_KEYDOWN) {
    if (wparam == VK_ESCAPE) {
      self->Hide();
      return 0;
    }
```

여기까지는 맞습니다. 문제는 **`WM_CHAR`가 따로 온다**는 점입니다.

메시지 루프의 `TranslateMessage`는 `WM_KEYDOWN`을 보고 `WM_CHAR`를 큐에 넣습니다. 이 변환은 창 프로시저가 호출되기 **전에** 일어나므로, `WM_KEYDOWN`을 소비해도 `WM_CHAR`는 이미 큐에 들어가 있습니다.

ESC의 문자 코드는 `0x1B`입니다. 편집 컨트롤은 `WM_CHAR`로 이런 제어 문자를 받으면 입력할 수 없는 문자로 판단하고 `MessageBeep`을 울립니다. 이것이 들리는 소리의 정체입니다.

`EditProc`에는 `WM_CHAR`를 거르는 자리가 하나 있지만 IME 커밋만 다룹니다.

```cpp
  if (msg == WM_CHAR && self->swallow_ime_commit_) {
    self->swallow_ime_commit_ = false;
    if (wparam > 0x7F) {      // ESC(0x1B)는 이 조건에 걸리지 않는다
      return 0;
    }
  }
```

ESC는 `0x7F` 이하라 그대로 통과해 편집 컨트롤에 도달합니다.

### Enter도 같은 문제를 갖고 있다

```cpp
    if (wparam == VK_RETURN) {
      self->ActivateHot();
      return 0;
    }
```

Enter의 문자 코드 `0x0D`도 편집 컨트롤에서 같은 경고음을 냅니다. 원인과 해법이 동일하므로 함께 고칩니다.

### 덤으로 생기는 낭비

`ShouldApplyFilter`가 `WM_CHAR`를 참으로 판정하므로, 창을 이미 숨긴 뒤에 `ApplyFilter()`가 한 번 더 돕니다. `WM_CHAR`를 삼키면 이 호출도 사라집니다.

---

## 2. 수정 내용

`EditProc`의 `swallow_ime_commit_` 블록 **바로 다음**, IME 메시지를 다루는 `switch (msg)` **앞**에 넣습니다.

```cpp
  // WM_KEYDOWN에서 처리한 키라도 TranslateMessage가 만든 WM_CHAR는 따로 도착한다.
  // 편집 컨트롤은 ESC(0x1B)와 Enter(0x0D)를 입력할 수 없는 문자로 보고 MessageBeep을
  // 울리므로 여기서 삼킨다.
  if (msg == WM_CHAR && (wparam == 0x1B || wparam == 0x0D)) {
    return 0;
  }
```

**순서를 지켜 주십시오.** `swallow_ime_commit_` 블록보다 앞에 넣으면 IME 커밋 직후 ESC를 눌렀을 때 그 플래그가 내려가지 않은 채 남습니다.

---

## 3. 검증

1. Spotlight를 열고 ESC로 닫습니다. **경고음이 없어야** 합니다.
2. 앱을 검색해 Enter로 실행합니다. 여기서도 경고음이 없어야 하고, 앱은 정상으로 실행되어야 합니다.
3. 한글을 입력해 봅니다. 조합과 커밋이 그대로 동작해야 합니다. `swallow_ime_commit_` 로직을 건드리지 않았는지 보는 항목입니다.
4. 일반 문자 입력과 지우기, 방향키 이동, 붙여넣기가 그대로인지 확인합니다.

---

## 4. 주의 사항

- `WM_KEYDOWN` 쪽 처리는 그대로 두십시오. 그쪽은 정상입니다.
- Tab 키도 편집 컨트롤에서 같은 경고음을 냅니다. 다만 Spotlight는 Tab에 기능을 두지 않았으므로 이번 범위에서 제외합니다. 나중에 Tab으로 항목을 옮기는 기능을 넣을 때 함께 다루면 됩니다.
- `wparam`을 `VK_ESCAPE`나 `VK_RETURN` 상수와 비교해도 값은 같지만, `WM_CHAR`의 `wparam`은 가상 키 코드가 아니라 문자 코드입니다. 의미가 다르므로 위와 같이 `0x1B`, `0x0D`로 적고 주석을 남겨 주십시오.
