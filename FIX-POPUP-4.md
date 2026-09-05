# 작업 지시서 5: 가드 타이머가 돌지 않는 문제와 Spotlight 정지

커밋 `ec5b7ad`로 열림 폭주는 해결되었습니다. 그러나 메뉴가 바깥 클릭으로 닫히지 않고, Spotlight에 입력하면 프로그램 전체가 멈춥니다.

---

## 0. 해결된 것

우클릭 한 번에 `menu open`이 **한 줄만** 남습니다. 이전에는 44밀리초 동안 열두 줄이었습니다.

```
09:51:44.693 [dock]  menu open index=0 name=explorer.exe running=1 windows=1 pinned=1
09:51:44.703 [popup] open rows=7 shown 16ms
09:51:44.704 [dock]  menu hwnd=00000000000E0CD2 rows=7 16ms
09:51:44.704 [popup] msg=mousemove pt=12,258 inside=0
```

`armed_` 수정은 성공했습니다. `menu reopen storm` 줄도 없습니다. **되돌리지 마십시오.**

지문 기반 생략도 잘 동작합니다. `rebuild skip fingerprint items=13 0ms`가 그 증거입니다.

---

## 1. 문제 A: 가드 타이머가 돌지 않는다

### 1.1 증거

위 로그의 마지막 줄이 09:51:44.704입니다. 그 뒤로 **아무것도 없습니다.**

`tick_`은 `Open()`에서 0으로 초기화되고 가드 타이머는 50ms 주기이므로, `alive tick=20`이 약 1초 뒤인 09:51:45.7에 나와야 합니다. 나오지 않았습니다.

그리고 사용자는 이 상태에서 **Spotlight를 열 수 있었습니다.** Spotlight는 같은 프로세스의 같은 UI 스레드가 처리합니다. 즉 **UI 스레드는 살아서 메시지를 처리하고 있었습니다.**

두 사실을 합치면 결론은 하나입니다. **UI 스레드는 돌고 있는데 가드 타이머의 `WM_TIMER`만 오지 않습니다.**

이것이 바깥 클릭으로 닫히지 않는 이유를 전부 설명합니다. 배경 창인 팝업은 바깥 클릭 메시지를 받지 못하므로(로그에 `msg=lbuttondown`이 없습니다), 닫기는 전적으로 가드 타이머 폴링에 달려 있습니다. 그 타이머가 죽어 있으니 닫힐 방법이 없습니다.

### 1.2 왜 죽었는지는 아직 모른다

`ArmGuardTimer()`는 `SetTimer`의 **반환값을 확인하지 않습니다.**

```cpp
void PopupSurface::ArmGuardTimer() {
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kPopupGuardTimer, kPopupGuardMs, nullptr);
  }
}
```

`SetTimer`는 실패하면 0을 돌려줍니다. 지금 구조로는 실패해도 알 수 없습니다.

정적 분석으로는 여기까지입니다. 다음 두 가지를 동시에 진행하십시오. 하나는 원인을 밝히고, 하나는 원인과 무관하게 동작을 회복시킵니다.

### 1.3 계측한다 (필수)

**`ArmGuardTimer()`에서 반환값을 기록하십시오.**

```cpp
const UINT_PTR id = SetTimer(hwnd_, kPopupGuardTimer, kPopupGuardMs, nullptr);
Log(L"popup", L"arm guard id=%llu err=%lu", static_cast<unsigned long long>(id), id == 0 ? GetLastError() : 0);
```

**틱 로그를 촘촘하게 바꾸십시오.** 현재는 20틱마다 남기므로 타이머가 몇 번이라도 돌았는지 알 수 없습니다.

- 처음 **5틱**은 매번 남깁니다.
- 그 뒤로는 20틱마다 남깁니다.

```cpp
++tick_;
if (tick_ <= 5 || tick_ % 20 == 0) {
  Log(L"popup", L"alive tick=%u src=%s armed=%d hot=%d inside=%d",
      tick_, /* "timer" 또는 "owner" */, armed_ ? 1 : 0, hot_, inside ? 1 : 0);
}
```

이 두 줄이면 다음 재현에서 "타이머가 아예 안 돈다"와 "돌다가 멈춘다"가 즉시 구분됩니다.

### 1.4 소유자 쪽에서도 폴링을 돌린다 (필수)

타이머가 죽어도 메뉴가 닫히도록 이중화하십시오.

`PopupSurface`에 공개 함수를 추가합니다.

```cpp
// 팝업이 열려 있을 때 소유자가 주기적으로 부른다. 여러 번 불려도 안전해야 한다.
void Tick();
```

