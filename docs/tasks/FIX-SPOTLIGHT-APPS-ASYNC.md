# 작업 지시서 20: Spotlight 앱 목록 열거가 UI 스레드를 최대 1.8초 막는다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

Spotlight를 여는 순간 앱 목록을 동기로 다시 읽습니다. 그 사이 UI 스레드가 멈춥니다. 파일 검색이 이미 쓰고 있는 비동기 패턴으로 옮기고, 다시 읽는 주기도 늘립니다.

---

## 1. 측정 결과

### 1.1 여는 순간 1.3초를 기다린다

```
22:53:05.217  [spotlight] AppsFolder enumerate 199 apps in 1771.0 ms   ← 기동
22:57:53.236  [spotlight] AppsFolder enumerate 199 apps in 1311.8 ms   ← 사용자가 연 순간
22:57:54.787  [spotlight] icon 93ms path=...                            ← 그 뒤에야 결과 처리 시작
```

기동 시의 1771ms는 `[bar] ready`(22:53:03.433)와 `[host] menu bar ready`(22:53:05.273) 사이를 그대로 채웁니다. 시작이 그만큼 늦어집니다.

전체 로그에 이 열거가 **125회** 남아 있고, 소요 시간은 294ms에서 1771ms까지 흩어집니다. 셸 캐시 상태에 따라 달라집니다.

### 1.2 두 호출 지점이 모두 UI 스레드다

`src/spotlight.cpp`입니다.

```cpp
void Spotlight::Warmup(HWND owner, bool dark) {   // 1070행
  ...
  EnsureApps();                                    // 기동 경로
}

void Spotlight::Toggle(HWND owner, bool dark) {   // 1086행
  ...
  EnsureApps();                                    // 사용자가 여는 경로
```

`EnsureApps`는 `ReloadApps`를 그 자리에서 호출하고, `ReloadApps`는 `CollectAppsFolder`가 끝날 때까지 돌아오지 않습니다.

### 1.3 캐시 수명이 짧아 거의 매번 다시 읽는다

```cpp
constexpr ULONGLONG kAppReloadMs = 60000;   // spotlight.cpp:50
```

```cpp
void Spotlight::EnsureApps() {
  if (apps_.empty() || GetTickCount64() - apps_loaded_at_ > kAppReloadMs) {
    ReloadApps();
  }
}
```

Spotlight를 1분 안에 다시 여는 일은 드뭅니다. 실사용에서는 **열 때마다 매번** 조건이 참이 되고, `Warmup`이 기동 때 채워 둔 목록도 60초면 버려집니다.

### 1.4 옮기기 좋은 구조다

`ReloadApps`가 하는 일을 나누면 이렇습니다.

| 단계 | 내용 | 스레드 |
| --- | --- | --- |
| `DestroyAppIcons()` | `AppEntry.icon`(HICON) 파괴 | UI에 남겨야 함 |
| `CollectAppsFolder` / `CollectFolder` | 이름과 경로 쌍 수집. **여기가 느린 부분** | 옮길 수 있음 |
| `std::sort` + `CompareStringEx` | 이름순 정렬 | 옮길 수 있음 |
| `AppEntry` 생성, `apps_` 교체 | `LooksLikeFilesystemPath` 호출 포함 | UI에 남겨야 함 |

아이콘은 이 단계에서 만들지 않습니다. 나중에 `RequestIcon`이 비동기로 채웁니다. 그래서 백그라운드로 넘길 것은 **이름과 경로 쌍의 목록뿐**이고, GDI 객체는 스레드를 넘지 않습니다.

`QueryFiles`(1813행)가 이미 같은 모양의 비동기 패턴을 씁니다. 그 구조를 그대로 따르면 됩니다.

---

## 2. 수정 내용

모두 `src/spotlight.cpp`와 `src/spotlight.hpp`에서 이루어집니다.

### 2-1. 다시 읽는 주기를 늘린다

```cpp
// 앱 설치와 제거는 드문 사건이다. 1분마다 다시 읽으면 열 때마다 재열거하는 것과 같다.
constexpr ULONGLONG kAppReloadMs = 600000;   // 10분
```

