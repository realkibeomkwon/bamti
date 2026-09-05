# 작업 지시서 31: 독 준비의 16.6초를 분해한다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

`FIX-BOOT-COLD-START-2.md` 3절의 계측으로 준비 시간이 두 덩어리로 갈린다는 것까지 확인했습니다. 이번에는 그 두 덩어리의 내부를 갈라서, 어디를 고쳐야 하는지 확정합니다.

**이 지시서는 계측이 목적입니다. 성능을 고치지 마십시오.** 무엇을 어떻게 고칠지는 이 계측의 값을 보고 다음 지시서에서 정합니다.

---

## 1. 확정된 사실

```
2026-09-03 13:49:35.762 [dock] warmup items=19 16578ms collect=9265 icons=7297 (ms)
```

`collect`와 `icons`의 합이 16562ms로 전체 16578ms와 16ms 차이입니다. **계측되지 않은 구간은 없습니다.**

직전 부팅(09:05)의 준비 시간은 4531ms였으므로 이번에 3.7배로 늘었습니다. 다만 이번에는 부팅 후 19초 만에 기동해서 디스크 경합이 가장 심한 구간에 준비가 걸렸습니다. **두 값을 그대로 비교하면 안 됩니다.**

`PLAN.md` 0절은 `CollectDockApps`가 창 하나당 COM 프로퍼티 스토어를 네 번 여는 것을 원인으로 지목했습니다. 그 가설은 **절반만 맞았습니다.** `collect`가 9.3초로 더 크지만 `icons`도 7.3초라서, 한쪽만 고치면 절반밖에 줄지 않습니다.

---

## 2. `collect` 안에서 무엇이 오래 걸리는가

### 2-1. 로그가 가리키는 곳

준비가 도는 구간의 로그에서 시각 간격을 보면 단서가 나옵니다.

```
13:49:20.746 [dock] pin cmp branch=path a=[C:\Windows\explorer.exe](23) b=[...]
13:49:22.297 [dock] pin cmp branch=path a=[...OutlookForWindows...] b=[...]
13:49:26.908 [dock] pin cmp branch=path a=[...MSTeams...] b=[...]
13:49:27.612 [dock] pin cmp branch=path a=[...chrome.exe](53) b=[...]
13:49:27.676 [dock] pin cmp branch=path a=[...firefox.exe](44) b=[...]
```

`pin cmp`는 `src/task_list.cpp` 775행의 `LogPinCmpFalse`에서 나오고, 이 함수는 `SameDockPin`이 거짓을 돌려줄 때만 불립니다. `SameDockPin`을 부르는 곳은 1330행의 `find_by_path`이며, 이 람다는 1341행부터 시작하는 **고정 항목 해소 루프** 안에서 돕니다.

즉 위 다섯 줄은 전부 고정 항목 해소 루프 안에서 찍힌 것이고, 그 사이에 **4.6초짜리 공백**(13:49:22.297 → 13:49:26.908)이 있습니다. 이 루프가 `collect` 9.3초의 상당 부분을 차지한다고 볼 근거가 됩니다.

**다만 이것은 로그 간격을 보고 세운 추정입니다.** `LogPinCmpFalse`는 `kPinCmpLogMax`에서 로그를 끊으므로 위 다섯 줄이 루프의 전체 범위라는 보장이 없고, 공백 구간에서 무엇이 돌았는지도 알 수 없습니다. 계측으로 확정해야 합니다.

### 2-2. 계측할 구간

`src/task_list.cpp` 1157행의 `CollectDockApps`를 네 구간으로 나누어 재십시오.

| 이름 | 범위 | 행 |
|---|---|---|
| `shell` | `ShellBrowserHwnds()` 호출 | 1176 |
| `enum` | `EnumWindows` 호출과 `PruneWindowCache()` | 1179~1204 |
| `ingest` | `ingest(true)`와 조건부 `ingest(false)` | 1293~1296 |
| `pins` | 고정 항목 해소 루프 | 1341~1395 |

