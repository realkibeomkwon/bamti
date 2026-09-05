# 작업 지시서 10: WM_MOUSELEAVE 되먹임과 핀 매칭 실패

계측이 두 문제의 정체를 그대로 드러냈습니다. 하나는 원인이 확정되어 바로 고치고, 하나는 판정 지점을 좁혀 확인합니다.

---

## 1. 폭주의 정체: `WM_MOUSELEAVE` 되먹임

### 1.1 측정값

```
11:40:52.939 [perf] msg flood 57971/s top=[bamti.Dock:0x02A3 x57963] [bamti.DockHot:0x0200 x2] [bamti.Dock:0x0113 x2]
```

초당 **57,963건**이 독 창의 `0x02A3`입니다. `0x02A3`은 `WM_MOUSELEAVE`입니다.

두 번째와 세 번째는 각각 2건뿐입니다. 폭주는 이 한 메시지가 전부입니다.

### 1.2 되먹임 구조

`src/dock.cpp:1204`입니다.

```cpp
case WM_MOUSELEAVE:
  if (!PointerOverUi()) {
    StartHideTimer();
  } else {
    ArmMouseLeave();     // ← 커서가 창 밖인데 다시 추적을 건다
  }
  return 0;
```

`ArmMouseLeave()`는 `TrackMouseEvent(TME_LEAVE)`를 독 창(`hwnd_`)에 겁니다.

```
1. 커서가 독 창을 벗어난다. WM_MOUSELEAVE 가 온다.
2. PointerOverUi() 가 참이다.
3. ArmMouseLeave() 가 TrackMouseEvent 를 다시 건다.
4. 커서는 이미 독 창 밖이므로 WM_MOUSELEAVE 가 즉시 다시 온다.
5. 2번으로 돌아간다.
```

**커서가 이미 나간 창에 대해 "나가면 알려 달라"고 요청하니 즉시 응답이 옵니다.** 대기가 없으므로 메시지 루프가 전속력으로 돕니다.

### 1.3 왜 메뉴를 쓸 때 시작되는가

`PointerOverUi()`는 첫 줄에서 `Busy()`를 확인하고, `Busy()`는 팝업이 열려 있으면 참입니다.

```cpp
bool Dock::PointerOverUi() const {
  if (Busy()) {
    return true;
  }
  ...
}
```

즉 **메뉴가 열려 있는 동안 커서가 독 창을 벗어나면** 조건이 성립합니다. 메뉴는 독 위쪽에 뜨므로, 사용자가 메뉴 쪽으로 커서를 올리는 순간이 정확히 그 상황입니다.

이것이 지금까지 관측된 "코어 0.9개 점유", "간헐적으로 메뉴가 느리게 뜬다", 그리고 이전의 멈춤까지 설명합니다.

---

## 2. 핀 매칭: 판정 지점을 좁혀야 한다

### 2.1 측정값

```
[dock] pin miss key=C:\Windows\explorer.exe path=C:\Windows\explorer.exe canon=c:\windows\explorer.exe cached=0
[dock] pins c:\windows\explorer.exe c:\program files\...\snippingtool.exe ...
```

키의 정규화 결과가 `c:\windows\explorer.exe`이고, 핀 목록의 첫 항목도 `c:\windows\explorer.exe`입니다. **두 값이 같아 보이는데 매칭에 실패합니다.**

`SameDockPin()`의 마지막 줄은 `CanonicalPath(a) == CanonicalPath(b)`이므로 이 둘이 정말 같다면 참이 나와야 합니다.

### 2.2 아직 모르는 것

로그에 찍힌 값과 실제 비교에 들어간 값이 다를 수 있습니다. 가능성은 여럿이며 **어느 것인지 확정되지 않았습니다.**

- 핀 항목에 `\t`와 relaunch 명령이 붙어 있는데 로그가 그 부분을 잘라서 보여 주고 있을 수 있습니다.
- `SameDockPin()`이 마지막 줄이 아니라 AUMID 분기(687~691줄)로 빠졌을 수 있습니다.
- 매칭에 쓰인 핀 목록이 로그에 찍은 목록과 다를 수 있습니다.

**추측해서 고치지 마십시오.** 3-2의 계측으로 확정한 뒤에 별도 지시서로 다룹니다.

---

## 3. 수정 지시

### 3-1. `WM_MOUSELEAVE` 되먹임을 끊는다 (필수, 즉시 수정)

원인이 확정되었으므로 이번에는 계측이 아니라 수정입니다.

`src/dock.cpp:1204`의 `else` 분기를 **제거**하십시오.

```cpp
case WM_MOUSELEAVE:
  if (!PointerOverUi()) {
    StartHideTimer();
  }
  return 0;
```

`WM_MOUSELEAVE`는 커서가 이미 창을 벗어났다는 뜻이므로, 그 자리에서 추적을 다시 걸면 안 됩니다. 추적은 커서가 창 안으로 돌아왔을 때 다시 걸어야 하고, 그 처리는 이미 `WM_MOUSEMOVE` 분기에 있습니다.

`ArmMouseLeave()` 자체는 지우지 마십시오. `WM_MOUSEMOVE`에서 쓰고 있습니다.