`OnGuardTimer()`의 본문을 `Tick()`으로 옮기고, 자신의 `WM_TIMER`는 `Tick()`을 부르게 하십시오. 어느 경로에서 왔는지 구분할 수 있도록 인자나 멤버로 출처를 남겨 1.3의 `src=` 항목에 반영하십시오.

`Dock::PollPointer()`에서 `popup_.IsOpen()`이면 `popup_.Tick()`을 부르십시오. 이 타이머는 로그상 정상 동작이 확인된 경로입니다. 상단바의 `PopupSurface`에도 같은 처리를 하십시오.

독 폴링은 500ms 주기이므로 닫힘이 최대 0.5초 늦을 수 있습니다. 자체 타이머가 살아 있으면 50ms로 동작하므로, 이는 어디까지나 안전망입니다.

**중복 호출에 안전해야 합니다.** `Tick()` 안에서 상태를 갱신하므로, 두 경로가 같은 틱에 들어와도 잘못된 판정이 나오지 않도록 하십시오. 특히 버튼 눌림·뗌 전이 판정이 두 번 소비되지 않게 주의하십시오.

---

## 2. 문제 B: Spotlight에 입력하면 프로그램이 멈춘다

### 2.1 현재 구조

앞선 조사에서 나온 내용을 다시 확인했습니다. **검색 자체는 이미 워커 스레드에 있습니다.** `src/spotlight.cpp:1581`에 `std::thread`가 있고 `search_gen_`으로 세대를 관리합니다. 따라서 `WalkNamed`의 `FindFirstFileW`와 `QueryIndexedHits`의 COM 호출은 UI 스레드가 아닙니다.

남은 UI 스레드 작업은 다음입니다.

| 위치 | 내용 |
|---|---|
| `spotlight.cpp:2184` | 그리기 도중 `EnsureIcon(row.match)` 호출 |
| `spotlight.cpp:1526` | `EnsureIcon` 내부에서 `SHGetFileInfoW` |
| `ApplyFilter` | `LayoutWindow()`, `RebuildMatches()` |

**`SHGetFileInfoW`가 유력합니다.** 이 함수는 셸 확장을 로드하고 파일 시스템에 접근하므로, 네트워크 경로나 느린 항목에서는 수 초를 블로킹합니다. 그리기 경로에서 결과 행마다 호출하면 입력할 때마다 UI가 멈춥니다.

### 2.2 계측한다 (필수)

고치기 전에 어디서 시간을 쓰는지 확정하십시오.

- `EnsureIcon()` 한 번 호출이 **10ms를 넘으면** 남기십시오.

  ```cpp
  Log(L"spotlight", L"icon %ums path=%s", ms, /* 경로 */);
  ```
- `ApplyFilter()`의 기존 50ms 임계 로그는 그대로 두십시오.
- 결과 목록을 그리는 함수 전체 소요가 **16ms를 넘으면** 남기십시오.

  ```cpp
  Log(L"spotlight", L"paint rows=%zu %ums", /* 행 수 */, ms);
  ```

### 2.3 아이콘을 그리기 경로에서 뺀다 (필수)

계측 결과와 무관하게 이 구조는 고쳐야 합니다. 그리기 도중에 셸 API를 부르면 안 됩니다.

- `EnsureIcon()`을 그리기 경로에서 호출하지 마십시오.
- 아이콘이 아직 없으면 **자리표시자를 그리고 넘어갑니다.** 목록은 즉시 나와야 합니다.
- 아이콘 추출은 이미 있는 검색 워커 스레드에서 수행하고, 끝나면 `PostMessage`로 UI 스레드에 알려 해당 행만 다시 그리십시오.
- 세대(`search_gen_`) 검사를 아이콘 결과에도 적용해, 지난 질의의 아이콘이 뒤늦게 도착해도 버려지게 하십시오.
- `SHGetFileInfoW`를 워커 스레드에서 부르려면 그 스레드에서 `CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)`를 호출해야 합니다. 스레드 시작과 끝에서 짝을 맞추십시오.

아이콘 캐시는 경로를 키로 하되, 이미 있는 캐시가 있으면 그것을 쓰고 새로 만들지 마십시오.

---

## 3. 정리 작업: 미커밋 Spotlight 수정을 커밋한다 (필수, 가장 먼저)

`src/spotlight.cpp`와 `src/spotlight.hpp`에 **994줄 추가, 171줄 삭제** 규모의 미커밋 수정이 있습니다. 사용자 확인 결과 사용자가 직접 작성한 것이 아니며, 이전 세션에서 남은 작업입니다. 사용자가 처리를 위임했습니다.

