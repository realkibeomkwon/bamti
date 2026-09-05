# 작업 지시서 6: UI 스레드가 어디서 멈추는지 찾는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

지금까지 다섯 차례에 걸쳐 증상을 하나씩 제거했습니다. 그 결과 **원인이 하나로 좁혀졌습니다.** 이 문서는 그것을 특정하기 위한 계측과, 함께 확인된 결함 두 가지의 수정을 지정합니다.

---

## 0. 이번 로그로 확정된 사실

### 0.1 가드 타이머는 정상이다

```
10:29:38.796 [popup] arm guard id=1 err=0
10:29:38.871 [popup] alive tick=1 src=timer armed=1 hot=-1 inside=0
10:29:38.921 [popup] alive tick=2 src=timer armed=1 hot=-1 inside=0
10:29:38.996 [popup] alive tick=3 src=timer armed=1 hot=-1 inside=0
```

`SetTimer`는 성공했고(`id=1 err=0`), 틱은 `src=timer`로 50~75ms 간격에 정상 발생합니다. **앞선 지시서에서 "가드 타이머가 죽었다"고 판단했던 것은 틀렸습니다.** 그때는 20틱마다만 기록했기 때문에, 1초 안에 멈추면 아무 줄도 남지 않았을 뿐입니다.

1.4의 소유자 이중화는 그대로 두십시오. 안전망으로 유효합니다.

### 0.2 첫째와 둘째 메뉴는 정상 동작한다

```
10:29:38.796 [popup] open rows=7 shown 16ms   → dismiss outside-poll (39.431)
10:29:42.123 [popup] open rows=7 shown 0ms    → dismiss outside-click (43.259)
```

표시도 닫힘도 됩니다.

### 0.3 셋째 메뉴에서 UI 스레드가 멈춘다

```
10:29:43.360 [dock]  menu open index=0 name=explorer.exe
10:29:43.366 [popup] arm guard id=1 err=0
10:29:43.366 [popup] open rows=7 shown 0ms
10:29:43.368 [popup] msg=mousemove pt=12,258 inside=0
          ← 여기서 로그가 완전히 끊긴다
```

`alive tick=1`이 약 60ms 뒤인 43.43에 나와야 하는데 없습니다. 사용자는 이 상태에서 Spotlight에 입력이 되지 않는다고 보고했습니다.

**즉 10:29:43.368 직후에 UI 스레드가 멈췄고, 그 뒤로는 아무것도 처리하지 못합니다.** 메뉴가 안 닫히는 것도, Spotlight가 안 되는 것도 전부 이 하나의 결과입니다.

**이제 남은 문제는 이것 하나입니다.** 다른 증상을 쫓지 마십시오.

---

## 1. 무엇을 해야 하는가

멈추는 지점을 **추측하지 말고 측정해야 합니다.** 지금까지 여러 차례 정적 분석으로 원인을 지목했고 그때마다 빗나갔습니다. 이번에는 실행 중에 지점을 스스로 말하게 만드십시오.

### 1-1. UI 스레드 감시 스레드를 만든다 (필수, 최우선)

**새 파일** `src/watchdog.hpp`와 `src/watchdog.cpp`를 만드십시오. `CMakeLists.txt`와 `bamti.vcxproj` 양쪽에 등록해야 합니다.

```cpp
namespace bamti {

// UI 스레드가 지금 무엇을 하고 있는지 나타낸다.
// 문자열 리터럴만 넘긴다. 할당하지 않으므로 어느 지점에서 불러도 안전하다.
void WatchdogStage(const wchar_t* stage);

// 감시 스레드를 시작하고 멈춘다. hwnd는 UI 스레드가 소유한 창이다.
bool WatchdogStart(HWND ui_window);
void WatchdogStop();

}  // namespace bamti
```

구현 지침입니다.

- 현재 단계는 `std::atomic<const wchar_t*>`에 보관합니다. 리터럴 포인터만 저장하므로 할당도 잠금도 없습니다.
- 감시 스레드는 500ms마다 다음을 수행합니다.

  ```cpp
  DWORD_PTR result = 0;
  const LRESULT ok = SendMessageTimeoutW(ui_window, WM_NULL, 0, 0,
                                         SMTO_NORMAL, 1000, &result);
  ```
- `ok`가 0이고 `GetLastError()`가 `ERROR_TIMEOUT`이면 UI 스레드가 응답하지 않는 것입니다. 이때 한 번만 기록합니다.

  ```cpp
  Log(L"watchdog", L"UI STUCK stage=%s", CurrentStage());
  ```
- 이후 응답이 돌아오면 회복을 한 번 기록합니다.

  ```cpp
  Log(L"watchdog", L"UI recovered after %ums stage=%s", ms, CurrentStage());
  ```