네 값의 합이 전체에 가깝지 않으면 나머지 구간에도 계측을 넣어야 합니다. 판정할 수 있도록 **전체 시간도 함께 남기십시오.**

### 2-3. `enum` 안을 한 단계 더 나눈다

`enum` 구간은 창마다 `CachedWindow`와 `WindowTitle`을 부릅니다. `CachedWindow`(637행)는 캐시가 비어 있으면 `WindowExePath`, `ReadWindowProps`, `PathAumid`를 차례로 부르는데, **부팅 직후 첫 호출에서는 캐시가 반드시 비어 있습니다.** `PLAN.md`가 지목한 프로퍼티 스토어 비용이 바로 여기에 몰립니다.

`enum` 안에서 두 값을 누적해 주십시오.

- `cached`: `CachedWindow` 호출에 쓴 시간의 합
- `title`: `WindowTitle` 호출에 쓴 시간의 합

`WindowTitle`(893행)은 `SendMessageTimeoutW`로 넘어가는 경로가 있어 응답이 느린 창마다 지연이 쌓입니다. 이 둘을 갈라야 프로퍼티 스토어와 창 텍스트 중 어느 쪽이 지배적인지 알 수 있습니다.

또한 창 개수와 캐시 적중 횟수도 함께 남기십시오. 창이 몇 개인지 모르면 한 창당 비용을 계산할 수 없습니다.

### 2-4. 로그 형식

`CollectDockApps`는 준비 경로인지 아닌지를 모르므로, **평상시 호출에서도 이 로그가 나옵니다.** 지금은 그래도 괜찮습니다. 원인을 찾는 단계이고, 평상시 값과 부팅 직후 값을 대조하는 것 자체가 판단 재료가 됩니다.

```
[perf] collect total=9265 shell=... enum=... ingest=... pins=... (ms) windows=... cache_hit=...
[perf] collect enum cached=... title=... (ms)
```

두 줄로 나누어도 되고 한 줄로 합쳐도 됩니다. 기존 `[perf]` 로그의 `avg/max` 형식을 따를 필요는 없습니다. 이 값들은 누적 합계이지 통계가 아니기 때문입니다.

---

## 3. `icons` 안에서 무엇이 오래 걸리는가

### 3-1. 로그가 가리키는 곳

준비 구간에서 아이콘 적재 로그가 18줄 나왔고, 출처별 분포는 이렇습니다.

| 출처 | 개수 |
|---|---|
| `exe_extract` | 9 |
| `aumid` | 6 |
| `exe_shell` | 3 |

시각은 13:49:30.162부터 13:49:35.7까지 약 5.5초에 걸쳐 있습니다. 18개로 나누면 한 개당 평균 300ms입니다.

```
13:49:30.162 [dock] icon source=aumid name=캡처 도구 px=36
13:49:31.683 [dock] icon source=aumid name=설정 px=36
13:49:31.791 [dock] icon source=exe_shell name=explorer.exe px=36
13:49:31.943 [dock] icon source=exe_shell name=olk.exe px=36
13:49:32.163 [dock] icon source=exe_shell name=ms-teams.exe px=36
```

13:49:30.162와 13:49:31.683 사이가 1.5초입니다. 로그는 **성공한 뒤에** 찍히므로, 이 간격 안에는 실패해서 다음 경로로 넘어간 시도까지 포함되어 있습니다.

### 3-2. 계측할 구간

`src/dock.cpp` 1851행의 `Dock::EnsureIcons`에서 `LoadIconBitmap`(1861행) 호출 하나하나를 재고, **출처별로 합계와 개수를 나누어** 누적하십시오.

`LoadIconBitmap`은 `BitmapFromShellItem`, `BitmapFromAumid`, 실행 파일 추출을 순서대로 시도하면서 실패하면 다음으로 넘어갑니다. 성공한 출처만 로그에 남으므로, **실패한 시도의 비용이 어디에 숨어 있는지 알 수 없는 것이 지금의 문제입니다.**

따라서 두 가지를 남기십시오.