이 상태를 그대로 두면 앞으로의 모든 `git diff`가 이 덩어리에 묻혀 검수가 어렵습니다.

**다른 작업보다 먼저**, 이 수정을 있는 그대로 별도 커밋으로 남기십시오. 내용을 고치거나 정리하지 말고 그대로 담으십시오.

```
chore: 작업 중이던 Spotlight 파일 검색 구현을 커밋한다
```

본문에 다음을 적으십시오.

- 이전 세션에서 남은 미커밋 작업이며 검토를 거치지 않았다는 점
- 파일 검색이 워커 스레드에서 동작하고 `search_gen_`으로 세대를 관리한다는 점
- 이번 커밋은 상태 보존이 목적이고 기능 검증은 하지 않았다는 점

이 커밋 이후에 1장과 2장의 수정을 진행하십시오.

---

## 4. 하지 말아야 할 것

- `armed_` 수정을 되돌리지 마십시오. 0장이 효과를 입증합니다.
- `UpdateWindow()` 수정, 폴링 입력 처리, 지문 기반 `Rebuild` 생략, 4dip 간격을 되돌리지 마십시오.
- 3장의 커밋에서 Spotlight 코드를 **정리하거나 개선하지 마십시오.** 있는 그대로 담습니다. 개선은 2장에서 지정한 범위만 합니다.
- `SHGetFileInfoW`를 UI 스레드에서 부르는 코드를 새로 만들지 마십시오.
- 워커 스레드에서 UI 창에 직접 그리거나 `SendMessage`를 쓰지 마십시오. `PostMessage`만 씁니다.
- `WH_MOUSE_LL`, `WH_KEYBOARD_LL`을 새로 도입하지 마십시오.
- `SetForegroundWindow`, `AttachThreadInput`을 도입하지 마십시오.
- `WS_EX_NOACTIVATE`를 떼지 마십시오.
- 1.4의 안전망을 넣었다고 해서 1.3의 계측을 생략하지 마십시오. 원인을 모른 채 덮으면 다른 모습으로 돌아옵니다.

---

## 5. 검증

**가드 타이머 (핵심)**
- [ ] 메뉴를 열면 `[popup] arm guard id=... err=...`가 남고, `id`가 0이 아니다.
- [ ] 메뉴를 연 뒤 `[popup] alive tick=1` 부터 `tick=5`까지 다섯 줄이 남는다.
- [ ] `src=`가 `timer`인지 `owner`인지 확인하고 보고한다. 이것이 원인을 가린다.
- [ ] 메뉴를 열고 바깥을 클릭하면 닫힌다. `[popup] dismiss reason=outside-poll`이 남는다.
- [ ] 버튼을 3초간 누르고 있다가 떼어도 메뉴가 남아 있다.
- [ ] 메뉴를 연 직후 버튼을 떼자마자 바깥을 클릭하면 닫힌다.
- [ ] 메뉴를 열 번 연속 열고 닫아도 멈추지 않는다.

**Spotlight**
- [ ] 검색어를 한 글자씩 입력할 때 입력이 끊기지 않는다.
- [ ] `[spotlight] icon ... %ums`와 `[spotlight] paint ... %ums`가 남는지 확인하고 수치를 보고한다.
- [ ] 결과가 나오기 전에도 목록 틀과 자리표시자가 즉시 보인다.
- [ ] 빠르게 타이핑했다가 지워도 지난 질의의 아이콘이 섞이지 않는다.
- [ ] Spotlight를 여닫아도 아이콘 핸들이 새지 않는다. 작업 관리자에서 GDI 개수를 확인한다.

**회귀**
- [ ] 우클릭 한 번에 `[dock] menu open`이 한 줄만 남는다.
- [ ] `[popup] open ... shown` 값이 16ms 이하다.
- [ ] 유휴 60초 누적 CPU가 3.8초보다 나빠지지 않는다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 6. 커밋

세 개로 나누십시오.

```
chore: 작업 중이던 Spotlight 파일 검색 구현을 커밋한다
```
3장입니다. **가장 먼저** 수행합니다.

```
fix: 팝업 가드 폴링을 소유자 타이머로 이중화한다
```
1장입니다. 본문에 `arm guard`의 `id` 값과 `src=` 관측 결과를 적으십시오.

```
perf: Spotlight 아이콘 추출을 그리기 경로에서 뺀다
```
2장입니다. 본문에 계측 수치를 적으십시오.
