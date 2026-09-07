# 작업 지시서: 보충 주인 사전이 만든 CPU 회귀를 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

건드리는 파일은 `src/tray_mirror.cpp` 와 `src/tray_mirror.hpp` 두 개입니다.
`docs/tasks/FIX-FILL-ICON-AND-GHOST.md` 의 구현을 검수하다 나온 회귀입니다.

**이 회귀는 앞선 지시서의 설계 결함에서 비롯되었습니다.** 구현이 지시를 어긴 것이 아닙니다.

---

## 1. 증상

`FIX-FILL-ICON-AND-GHOST.md` 를 적용한 뒤 부하가 크게 나빠졌습니다.

| 항목 | 적용 전 | 적용 후 |
| --- | --- | --- |
| 코어 하나 기준 CPU | 0.9% | **11.3%** |
| `fill enum slow` | 41분에 4회 | **11분에 55회** |
| 최악 열거 시간 | 891ms | **41초** |

```
19:45:08.034 [tray] fill enum slow ms=41078 count=14
19:45:12.160 [tray] fill enum slow ms=4125 count=14
```

`fill enum slow` 는 `uia->Enumerate()` 하나만 재는 값입니다. **41초 동안 워커 스레드가 통째로 막혔습니다.** 그동안 `DrainInvoke` 가 돌지 못하므로 트레이 클릭이 전혀 반응하지 않습니다.

---

## 2. 무엇이 원인인가

### 2-1. 확정된 것: 주인 사전이 무한히 커진다 (설계 결함)

`fill_owners_` 는 **툴팁 문자열을 열쇠**로 하고, 지시서가 "지우지 마십시오"라고 했으므로 계속 누적됩니다.

그런데 툴팁이 실시간으로 바뀌는 트레이 아이콘이 있습니다. 작업 관리자가 대표적입니다.

```
[tray] fill refresh intercept lost tip="CPU 9% 메모리 41% 디스크 4% 네트워크 0%"
[tray] fill refresh intercept lost tip="CPU 12% 메모리 41% 디스크 3% 네트워크 0%"
```

툴팁이 바뀔 때마다 `fill_owners_.find(icon.tip)` 이 실패하므로 매번 이렇게 됩니다.

1. `WindowExePath(icon.owner)` 를 호출합니다. 안에서 `OpenProcess` 와 `QueryFullProcessImageNameW` 를 돕니다.
2. 사전에 **새 항목**을 추가합니다. 창은 하나인데 항목만 늘어납니다.

**같은 창 하나가 초당 한 개씩 사전 항목을 만듭니다.** 이것은 `intercept_cover_tips_` 에서 이미 한 번 겪은 함정과 정확히 같습니다. 앞선 지시서가 그 교훈을 반영하지 못했습니다.

측정 시점의 메모리는 이렇습니다. 참고값으로만 쓰십시오.

```
WorkingSet 218.7 MB / Private 155.4 MB / Handles 1216
```

### 2-2. 아직 확정되지 않은 것: 41초 열거

2-1 이 CPU 상승을 설명하기는 하지만, **UIA 열거 하나가 41초 걸린 이유까지 설명하지는 못합니다.** `Enumerate` 는 explorer 를 상대로 하는 프로세스 간 호출이므로 상대가 바빴을 수도 있습니다.

**3절을 고친 뒤 다시 재십시오.** 그래도 수십 초짜리 열거가 남으면 거기서 멈추고 보고하십시오. 원인을 짐작해서 덧붙이지 마십시오.

---

## 3. 무엇을 고치는가

**창 하나당 사전 항목이 하나만 남게 하십시오.**

### 3-1. 실행 파일 경로 조회를 창 단위로 캐시한다

```cpp
std::unordered_map<HWND, std::pair<DWORD, std::wstring>> fill_exe_by_owner_;
```

`RememberFillOwners` 에서 `WindowExePath` 를 부르기 전에 이 지도를 먼저 봅니다. 같은 창이고 PID 도 같으면 조회를 건너뜁니다. **창 하나에 `OpenProcess` 는 한 번이면 충분합니다.**

