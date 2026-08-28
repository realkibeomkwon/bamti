# 정리 대상 조사

안정성을 유지한 채, 독 코드를 넓게 고치지 않고 측정과 호출 관계만 적습니다. 감시 스레드, `msg flood`, 게시 메시지 `storm`, `popup alive`/`dismiss`, `pin cmp`/`pin miss`는 제거 대상이 아닙니다. `pin cmp`만 핀 매칭이 해결된 뒤에 제거 후보가 됩니다.

이번 커밋에서 제거한 것은 호출이 없는 `Channel` / `OpaqueColor` / `BlendOn`뿐입니다.

---

## 1. `WM_PAINT`와 `Paint()`

`HandleMessage`의 `WM_PAINT`는 `Paint()`를 부릅니다. `Paint()`는 `BeginPaint` 다음에 `RenderLayered()`를 다시 부릅니다. 독 창은 `WS_EX_LAYERED`이고 실제 픽셀은 `UpdateLayeredWindow`로 올립니다.

**측정.** `WM_PAINT` 진입마다 `[dock] wm_paint n=` 로그를 넣었습니다. 독을 표시하고 `CollectDockApps`가 돌아가는 재구성까지 포함한 뒤에도 이 줄은 **한 번도** 남지 않았습니다. `Rebuild()` 끝의 `InvalidateRect(hwnd_, nullptr, FALSE)`도 같은 세션에서 `WM_PAINT`를 만들지 않았습니다.

30분 연속 사용까지는 이 세션에서 채우지 못했습니다. 다만 재구성·표시·숨김처럼 `InvalidateRect`가 실제로 나가는 경로에서도 페인트가 오지 않았으므로, 계층 창에서는 `WM_PAINT`/`Paint()`가 그리는 경로가 아닙니다.

**정리 후보.** `Paint()`와 `Rebuild`의 `InvalidateRect`는 이중 그리기를 노리는 코드가 아니라, 오지 않는 메시지를 위한 자리입니다. 지울 때는 표시 직후 빈 창이 되는지 따로 확인해야 합니다. 지금은 유지합니다.

---

## 2. `HandleHot`과 `HandleMessage`의 `WM_MOUSELEAVE`

두 처리가 있습니다.

- 핫 창 `HandleHot` (`dock.cpp` `WM_MOUSELEAVE`): `PointerOverUi()`가 거짓일 때만 `StartHideTimer()`. 호버 인덱스는 건드리지 않습니다.
- 독 창 `HandleMessage` (`WM_MOUSELEAVE`): `hover_`를 `-1`로 되돌리고 필요하면 `RenderLayered()`한 뒤, 마찬가지로 `PointerOverUi()`가 거짓이면 `StartHideTimer()`.

`PointerOverUi()`는 팝업/`Busy()`, 독 사각형, 핫 가장자리를 함께 봅니다. 핫에서 독으로 올라가면 핫이 `WM_MOUSELEAVE`를 받지만 `PointerOverUi()`가 참이라 숨기지 않습니다. 독에서 핫으로 내려가면 독이 leave를 받아 호버만 지우고, 핫 위에 있으면 역시 숨기지 않습니다.

**합치면.** 공용 함수로 묶을 수는 있습니다. 다만 호버를 지우는 쪽은 **독 창 leave에만** 있어야 합니다. 핫 leave에서 호버를 지우거나 독을 다시 그리면, 핫에서 아이콘으로 들어가는 순간에 강조가 떨어집니다. 숨김 판정만 공용으로 빼는 편이 안전합니다. 지금 합치지 않습니다.

---

## 3. `PollPointer` 타이머 (`kPollTimerId`, `kIdlePollMs` = 500)

`UpdateIdleTimer()`는 독이 보이거나 팝업이 열려 있을 때만 이 타이머를 켭니다. `PollPointer()`는 전체 화면 여부를 다시 보고, 팝업이 열려 있으면 `popup_.Tick()`을 부르며, 커서가 UI 위에 있으면 표시를 유지하고 아니면 `StartHideTimer()`를 겁니다.

**실제로 껐을 때.** `UpdateIdleTimer()`가 `kPollTimerId`를 켜지 않게 만든 뒤 Release로 확인했습니다. 이 끄기는 커밋하지 않았습니다.

- 핫 가장자리에 커서를 두면 `HandleHot`의 `WM_MOUSEMOVE`만으로 독이 나타났습니다. 첫 표시는 재구성이 끝날 때까지 지연되었습니다.
- 커서를 화면 가운데로 옮긴 뒤 500ms가 지나도 독 너비가 그대로였습니다. leave 메시지가 없거나 숨김 타이머가 취소된 경우, 폴링이 없으면 독이 남아 있었습니다.
- 우클릭 메뉴는 열렸고, 2초 동안 커서를 둔 뒤에도 유지되었습니다. 바깥 클릭 뒤 `[popup] dismiss reason=outside-poll`이 남았습니다. 팝업 자체 가드 타이머(50ms)의 `Tick("timer")`가 닫았고, 독 `PollPointer`의 `popup_.Tick()`이 아니었습니다.