- 정지 상태가 이어지는 동안 매 주기 기록하지 마십시오. 로그가 넘칩니다. 처음 한 번, 그리고 **5초마다 한 번**만 남기십시오.
- `SMTO_ABORTIFHUNG`을 쓰지 마십시오. 그 플래그는 이미 멈춘 창에 대해 즉시 반환하므로, 우리가 알고 싶은 상태를 감지하지 못합니다.
- 스레드는 `WatchdogStop()`에서 확실히 종료해야 합니다. 이벤트 핸들이나 `std::atomic<bool>`로 종료를 알리고 `join`하십시오.

`Run()`에서 메뉴 바 생성 직후 `WatchdogStart(bar.hwnd())`를 부르고, 메시지 루프가 끝난 뒤 `WatchdogStop()`을 부르십시오.

### 1-2. 단계 표시를 심는다 (필수)

`WatchdogStage()`를 다음 지점에 넣으십시오. **문자열 리터럴만** 넘기십시오.

| 위치 | 단계 문자열 |
|---|---|
| 메시지 루프에서 `GetMessage` 직전 | `L"idle"` |
| `Dock::HandleMessage` 진입 | `L"dock.msg"` |
| `Dock::Rebuild` 진입 | `L"dock.rebuild"` |
| `Dock::OpenDockMenu` 진입 | `L"dock.menu"` |
| `Dock::RenderLayered` 진입 | `L"dock.render"` |
| `PopupSurface::Open` 진입 | `L"popup.open"` |
| `PopupSurface::Render` 진입 | `L"popup.render"` |
| `PopupSurface::EnsureRenderTarget` 진입 | `L"popup.target"` |
| `PopupSurface::Tick` 진입 | `L"popup.tick"` |
| `PopupSurface::Dismiss` 진입 | `L"popup.dismiss"` |
| `Spotlight::ApplyFilter` 진입 | `L"spot.filter"` |
| Spotlight 그리기 함수 진입 | `L"spot.paint"` |
| `MenuBar::Paint` 진입 | `L"bar.paint"` |
| `IsTrueFullscreen` 진입 | `L"fullscreen"` |
| `CollectDockApps` 진입 | `L"collect"` |

각 함수를 나갈 때 `L"idle"`로 되돌리지 마십시오. **마지막으로 들어간 지점이 남아 있어야** 어디서 멈췄는지 알 수 있습니다. 메시지 루프가 다음 메시지를 기다릴 때만 `L"idle"`로 바꿉니다.

이 한 줄이 다음 재현에서 답을 줍니다.

```
[watchdog] UI STUCK stage=popup.render
```

### 1-3. 감시 스레드가 로그 잠금에 막히지 않게 한다 (필수)

`Log()`는 뮤텍스를 씁니다. UI 스레드가 `Log()` 안에서 멈추면 감시 스레드도 함께 막혀 기록을 남기지 못합니다.

`log.cpp`에 잠금을 시도만 하는 경로를 추가하십시오.

```cpp
// 잠금을 얻지 못하면 기록을 포기하고 즉시 돌아온다. 감시 스레드 전용이다.
void LogTry(const wchar_t* tag, const wchar_t* fmt, ...);
```

`std::mutex` 대신 `try_lock()`을 쓰고, 실패하면 아무것도 하지 않고 반환합니다. 감시 스레드는 이 함수만 사용하십시오.

---

## 2. 함께 확인된 결함

### 2-1. 빠른 클릭을 폴링이 놓친다 (필수)

`Tick()`은 `GetAsyncKeyState(...) & 0x8000`으로 **지금 눌려 있는지**만 봅니다. 틱 간격이 50~75ms이므로, 그보다 빨리 눌렀다 떼면 두 틱 모두 "떼어짐"으로 보여 **누름 자체가 관측되지 않습니다.**

사용자가 "바깥쪽 클릭해서 메뉴 닫을 때도 오래 걸림"이라고 한 것이 이 현상입니다. 클릭이 틱 경계에 걸릴 때까지 여러 번 눌러야 합니다.

`GetAsyncKeyState`의 **최하위 비트(0x0001)** 는 "직전 호출 이후에 눌린 적이 있는가"를 알려 줍니다. 이것을 함께 보십시오.

```cpp
const SHORT s = GetAsyncKeyState(vk);
const bool down = (s & 0x8000) != 0;      // 지금 눌려 있다
const bool pressed_since = (s & 0x0001) != 0;  // 지난 호출 이후 눌린 적이 있다
```