`src/dock.cpp:1088`의 hot 창 쪽 `WM_MOUSELEAVE`에는 이 문제가 없습니다. 그대로 두십시오. `ArmHotMouseLeave()`도 같은 형태로 재무장하는 곳이 있는지 확인하고, 있으면 같은 방식으로 고치십시오.

**독이 숨겨지는 동작이 깨지지 않는지 반드시 확인하십시오.** 메뉴를 닫은 뒤 커서를 독 밖으로 옮기면 독이 정상적으로 사라져야 합니다.

### 3-2. 핀 판정의 실제 입력을 남긴다 (필수, 계측)

`SameDockPin()`이 **거짓을 돌려준 그 호출**에서, 실제로 비교한 값을 남기십시오. 밖에서 재구성하지 말고 함수 안에서 남겨야 합니다.

- 인자 `a`와 `b`의 원본 문자열. 눈에 보이지 않는 문자를 확인해야 하므로 **길이도 함께** 적으십시오.
- `\t`가 포함되어 있으면 그 위치를 적으십시오. 문자열에 제어 문자가 있으면 `<TAB>` 같은 표시로 바꿔 출력하십시오.
- 어느 분기를 탔는지. `both-aumid`, `mixed-aumid`, `path` 중 하나로 적으십시오.
- `path` 분기라면 `CanonicalPath(a)`와 `CanonicalPath(b)`의 결과와 길이를 적으십시오.

```cpp
Log(L"dock", L"pin cmp branch=%s a=[%s](%zu) b=[%s](%zu) ca=[%s](%zu) cb=[%s](%zu)", ...);
```

로그가 넘치지 않도록 다음을 지키십시오.

- **거짓을 돌려줄 때만** 남깁니다.
- 한 번의 `Rebuild`당 최대 5줄로 제한합니다.
- 정적 전역 카운터를 쓰되, `Rebuild` 시작 시 0으로 되돌립니다.

이 한 줄이 2.2의 세 가지 가능성 중 어느 것인지 가려 줍니다.

### 3-3. 폭주 계측을 유지한다 (필수)

`msg flood` 집계와 게시 메시지 계수기를 **그대로 두십시오.** 이번에 정체를 밝힌 장치이고, 앞으로 같은 종류가 재발하면 즉시 드러납니다.

임계값 500/s도 그대로 두십시오.

---

## 4. 하지 말아야 할 것

- 2장의 핀 매칭을 추측해서 고치지 마십시오. 3-2는 계측입니다.
- 지금까지의 수정을 하나도 되돌리지 마십시오. 트레이 억제, 굶주림 방지, `armed_`, 감시 스레드, 빈 경로 캐시 방지, 메시지 계측을 모두 유지합니다.
- `ArmMouseLeave()` 함수 자체를 삭제하지 마십시오. `WM_MOUSEMOVE`에서 씁니다.
- `PointerOverUi()`의 `Busy()` 검사를 빼지 마십시오. 메뉴가 열린 동안 독이 사라지는 것을 막는 장치입니다.
- `msg flood` 계측을 제거하거나 임계값을 올리지 마십시오.
- `WH_MOUSE_LL`, `WH_KEYBOARD_LL`을 도입하지 마십시오.

---

## 5. 검증

**폭주 (핵심)**
- [ ] 메뉴를 열고 커서를 메뉴 위로 올린 채 **10초간** 둔다. 그동안 CPU 점유가 0.1 코어 미만이다.

  ```powershell
  $p = Get-Process bamti; $c=$p.TotalProcessorTime.TotalSeconds; Start-Sleep 10; $p.Refresh()
  "{0:N2} cores" -f (($p.TotalProcessorTime.TotalSeconds-$c)/10)
  ```
- [ ] `[perf] msg flood` 줄이 더 이상 남지 않는다.
- [ ] 메뉴를 스무 번 여닫아도 CPU가 오르지 않는다.

**독 숨김 회귀 (이번 수정으로 깨지기 쉬운 지점)**
- [ ] 메뉴를 닫고 커서를 독 밖으로 옮기면 독이 사라진다.
- [ ] 커서를 독 위에 올리면 독이 계속 보인다.
- [ ] 메뉴가 열려 있는 동안에는 커서를 독 밖으로 옮겨도 독이 사라지지 않는다.
- [ ] 커서를 화면 아래 가장자리에 올리면 독이 나타난다.

**핀**
- [ ] 탐색기 아이콘이 맨 오른쪽으로 가는 상황을 재현하고 `[dock] pin cmp ...` 줄 전체를 보고한다.
- [ ] `branch=` 값과 두 문자열의 길이를 보고한다.

**회귀**
- [ ] 메뉴가 열리고 닫히고 항목 실행이 된다.
- [ ] `watchdog.log`에 `alive stage=idle ping=ok`만 쌓인다.
- [ ] 유휴 60초 누적 CPU가 1초 미만이다.
- [ ] Release 빌드가 `/W4` 경고 없이 통과한다.

---

## 6. 커밋

두 개로 나누십시오.

```
fix: 커서가 나간 창에 마우스 추적을 다시 걸지 않는다
```
3-1입니다. 본문에 `msg flood 57971/s top=[bamti.Dock:0x02A3 x57963]` 측정값과, 수정 후 같은 조건에서 잰 CPU 값을 적으십시오.

```
chore: 핀 비교의 실제 입력을 기록한다
```
3-2입니다.