1. 출처별 합계: `aumid=... exe_shell=... exe_extract=... spotlight=...` 형식으로, 성공한 출처 기준의 소요 시간 합과 개수
2. 캐시 적중 횟수: `icon_cache_`에서 바로 꺼낸 항목이 몇 개인지

부팅 직후에는 캐시가 비어 있으므로 적중이 0에 가까울 것입니다. 그 값을 확인해 두면, 이후에 캐시를 파일로 저장하는 방향이 유효한지 판단할 근거가 됩니다.

```
[perf] icons total=7297 loaded=19 cache_hit=0 aumid=.../6 exe_shell=.../3 exe_extract=.../9 (ms/n)
```

### 3-3. 준비 경로에서만 남기십시오

`EnsureIcons`는 평상시 `Rebuild`에서도 불립니다. 매번 이 로그를 남기면 로그가 지저분해지므로, **`warming_up_`이 참일 때만** 남기십시오. `FIX-BOOT-COLD-START-2.md` 3절에서 `warmup_collect_ms_`와 `warmup_icons_ms_`를 같은 방식으로 처리해 두었으니 그 흐름에 맞추면 됩니다.

`CollectDockApps`는 `task_list.cpp`에 있어서 `warming_up_`을 볼 수 없으므로 2-4절대로 항상 남깁니다. 두 절의 처리가 다른 이유가 이것입니다.

---

## 4. 판정 기준

재부팅한 뒤 다음 줄들을 완료 보고에 그대로 옮겨 적으십시오.

1. `[dock] warmup` 줄
2. `[perf] collect` 줄 (준비 구간의 것)
3. `[perf] icons` 줄

판정은 셋입니다.

1. **`collect`의 네 구간 합이 `collect` 전체에 가까워야 합니다.** 크게 벌어지면 계측되지 않은 구간이 남아 있다는 뜻이므로 그 사실을 보고하십시오.
2. **`icons`의 출처별 합이 `icons` 전체에 가까워야 합니다.** 크게 벌어지면 실패한 시도의 비용이 큰 것이므로, 그 사실 자체가 중요한 발견입니다.
3. `pins` 구간이 `collect`의 절반을 넘는지 확인하십시오. 2-1절의 추정이 맞는지 가리는 값입니다.

---

## 5. 하지 말아야 할 것

- **성능을 고치지 마십시오.** 이번 작업은 계측입니다. 값이 크게 나오는 구간을 발견해도 그 자리에서 고치지 말고 보고만 하십시오.
- **`EnsureIcons()`를 준비 경로에서 빼지 마십시오.** `src/dock.cpp` 1739~1748행의 필터가 아이콘 적재 결과로 항목을 걸러 내고 있어서, 아이콘 없이 넘어가면 실행 중인 비고정 앱이 목록에서 빠집니다.

```cpp
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == DockItemKind::kSpotlight || items_[i].pinned ||
        (i < icons_.size() && icons_[i] != nullptr)) {
```

- **`CachedWindow`의 캐시 정책을 바꾸지 마십시오.** 부팅 직후에 캐시가 비어 있는 것은 정상이며, 그 사실을 확인하는 것이 이번 계측의 목적입니다.
- `LoadIconBitmap`의 시도 순서를 바꾸지 마십시오. 순서를 바꾸면 아이콘이 달라질 수 있고, 이번 계측의 기준값도 함께 흔들립니다.
- `kPinCmpLogMax`를 늘려서 로그로 문제를 파악하려 하지 마십시오. 계측값으로 판정하는 것이 이번 작업의 방침입니다.
- 준비 타이머의 지연(`kWarmupDelayMs`)을 바꾸지 마십시오. 그것은 값을 보고 나서 정할 사항입니다.
- 빌드는 `D:\repos\bamti\build` 트리에 Release 구성으로 하십시오. 실행 중이면 링크가 실패하므로, 그때는 컴파일만 확인하고 그 사실을 보고해 주십시오.
- 검증 절차에 레지스트리 쓰기 명령을 넣지 마십시오. 조회만 하십시오.
- 재부팅이 필요한 검증은 수행하지 마십시오. 사용자에게 부탁할 항목입니다.