### 3-2. 같은 창의 낡은 툴팁 항목을 지운다

`FillOwner` 를 툴팁으로 넣을 때, **같은 `owner` 를 가진 기존 항목을 먼저 제거**하십시오. 그러면 툴팁이 아무리 바뀌어도 창 하나당 항목이 하나로 유지됩니다.

역방향 지도를 두면 훑지 않고 지울 수 있습니다.

```cpp
std::unordered_map<HWND, std::wstring> fill_tip_by_owner_;  // 그 창의 현재 툴팁
```

새 툴팁이 들어오면 이 지도로 이전 툴팁을 찾아 `fill_owners_` 에서 지우고, 새 툴팁으로 다시 넣습니다.

### 3-3. 상한을 둔다

위 둘을 해도 창 수만큼은 늘어납니다. 방어적으로 `fill_owners_` 와 `fill_exe_by_owner_` 에 상한을 두십시오. 예를 들어 256 개를 넘으면 `IsWindow` 가 거짓인 항목부터 정리합니다.

**상한에 걸렸다는 사실을 로그로 남기십시오.** 걸린다면 설계가 여전히 잘못된 것입니다.

```
[tray] fill owners pruned n=%zu
```

---

## 4. 함께 확인할 것

### 4-1. 걸러진 앱이 실제로 있었습니다

`fill skip unknown` 이 두 건 나왔습니다.

```
19:37:12.744 [tray] fill skip unknown tip="네이버 라이브 스트리밍 커넥터 ... (방송 중)"
19:49:48.878 [tray] fill skip unknown tip="ChatGPT"
```

가로채기가 한 번도 잡지 못한 앱은 상단바에 나타나지 않는다는 대가가 현실이 되었습니다. **이것은 사용자가 판단할 문제이므로 스스로 규칙을 바꾸지 마십시오.** 3절을 마친 뒤 이 줄이 몇 개의 서로 다른 툴팁에서 나오는지 세어 보고하십시오.

### 4-2. `K` 가 사라졌는지는 아직 확인되지 않았습니다

`fill key=` 가 한 번도 나오지 않았으므로 보충 항목 자체가 게시되지 않았고, 따라서 종료 시나리오가 검증되지 않았습니다. 3절을 마친 뒤 사용자에게 카카오톡 완전 종료를 부탁해 확인하십시오.

---

## 5. 검증

측정값으로 판정하십시오.

1. 빌드 후 실행하고 **작업 관리자를 켠 채로** 10분 이상 둡니다.
2. 코어 하나 기준 CPU 점유율을 재십시오. **목표는 1퍼센트 안팎입니다.** 회귀 전 값이 0.9퍼센트였습니다.
   ```powershell
   $p = Get-Process bamti; $t1=$p.TotalProcessorTime; $w1=Get-Date; Start-Sleep -Seconds 20
   $p.Refresh(); "{0:N1} %" -f ((($p.TotalProcessorTime-$t1).TotalMilliseconds)/(((Get-Date)-$w1).TotalMilliseconds)*100)
   ```
3. `fill enum slow` 가 10분에 몇 회인지 세십시오. 회귀 전은 41분에 4회였습니다.
4. 최악 열거 시간을 보십시오. **수십 초짜리가 남으면 멈추고 보고하십시오.**
5. `fill owners pruned` 가 나오는지 보십시오. 나오면 멈추고 보고하십시오.
6. 4-1 의 개수를 세어 보고하십시오.
7. 4-2 는 사용자에게 확인을 부탁하십시오. 사용자 화면에 입력을 합성하지 마십시오.

---

## 6. 건드리지 말 것

- `kFillRefreshMs` 5초 주기와 `TakeStartupFillPulse` 펄스
- `IsWindow` 와 PID 대조로 유령을 걸러내는 규칙 자체. 열쇠 관리만 고칩니다
- `FillPngForExe` 의 아이콘 캐시
- `InvokeByExe` 와 `Dock::RevealDockApp`
- `fill skip unknown` 의 게시 거부 규칙. 사용자 판단을 기다립니다
- `src/menu_bar.cpp` 전체