이것만으로도 열거 횟수가 크게 줄지만, 만료된 순간에는 여전히 멈추므로 2-2가 필요합니다.

### 2-2. 열거를 백그라운드 스레드로 옮긴다

**메시지와 payload를 추가합니다.** `kIconReadyMsg`(47행) 옆에 둡니다.

```cpp
constexpr UINT kAppsReadyMsg = WM_APP + 42;
```

`FileSearchPayload`(961행) 옆에 둡니다.

```cpp
struct AppsPayload {
  uint64_t gen = 0;
  std::vector<std::pair<std::wstring, std::wstring>> raw;
};
```

**멤버를 추가합니다.** `spotlight.hpp`의 `apps_loaded_at_`(124행) 근처입니다.

```cpp
  std::atomic<uint64_t> apps_gen_{0};
  std::atomic<int> apps_inflight_{0};
```

선언도 추가합니다.

```cpp
  void StartAppsReload();
  void AcceptApps(void* payload);
```

**`search_gen_`을 재사용하지 마십시오.** 그 세대는 입력이 바뀔 때마다 올라가므로, 검색어를 타이핑하는 동안 앱 목록 갱신이 매번 취소됩니다. 반드시 별도 `apps_gen_`을 쓰십시오.

**`ReloadApps`를 `StartAppsReload`로 바꿉니다.** 수집과 정렬만 스레드에서 하고 결과를 보냅니다.

```cpp
void Spotlight::StartAppsReload() {
  if (hwnd_ == nullptr) {
    return;
  }
  // 이미 읽는 중이면 겹쳐 시작하지 않는다.
  if (apps_inflight_.load(std::memory_order_acquire) != 0) {
    return;
  }
  const uint64_t gen = apps_gen_.load(std::memory_order_acquire);
  const HWND hwnd = hwnd_;
  apps_inflight_.fetch_add(1, std::memory_order_acq_rel);
  std::thread([this, gen, hwnd]() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool com_ok = SUCCEEDED(com) || com == S_FALSE;

    std::unordered_set<std::wstring> seen;
    std::vector<std::pair<std::wstring, std::wstring>> raw;
    // 기존 ReloadApps의 수집과 정렬을 그대로 옮긴다.
    // CollectAppsFolder → 실패 시 CollectFolder 두 번 → std::sort까지.

    auto* payload = new AppsPayload;
    payload->gen = gen;
    payload->raw = std::move(raw);
    if (!PostMessageW(hwnd, kAppsReadyMsg, 0, reinterpret_cast<LPARAM>(payload))) {
      delete payload;
    }
    if (com_ok) {
      CoUninitialize();
    }
    apps_inflight_.fetch_sub(1, std::memory_order_acq_rel);
  }).detach();
}
```

소요 시간을 재는 `QueryPerformanceCounter` 구간과 `AppsFolder enumerate` 로그는 스레드 안에 그대로 두십시오. 이번 수정으로 이 시간이 줄어드는 것이 아니라 UI를 막지 않게 되는 것이므로, 값은 계속 관찰할 수 있어야 합니다.

**결과를 UI 스레드에서 받습니다.**

```cpp
void Spotlight::AcceptApps(void* payload) {
  std::unique_ptr<AppsPayload> owned(static_cast<AppsPayload*>(payload));
  if (!owned || owned->gen != apps_gen_.load(std::memory_order_acquire)) {
    return;
  }
  DestroyAppIcons();
  apps_.clear();
  apps_.reserve(owned->raw.size());
  for (auto& item : owned->raw) {
    AppEntry entry;
    entry.name = std::move(item.first);
    entry.path = std::move(item.second);
    entry.filesystem = LooksLikeFilesystemPath(entry.path);
    apps_.push_back(std::move(entry));
  }
  apps_loaded_at_ = GetTickCount64();
  Log(L"spotlight", L"apps ready %d", static_cast<int>(apps_.size()));
  if (visible_) {
    RebuildMatches();
    Present();
  }
}
```

`HandleMessage`에서 `kFileSearchDoneMsg`와 `kIconReadyMsg`를 받는 곳(1344행 부근) 옆에 `kAppsReadyMsg` 분기를 더해 `AcceptApps(reinterpret_cast<void*>(lparam))`를 호출합니다.