- 바깥 클릭 판정에서 `down`뿐 아니라 `pressed_since`도 누름으로 취급하십시오.
- 최하위 비트는 읽으면 지워지므로, **틱마다 각 버튼에 대해 정확히 한 번만** 호출하십시오. 여러 번 부르면 다른 검사가 이벤트를 잃습니다.
- 이 비트는 프로세스 전체에서 공유됩니다. `Spotlight`나 다른 코드가 같은 가상 키에 `GetAsyncKeyState`를 부르고 있지 않은지 확인하고, 있으면 그 지점을 보고하십시오.

`ESC`와 `Win` 키 판정에도 같은 개선을 적용하십시오. 같은 이유로 놓칠 수 있습니다.

### 2-2. 우클릭에서 메뉴가 뜨기까지의 지연을 계측한다 (필수)

사용자는 첫째와 둘째 메뉴가 뜨는 데 1초 정도 걸린다고 했습니다. 그런데 로그의 `shown` 값은 16ms와 0ms입니다. **지연은 `PopupSurface::Open()` 바깥에 있습니다.**

`Dock::HandleMessage`의 `WM_RBUTTONDOWN`과 `WM_RBUTTONUP`에서 각각 시각을 기록하십시오.

```cpp
Log(L"dock", L"rbutton down");
Log(L"dock", L"rbutton up %ums since down", /* 경과 */);
```

`WM_RBUTTONDOWN` 처리가 없다면 로그만 남기는 분기를 추가하십시오. 동작은 바꾸지 마십시오.

이 두 줄과 기존 `menu open` 줄의 시각을 비교하면, 지연이 다음 셋 중 어디인지 알 수 있습니다.

- 버튼을 누르고 떼기까지 (사용자 조작)
- 떼고 나서 `menu open`까지 (독의 처리)
- `menu open` 이후 (이미 0~16ms로 확인됨)

---

## 3. 하지 말아야 할 것

- 이번에는 **멈춤의 원인을 추측해서 고치지 마십시오.** 1장의 계측을 넣고, 그 결과를 보고하는 것이 이번 작업의 목표입니다. 원인이 확정되기 전에 손대면 또 빗나갑니다.
- 지금까지의 수정을 하나도 되돌리지 마십시오. `armed_`, 4dip 간격, `UpdateWindow`, 폴링 입력, 지문 생략, 소유자 이중화, Spotlight 아이콘 분리 모두 유지합니다.
- `WatchdogStage()`에 동적 문자열을 넘기지 마십시오. `std::wstring`이나 `wsprintf` 결과를 넘기면 안 됩니다. 리터럴만 넘깁니다.
- 감시 스레드에서 `Log()`를 쓰지 마십시오. 1-3의 `LogTry()`만 씁니다.
- 감시 스레드에서 창을 그리거나 `SendMessage`로 실제 작업을 시키지 마십시오. `WM_NULL` 확인만 합니다.
- 2-1에서 `GetAsyncKeyState`를 같은 키에 대해 한 틱에 두 번 이상 부르지 마십시오.
- `WH_MOUSE_LL`, `WH_KEYBOARD_LL`을 새로 도입하지 마십시오.
- `SetForegroundWindow`, `AttachThreadInput`을 도입하지 마십시오.

---

## 4. 검증

**감시 (핵심)**
- [ ] 정상 동작 중에는 `[watchdog] UI STUCK` 줄이 남지 않는다.
- [ ] 메뉴를 반복해서 열어 멈춤을 재현한 뒤, `[watchdog] UI STUCK stage=...` 줄이 남는다.
- [ ] **그 `stage=` 값을 보고한다. 이것이 이번 작업의 성과물이다.**
- [ ] 멈춤에서 회복되면 `UI recovered` 줄이 남는다.

**클릭 판정**
- [ ] 메뉴를 열고 바깥을 **빠르게 한 번** 클릭하면 즉시 닫힌다. 여러 번 누를 필요가 없다.
- [ ] `ESC` 키를 짧게 눌러도 닫힌다.
- [ ] 버튼을 3초간 누르고 있다가 떼어도 메뉴가 남아 있다.

**지연 계측**
- [ ] `[dock] rbutton down`과 `rbutton up ... since down`, `menu open`의 시각 차이를 보고한다.

**회귀**
- [ ] 우클릭 한 번에 `[dock] menu open`이 한 줄만 남는다.
- [ ] `[popup] alive tick=1 src=timer`가 계속 나온다.
- [ ] 유휴 60초 누적 CPU가 3.8초보다 나빠지지 않는다. 감시 스레드는 500ms마다 한 번만 깨어나므로 영향이 없어야 한다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 5. 커밋

두 개로 나누십시오.

```
chore: UI 스레드 감시 스레드와 단계 표시를 추가한다
```
1장입니다.

```
fix: 폴링이 빠른 클릭을 놓치지 않게 한다
```
2장입니다. 본문에 2-2의 시각 차이 측정 결과를 적으십시오.
