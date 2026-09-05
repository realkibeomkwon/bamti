# 작업 지시서 7: 감시 스레드가 침묵하는 문제를 고치고 멈춤 지점을 잡는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

멈춤은 재현되었으나 `[watchdog] UI STUCK` 줄이 **한 줄도 남지 않았습니다.** 감시 장치 자체에 결함이 있습니다. 이 문서는 그것을 고쳐 다음 재현에서 반드시 지점을 잡도록 합니다.

---

## 0. 이번 로그로 확정된 사실

### 0.1 메뉴 표시는 이제 진짜로 빠르다

```
10:49:30.759 [dock]  rbutton down
10:49:30.759 [dock]  rbutton up 0ms since down
10:49:30.759 [dock]  menu open index=0 name=explorer.exe
10:49:30.765 [popup] arm guard id=1 err=0
10:49:30.765 [popup] open rows=7 shown 16ms
```

버튼을 뗀 시점부터 메뉴가 그려지기까지 **6밀리초**입니다. 사용자가 느낀 "1초"는 bamti의 처리 구간이 아닙니다. 독이 나타나는 과정이나 조작 감각 쪽입니다. 이 항목은 더 쫓지 마십시오.

둘째 메뉴도 같습니다. `rbutton up 141ms since down`은 사용자가 버튼을 그만큼 누르고 있었다는 뜻이고, 뗀 직후 즉시 열렸습니다.

### 0.2 여전히 둘째 또는 셋째 메뉴에서 멈춘다

```
10:49:33.275 [popup] open rows=7 shown 0ms
10:49:33.283 [popup] msg=mousemove pt=12,258 inside=0
          ← 로그 완전 정지
```

`alive tick=1`이 약 60ms 뒤에 나와야 하는데 없습니다.

### 0.3 감시 스레드가 아무것도 남기지 못했다

배선은 정상입니다. `WatchdogStart(bar.hwnd())`가 `bar.Create()` 성공 직후에 호출되고, 감시 루프도 지시대로 구현되어 있습니다. 그런데 `[watchdog]` 줄이 하나도 없습니다.

---

## 1. 감시 스레드가 침묵한 이유

**제가 지정한 `LogTry()` 설계에 결함이 있었습니다.**

`LogTry()`는 로그 뮤텍스를 `try_lock`으로 시도하고, 실패하면 **조용히 반환**합니다.

```cpp
std::unique_lock lock(g_lock, std::try_to_lock);
if (!lock.owns_lock()) {
  return;          // 아무것도 남기지 않는다
}
```

UI 스레드가 `Log()` 안에서, 즉 뮤텍스를 쥔 채로 멈추면 어떻게 되는지 보십시오.

1. UI 스레드가 `Log()`에 들어가 `g_lock`을 잡는다.
2. 파일 쓰기에서 반환하지 않는다.
3. 감시 스레드가 정지를 감지한다.
4. 기록하려고 `LogTry()`를 부른다.
5. `try_lock`이 실패한다. **조용히 반환한다.**

**우리가 알고 싶은 바로 그 상황에서만 침묵하는 구조입니다.** 관측하려던 대상이 관측 장치를 막습니다.

이것이 유일한 설명은 아닙니다. UI 스레드가 실제로는 멈추지 않았고 `SendMessageTimeoutW`가 매번 성공했을 수도 있습니다. 보내진 메시지는 모달 루프 안에서도 응답되기 때문입니다. 두 경우를 구분해야 합니다.

---

## 2. 수정 지시

### 2-1. 감시 스레드에 독립된 출력 경로를 준다 (필수, 최우선)

감시 스레드는 **`Log()`의 뮤텍스와 파일을 절대 공유해서는 안 됩니다.**

`watchdog.cpp` 안에서 자체 출력을 구현하십시오.

- `WatchdogStart()`에서 `%USERPROFILE%\.bamti\watchdog.log`를 `CreateFileW`로 열어 핸들을 보관합니다. 공유 모드는 `FILE_SHARE_READ | FILE_SHARE_WRITE`로 두어 사용자가 읽는 동안에도 쓸 수 있게 하십시오.
- 기록은 `WriteFile`로 직접 합니다. 뮤텍스도 `FILE*`도 쓰지 마십시오. 감시 스레드 혼자만 쓰므로 잠금이 필요 없습니다.
- 매 줄마다 `FlushFileBuffers`를 부르십시오. 프로세스를 강제 종료해도 내용이 남아야 합니다.
- 같은 내용을 `OutputDebugStringW`로도 내보내십시오. 이 함수는 우리 로그 잠금과 무관합니다.
- 시각은 `GetLocalTime`으로 직접 만드십시오.