**`EnsureApps`는 이제 막지 않습니다.**

```cpp
void Spotlight::EnsureApps() {
  if (apps_.empty() || GetTickCount64() - apps_loaded_at_ > kAppReloadMs) {
    StartAppsReload();
  }
}
```

목록이 준비되기 전에 창이 열리면 앱 항목 없이 먼저 그려지고, 결과가 도착하면 `AcceptApps`가 다시 그립니다. 파일 검색이 이미 그렇게 동작하므로 화면 거동이 낯설지 않습니다.

### 2-3. 종료 시 정리에 새 경로를 포함한다

`WaitForFileSearches`(1882행)에 다음을 더합니다.

- 맨 앞에서 `apps_gen_.fetch_add(1, std::memory_order_acq_rel);`
- `search_inflight_` 대기 루프 옆에 `apps_inflight_`가 0이 될 때까지 기다리는 루프
- 남은 메시지를 비우는 부분에 `kAppsReadyMsg`를 `PeekMessageW`로 걷어내고 payload를 `delete`하는 루프

**이 정리를 빠뜨리면 창이 파괴된 뒤 도착한 payload가 새기 때문에 반드시 넣어야 합니다.**

---

## 3. 검증

1. 빌드한 뒤 실행하고 로그를 봅니다. 기동 시 `[host] menu bar ready`가 `AppsFolder enumerate`보다 **먼저** 나와야 합니다. 지금은 열거가 끝나야 그 줄이 나옵니다.
2. Spotlight를 열었을 때 창이 **즉시** 떠야 합니다. 로그에서 `[spotlight] paint`가 `AppsFolder enumerate`보다 먼저 나오면 UI가 막히지 않은 것입니다.
3. 열거가 끝난 뒤 `[spotlight] apps ready 199` 줄이 나오고, 그 시점부터 앱 이름으로 검색이 되어야 합니다.
4. 열자마자 앱 이름을 입력했을 때, 목록이 준비되면 결과가 채워져야 합니다. 입력한 글자가 사라지거나 초기화되면 안 됩니다.
5. Spotlight를 빠르게 여닫기를 열 번쯤 반복합니다. 크래시나 중복 열거(로그에 `AppsFolder enumerate`가 연달아 여러 줄)가 없어야 합니다. `apps_inflight_` 가드가 동작하는지 보는 항목입니다.
6. 열어 둔 상태에서 검색어를 빠르게 타이핑합니다. 앱 목록이 사라지지 않아야 합니다. `search_gen_`과 `apps_gen_`을 섞지 않았는지 보는 항목입니다.
7. 프로그램을 종료할 때 크래시가 없어야 합니다. 특히 Spotlight를 연 직후 곧바로 종료해 보십시오.

---

## 4. 주의 사항

- **GDI 객체를 스레드로 넘기지 마십시오.** 백그라운드가 다루는 것은 `std::wstring` 쌍의 목록뿐입니다. `DestroyAppIcons`와 `AppEntry` 생성은 반드시 UI 스레드에 남겨야 합니다.
- `LooksLikeFilesystemPath` 호출도 UI 스레드 쪽(`AcceptApps`)에 두십시오. 순수 문자열 판정으로 보이지만, 굳이 스레드 경계를 넘길 이유가 없습니다.
- `apps_`를 백그라운드에서 직접 건드리지 마십시오. 읽기도 하지 마십시오. UI 스레드가 같은 시점에 `RebuildMatches`로 순회할 수 있습니다.
- 열거 자체를 빠르게 만들려 하지 마십시오. `CollectAppsFolder`의 내용은 그대로 둡니다. 이번 목표는 **UI를 막지 않는 것**이지 열거를 최적화하는 것이 아닙니다.
- `kAppReloadMs`를 아예 없애고 무기한 캐시로 만들지 마십시오. 셸 알림으로 무효화하는 방식이 더 정확하기는 하지만 범위가 커집니다. 10분 주기로 두고, 필요하면 나중에 별도로 다룹니다.