**결론.** 표시는 마우스 메시지로 충분합니다. 숨김과 전체 화면 가림은 leave/`kFullscreenMsg`가 비면 폴링이 보완합니다. 팝업이 열린 동안의 `PollPointer` → `popup_.Tick()`은 팝업 가드 타이머와 겹칩니다. 타이머 자체를 지금은 유지하고, 팝업 분기만 나중에 줄일 후보입니다.

---

## 4. `icon_cache_` 생명 주기

키는 `items_[i].key + L"|" + 픽셀`. 픽셀은 `Dip(kIconDip)`입니다. 값은 `LoadIconBitmap`이 만든 `HBITMAP`입니다. `icons_[i]`는 캐시 비트맵을 가리킬 뿐 따로 소유하지 않습니다.

`EnsureIcons()`는 지금 목록에 있는 키를 `live`로 모은 뒤, 캐시에 없거나 널이면 로드하고, `live`에 없는 키는 `DeleteObject` 후 `erase`합니다. 같은 키에 널이 아닐 때는 덮어쓰지 않으므로 그 경로에서 이전 비트맵을 버리지 않습니다.

비우는 시점은 소멸자의 `ResetIconCache()`입니다. 재구성마다 전부 비우지는 않습니다.

**누수.** 죽은 키는 `EnsureIcons`가 지우고, 종료 시 `ResetIconCache`가 나머지를 지웁니다. 창이 살아 있는 동안 DPI가 바뀌면 키의 픽셀 부분이 달라져 이전 DPI 항목은 `live`에 없어 삭제됩니다. 지금 보이는 누수 지점은 없습니다.

---

## 5. `CollectDockApps` 한 번의 소요 시간

`QueryPerformanceCounter`로 나눈 값입니다. 창 열거의 `CachedWindow` 구간을 프로퍼티로 잡았고, 아이콘은 `EnsureIcons()`입니다. 계측 코드는 측정 후 제거했습니다.

첫 표시(캐시 차가움):

| 구간 | 시간 | 내용 |
| --- | --- | --- |
| 열거 벽시계 | 212ms | `EnumWindows` + `IsTaskWindow` + 프로퍼티 |
| 프로퍼티 | 209ms | `CachedWindow` (경로·AUMID 등). 열거 212ms의 거의 전부 |
| 그룹/핀 | 238ms | 가상 바탕 화면 필터, 그룹, 표시 이름, 핀 매칭 |
| 아이콘 | 299ms | 첫 `EnsureIcons` 13개. 이어서 12개로 줄인 두 번째는 0ms |

같은 세션의 재구성(경로 캐시가 채워진 뒤): 열거 134ms, 프로퍼티 133ms, 그룹 229ms. 스냅이 같으면 `EnsureIcons`는 건너뜁니다.

최적화 순서는 아이콘 로드(차가울 때), 그다음 그룹/이름, 그다음 프로퍼티입니다. 열거 루프 자체는 수 밀리초입니다.

---

## 6. 죽은 코드

호출이 없는 심볼만 대상으로 했습니다.

- `Channel`, `OpaqueColor`, `BlendOn` (`dock.cpp`): 정의만 있고 호출이 없습니다. **이번 커밋에서 삭제했습니다.**
- 아이콘 파이프라인 (`BitmapFromJumboList`, `HasStraightAlpha` 등)은 `LoadIconBitmap` / `FinalizeIconBitmap`에서 쓰입니다.

멤버 중 `hover_`, `drag_cursor_`, 드래그 계측 카운터는 이번 기능과 진단에 쓰입니다. `Paint()`는 호출되지만 1번 측정상 `WM_PAINT`가 오지 않습니다. 죽은 함수가 아니라 죽은 경로에 가깝습니다.

---

## 다음에 손댈 후보 (이번 범위 밖)

1. `Rebuild` 끝 `InvalidateRect`와 `Paint()` — `WM_PAINT`가 오는지 더 긴 사용으로 확인한 뒤에만 제거.
2. `PollPointer`의 팝업 `Tick()` 중복.
3. 차가울 때 `EnsureIcons` 299ms — 아이콘을 한 프레임에 모두 만들지 않기.
4. 그룹/표시 이름 229ms — `AppsFolderDisplayName` / `DisplayNameFor` 캐시.
5. `pin cmp` — 핀 매칭이 맞은 뒤에 로그 제거.
6. 핫/`HandleMessage` leave의 숨김 판정만 공용 함수로 추출. 호버 정리는 독 leave에 남김.