**`LogTry()`는 더 이상 감시 스레드에서 쓰지 마십시오.** 다른 곳에서 쓰이지 않으면 함께 지우십시오.

### 2-2. 정상일 때도 심장박동을 남긴다 (필수)

지금은 정지를 감지했을 때만 기록하므로, 아무 줄도 없을 때 "정지를 못 잡았다"와 "정지가 없었다"를 구분할 수 없습니다.

감시 스레드가 **5초마다 한 줄** 남기게 하십시오.

```
alive stage=<현재 단계> ping=ok
```

정지 시에는 다음처럼 남깁니다.

```
STUCK stage=<현재 단계> ping=timeout log_busy=<0|1>
```

이렇게 하면 다음 재현에서 세 가지가 즉시 갈립니다.

| 관측 | 해석 |
|---|---|
| `alive` 줄이 계속 나온다 | UI 스레드는 메시지를 처리하고 있다. 멈춘 것은 타이머나 입력 경로다 |
| `STUCK` 줄이 나온다 | UI 스레드가 정말 막혔다. `stage=` 값이 지점을 가리킨다 |
| 두 줄 다 끊긴다 | 감시 스레드 자체나 프로세스에 문제가 있다 |

### 2-3. 로그 잠금이 잡혀 있는지 보고한다 (필수)

1장의 가설을 확인할 수 있어야 합니다.

`log.cpp`에 다음을 추가하십시오.

```cpp
// Log()가 지금 뮤텍스를 쥔 채 실행 중인지 알려 준다. 감시 스레드 전용이다.
bool LogBusy();
```

구현은 원자 카운터로 하십시오. 뮤텍스를 잡기 **직전이 아니라 잡은 직후**에 올리고, 놓기 직전에 내립니다.

```cpp
std::atomic<int> g_log_depth{0};
// Log() 안에서
std::lock_guard lock(g_lock);
g_log_depth.fetch_add(1, std::memory_order_acq_rel);
// ... 기록 ...
g_log_depth.fetch_sub(1, std::memory_order_acq_rel);
```

`LogBusy()`는 이 값이 0보다 큰지 돌려줍니다. 감시 스레드는 이 함수만 부르고, 뮤텍스는 건드리지 않습니다.

`log_busy=1`과 `STUCK`이 함께 나오면 원인이 `Log()`의 파일 쓰기입니다. `log_busy=0`이면 다른 곳입니다.

### 2-4. 단계 표시를 넓힌다 (필수)

현재 단계 표시가 `dock.cpp`, `fullscreen.cpp`, `host.cpp`에만 들어가 있습니다. 멈춤이 팝업이나 Spotlight에서 일어나면 지점을 못 잡습니다.

앞선 지시서 `FIX-HANG.md` 1-2의 표에 있는 **모든 지점**에 넣으십시오. 특히 다음이 빠져 있으면 반드시 추가하십시오.

- `PopupSurface::Open`, `Render`, `EnsureRenderTarget`, `Tick`, `Dismiss`
- `Spotlight::ApplyFilter`와 그리기 함수
- `MenuBar::Paint`
- `CollectDockApps`

추가로 다음 두 지점을 넣으십시오. 이번 로그가 끊긴 자리 근처입니다.

- `PopupSurface::Handle`의 `WM_MOUSEMOVE` 분기 → `L"popup.mousemove"`
- `Log()` 진입 직후 → **넣지 마십시오.** 대신 2-3의 `LogBusy()`로 판정합니다. 단계를 덮어쓰면 원래 지점을 잃습니다.

### 2-5. 정지 시 스레드 상태를 함께 남긴다 (필수)

`stage=`만으로 부족할 때를 대비해, 정지가 처음 감지되었을 때 UI 스레드의 상태를 함께 기록하십시오.

```cpp
// UI 스레드 핸들은 WatchdogStart에서 OpenThread로 얻어 둔다.
// 정지 시 한 번만:
const DWORD susp = SuspendThread(ui_thread);   // 이전 정지 횟수를 돌려준다
CONTEXT ctx{};
ctx.ContextFlags = CONTEXT_CONTROL;
GetThreadContext(ui_thread, &ctx);
ResumeThread(ui_thread);
```

`ctx.Rip` 값을 16진수로 남기고, 모듈 기준 상대 주소도 함께 남기십시오.

```cpp
HMODULE self = GetModuleHandleW(nullptr);
const uintptr_t rva = ctx.Rip - reinterpret_cast<uintptr_t>(self);
```

```
STUCK stage=... rip=0x... rva=0x... log_busy=...
```

`rva` 값이 있으면 지도 파일이나 디스어셈블로 정확한 함수를 지목할 수 있습니다.

**주의 사항입니다.**

- `SuspendThread`와 `ResumeThread`는 **반드시 짝을 맞추십시오.** 사이에 로그나 할당을 넣지 마십시오. 정지된 스레드가 잡고 있는 잠금을 감시 스레드가 요구하면 교착합니다.
- 두 호출 사이에서는 `GetThreadContext`만 부르십시오. 기록은 `ResumeThread` **뒤에** 하십시오.
- 이 조사는 정지가 **처음 감지되었을 때 한 번만** 하십시오. 5초 주기 반복에서는 하지 마십시오.
- `OpenThread`에는 `THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT`가 필요합니다. 실패하면 이 조사를 건너뛰고 나머지는 계속하십시오.

---

## 3. 하지 말아야 할 것

- **멈춤의 원인을 추측해서 고치지 마십시오.** 이번 작업도 계측입니다. `stage=` 값을 얻는 것이 성과물입니다.
- 지금까지의 수정을 하나도 되돌리지 마십시오.
- 감시 스레드에서 `Log()`나 `LogTry()`를 부르지 마십시오. 2-1의 자체 경로만 씁니다.
- 감시 스레드에서 메모리를 크게 할당하거나 COM을 호출하지 마십시오.
- `SuspendThread` 상태에서 로그를 쓰거나 할당하지 마십시오. 2-5의 주의 사항을 그대로 지키십시오.
- `Log()` 진입 지점에 `WatchdogStage()`를 넣지 마십시오. 원래 단계를 덮어씁니다.
- 0.1에서 확인된 대로 메뉴 표시 지연은 이미 해결되었습니다. 그쪽을 더 손대지 마십시오.
- `WH_MOUSE_LL`, `WH_KEYBOARD_LL`을 도입하지 마십시오.

---

## 4. 검증

**감시 장치 (핵심)**
- [ ] 프로그램을 띄우면 `%USERPROFILE%\.bamti\watchdog.log`가 만들어진다.
- [ ] 정상 동작 중에 `alive stage=... ping=ok` 줄이 5초마다 쌓인다.
- [ ] 프로그램을 강제 종료해도 마지막 줄까지 파일에 남아 있다.
- [ ] 멈춤을 재현하면 `STUCK stage=... rip=... rva=... log_busy=...` 줄이 남는다.
- [ ] **그 줄 전체를 보고한다. 이것이 이번 작업의 성과물이다.**

**해석 가능성**
- [ ] `alive` 줄이 멈춤 중에도 계속 나오는지, 끊기는지 보고한다. 둘의 의미가 다르다.
- [ ] `log_busy` 값을 보고한다.

**회귀**
- [ ] 유휴 60초 누적 CPU가 3.8초보다 나빠지지 않는다. 감시 스레드는 5초마다 한 줄만 쓴다.
- [ ] 메뉴 열기와 바깥 클릭 닫기가 지금과 같이 동작한다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 5. 커밋

하나로 만드십시오.

```
chore: 감시 스레드에 독립 로그와 정지 시 스택 지점 기록을 넣는다
```

본문에 다음을 적으십시오.

- `LogTry`의 `try_lock`이 실패하면 침묵하므로, UI 스레드가 `Log()` 안에서 멈춘 경우 아무것도 남지 않았다는 점
- 감시 스레드가 자체 파일과 `OutputDebugStringW`로만 기록하도록 바꾼 점
- 정지 시 `rip`과 `rva`를 남기며, `SuspendThread`와 `ResumeThread` 사이에서는 문맥 조회만 한다는 점
