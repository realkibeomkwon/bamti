# 조사: 트레이 재등록 응답 문제

> 조사 당시의 측정 기록입니다. 현재 코드의 설명이 아닙니다.

조사 일시: 2026-08-29
대상: `TASK-TRAY-INTERCEPT.md`로 구현한 가로채기 백엔드가 서드파티 아이콘 여덟 개 가운데 넷만 받는 현상
비교 대상: Seelen UI(MIT, `eythaann/Seelen-UI`) 커밋 `4216b3e1a3`의 `src/background/modules/system_tray/`

---

## 질문

가로채기를 켰을 때 미러에 잡힌 항목이 넷뿐이었습니다. UIA 백엔드는 열둘을 봅니다. 왜 일부만 오는지, 그리고 Seelen UI는 전부 받는 방법을 찾았는지 확인했습니다.

## 결론

**Seelen UI도 Windows 11에서는 전부 받지 못합니다.** 우리와 같은 한계선에 서 있습니다. 다만 Windows 10에서는 다른 경로로 전부 얻으며, 그 경로는 이 컴퓨터에 존재하지 않습니다.

숫자도 다시 봐야 합니다. UIA가 세던 열둘은 시스템 아이콘 넷과 서드파티 여덟입니다. 시스템 아이콘은 구조적으로 가로채기 대상이 아니므로, 실제 응답률은 열둘 중 넷이 아니라 **여덟 중 넷**입니다.

---

## 1. 재등록은 앱이 스스로 구현해야 하는 규약이다

Windows가 자동으로 처리하지 않습니다. 앱이 직접 다음을 구현해 두어야 합니다.

```c
UINT g_taskbar_created = RegisterWindowMessageW(L"TaskbarCreated");
// WndProc 안에서
if (msg == g_taskbar_created) {
    Shell_NotifyIconW(NIM_ADD, &nid);   // 스스로 다시 등록한다
}
```

구현하지 않은 앱이 흔합니다. explorer를 강제 종료했다가 되살렸을 때 트레이 아이콘이 영영 돌아오지 않는 앱들이 정확히 이 경우입니다. 우리가 보내는 브로드캐스트도 그런 앱에게는 아무 의미가 없습니다.

## 2. 시스템 아이콘은 이 경로로 오지 않는다

네트워크, 볼륨, 배터리, 입력 표시기는 `Shell_NotifyIcon`으로 등록된 아이콘이 아닙니다. explorer가 자기 안에서 직접 그리는 XAML 요소입니다. 앱이 보내는 메시지가 존재하지 않으므로 가로채기로는 받을 수 없습니다.

이것은 우회할 수 있는 결함이 아니라 방식의 경계입니다. 시스템 아이콘을 상단바에 두려면 UIA 열거나 bamti 자체 위젯(4단계에서 만든 배터리·CPU·네트워크)을 써야 합니다.

2026-08-31 관찰: 가로채기 로그에 `exe=explorer.exe`인 `배터리 수준`, `오디오 서비스가 실행되고 있지 않습니다.`, `Bluetooth 장치`가 `Shell_NotifyIcon` 경로로 들어왔습니다. UIA 쪽 툴팁은 가로채기와 다릅니다. 화면의 XAML 시스템 아이콘과 같은 것인지는 단정하지 않습니다. 같은 날 후속: 이 경로로 그림과 툴팁은 오지만, 볼륨과 배터리는 콜백을 보내도 활성화되지 않습니다. 클릭은 explorer의 XAML 트리가 처리합니다. 블루투스만 콜백으로 열립니다.

## 3. 브로드캐스트가 닿지 않는 경우

앱이 재등록을 구현했더라도 메시지가 도달하지 않을 수 있습니다.

**메시지 전용 창.** `HWND_BROADCAST`는 최상위 창에만 전달됩니다. 트레이 아이콘 등록용으로 만든 보이지 않는 창을 `HWND_MESSAGE`를 부모로 삼아 만든 앱은 이 메시지를 받지 못합니다. 트레이 전용 더미 창을 그렇게 만드는 앱이 있습니다.

**UIPI.** 관리자 권한으로 실행된 앱은 일반 권한인 우리가 보낸 브로드캐스트를 받지 못합니다. 이 경계는 `ChangeWindowMessageFilterEx`로도 넘지 못합니다.

## 4. 우리 쪽 타이밍 문제

로그에서 확인된 사실입니다.

```
13:50:20.600  backend=intercept          스파이 창 생성과 브로드캐스트
13:50:20.616~619  items +1 ×4            재등록 넷 도착
13:50:21.603  intercept z-order lost     1초 뒤 우선순위 상실 감지
13:50:21.604  intercept z-order restored
```

브로드캐스트를 보낸 직후 1초 안에 `FindWindowW` 우선순위를 잃었습니다. 확인 주기가 1초이므로 **실제로 잃은 시각은 그보다 앞설 수 있고**, 그 사이에 재등록한 앱의 메시지는 우리를 건너뛰고 explorer로 곧장 갔을 것입니다. 앱마다 재등록 반응 속도가 다르므로 느린 앱일수록 놓쳤을 가능성이 큽니다.

세 원인 가운데 **이것만이 우리가 고칠 수 있는 부분입니다.**

---

## 5. Seelen UI는 어떻게 하는가

### 5-1. 재등록 유도는 우리와 동일하다

`tray_spy.rs`의 `refresh_icons`가 전부입니다.

```rust
/// Refreshes the icons of the tray.
///
/// Simulates the Windows taskbar being re-created. Some windows fail to
/// re-add their icons, in which case it's an implementation error on
/// their side. These windows that fail also do not re-add their icons
/// to the Windows taskbar when `explorer.exe` is restarted ordinarily.
fn refresh_icons() -> crate::Result<()> {
    let msg = unsafe { RegisterWindowMessageW(w!("TaskbarCreated")) };
    unsafe { SendNotifyMessageW(HWND_BROADCAST, msg, WPARAM::default(), LPARAM::default()) }?;
    Ok(())
}
```

브로드캐스트 한 줄이고 그 이상은 없습니다. 주석은 일부 앱이 재등록에 실패한다는 사실을 인정하면서, 그것을 앱 쪽 구현 오류로 규정하고 해결하지 않습니다. 근거로 드는 것도 우리와 같은 진단입니다. 그런 앱은 explorer를 정상 재시작해도 마찬가지로 돌아오지 않는다는 것입니다.

레지스트리로 목록을 보완하지도 않습니다. 트레이 모듈 소스 전체에 `NotifyIconSettings`를 포함한 레지스트리 접근 코드가 한 줄도 없습니다.

### 5-2. Windows 10에서는 다른 경로가 있다

`initial_tray_icons`가 기존 아이콘을 전부 얻으려고 시도합니다.

```rust
let toolbars = [
    Util::find_tray_toolbar_window(tray),
    Util::find_overflow_toolbar_window(),
];
let tray_process = unsafe { OpenProcess(PROCESS_ALL_ACCESS, false, process_id) }?;
let buffer = unsafe { VirtualAllocEx(tray_process, None, size_of::<TBBUTTON>(), MEM_COMMIT, ...) };
```

explorer 프로세스를 `PROCESS_ALL_ACCESS`로 열고 그 안에 메모리를 할당한 뒤, `TB_GETBUTTON`으로 레거시 툴바를 통째로 읽습니다. 재등록에 의존하지 않으므로 전부 얻습니다.

그런데 이 경로는 `ToolbarWindow32`를 요구합니다. 그들 소스의 주석이 한계를 명시합니다.

```rust
/// Response from `ToolbarWindow32` with `TB_GETBUTTON` message.
/// Only available on Windows 10, since tray windows are XAML islands in Windows 11.
```

실행 시 로그도 같습니다. `"Failed to retrieve initial tray icons. This is expected on W11."`

우리 1단계 탐침(`PROBE-TRAY.md`)에서 이 컴퓨터에 `ToolbarWindow32`가 존재하지 않고 검증 통과 버튼이 0개임을 확인했습니다. 두 관측이 일치합니다.

### 5-3. 비교표

| 항목 | Windows 10 | Windows 11 |
|---|---|---|
| 기존 아이콘 전부 확보 | 레거시 툴바를 explorer 메모리에서 읽어 얻는다 | **불가능하다.** 툴바 자체가 없다 |
| 새로 등록되는 아이콘 | `WM_COPYDATA` 가로채기 | 같다 |
| 재등록에 응답하지 않는 앱 | 위 경로가 덮는다 | **덮지 못한다** |

Seelen UI의 이슈 #282("일부 앱이 아이콘 없이 표시된다")가 이 증상의 사용자 보고입니다.

### 5-4. 우리가 쓸 수 없는 경로다

5-2절의 방식은 explorer 프로세스를 `PROCESS_ALL_ACCESS`로 열고 그 주소 공간에 메모리를 할당합니다. Seelen UI 유지보수자가 2025-09-26에 트레이 모듈을 걷어내면서 "멀웨어가 쓰는 것과 동일한 기법"이라고 지목한 것이 이 부분으로 보입니다.

우리는 이 경로를 선택지에 두지 않습니다. 두 가지 이유입니다. 첫째로 이 컴퓨터에 레거시 툴바가 없어서 애초에 해당 사항이 없습니다. 둘째로 `TASK-TRAY-INTERCEPT.md` 11절이 explorer 메모리 접근을 금지하고 있습니다.

---

## 6. 남은 여지

Seelen UI를 참고해도 이 문제는 풀리지 않습니다. 우리 쪽에 아직 시도하지 않은 것이 둘 있습니다.

### 6-1. z-order 확보 타이밍 (먼저 할 것)

4절에서 확인한 문제입니다. 브로드캐스트를 보내기 전에 우선순위를 확실히 잡고, 브로드캐스트 이후 몇 초 동안은 확인 주기를 100ms로 촘촘하게 올렸다가 원래대로 되돌립니다. 응답률이 실제로 오르는지는 측정으로 확인합니다.

비용이 작고 부작용이 없으므로 이것을 먼저 합니다.

`TASK-TRAY-PREEMPT.md`로 다음을 넣었습니다.

- 설정이 `intercept`이면 메뉴 바 창보다 먼저 스파이를 세웁니다.
- 브로드캐스트 전에 `FindWindowW`가 스파이를 돌려줄 때까지 최대 500ms 기다립니다.
- 브로드캐스트 직후와 우선순위 상실 때 100ms 주기로 5초 동안 확인합니다.
- explorer 재시작 때 우선순위를 다시 잡고, 잡혔을 때만 재브로드캐스트합니다. 2초 억제와 60초 3회로 루프를 막습니다.
- bamti 자신을 `HKCU\Run`의 `bamti` 값으로 로그온 시작 항목에 올립니다.

#### 측정 방법

응답률은 `intercept item` 줄을 세어서 구하면 안 됩니다. 그 줄은 앱이 재등록할 때마다 다시 기록되고 같은 아이콘이 여러 번 나오므로, 세션 동안 누적된 횟수이지 현재 잡고 있는 항목 수가 아닙니다. 대신 기동 3분 뒤에 한 번 남는 `roster` 줄의 `n=` 값과 `tips=` 목록을 봅니다. 이 줄은 한 시점의 스냅숏이라 두 백엔드를 그대로 대조할 수 있습니다.

```
[tray] intercept roster n=<개수> tips="<툴팁 목록>"
[tray] uia       roster n=<개수> tips="<툴팁 목록>"
```

판정은 개수 비교가 아니라 툴팁 대조로 합니다. `uia roster`의 항목에서 explorer가 자기 XAML로 그리는 시스템 아이콘(숨겨진 아이콘 표시, 입력 표시기, 네트워크, 볼륨, 배터리, 시계, 알림, 바탕 화면 보기)을 빼면 서드파티 목록이 남고, 그 목록이 `intercept roster`에 모두 들어 있으면 손실이 0입니다. 2절에서 적었듯이 시스템 아이콘은 가로채기 대상이 아니므로 분모에서 제외합니다.

#### 재부팅 후 측정 (2026-08-31)

재부팅 시각은 20:54:57이고, 로그온 시작 항목으로 자동 기동한 세션은 20:56:04입니다. 값은 `%USERPROFILE%\.bamti\bamti.log.old`의 15115~15209행에 있습니다.

| 항목 | 값 |
|---|---|
| 선기동 `prestart elapsed_ms` | 0 |
| 우선순위 확보 | `acquired ms=0` |
| `intercept roster n=` | 7 |
| `uia roster n=` | 11 |
| **서드파티 손실** | **0** |

로스터 내용은 다음과 같습니다.

```
20:59:04 intercept roster n=7  오피스키퍼|Everything|Bluetooth 장치|(no tip)|
                               DELL U4025QW: 100%|Tailscale|KakaoTalk
20:59:04 uia       roster n=11 숨겨진 아이콘 표시|KakaoTalk|Tailscale|Bluetooth 장치|
                               트레이 입력 표시기|네트워크|볼륨|배터리|시계|알림|바탕 화면 보기
```

UIA 열한 개에서 시스템 아이콘 여덟 개를 빼면 서드파티는 KakaoTalk, Tailscale, Bluetooth 장치 셋이고, 가로채기가 이 셋을 모두 받았습니다. 가로채기는 여기에 더해 오버플로에 들어가 있어 UIA가 보지 못한 Everything과 오피스키퍼, 그리고 explorer가 `Shell_NotifyIcon`으로 등록하는 DELL U4025QW와 툴팁 없는 항목까지 받았습니다.

같은 재부팅 안의 다른 세션들도 결과가 같습니다. 21:26 기동은 `intercept roster n=7` 대 `uia roster n=12`였고, 23:05 기동도 서드파티 넷(KakaoTalk, Tailscale, Everything, 오피스키퍼)을 모두 받았습니다.

이 결과는 결론 절에 적은 "여덟 중 넷"과 다릅니다. 그 숫자는 `TASK-TRAY-PREEMPT.md` 적용 전인 2026-08-29 측정이고, 당시 시스템 아이콘을 넷으로 잡아 서드파티를 여덟으로 셌습니다. 지금 로스터로 다시 세면 이 컴퓨터의 시스템 아이콘은 여덟이고 서드파티는 넷입니다. 분모 자체가 잘못 잡혀 있었으므로 두 측정을 직접 비교하지는 않습니다. 확실한 것은 선점을 적용한 뒤 재부팅 자동 기동에서 서드파티 손실이 관측되지 않았다는 사실입니다.

#### 남은 한계

`(no tip)` 항목이 로스터에 남습니다. 툴팁이 비어 있어 UIA 쪽 항목과 짝지을 수 없으므로 6-3절의 진단 로그로도 정체를 가리지 못합니다. 표시에는 지장이 없어 그대로 둡니다.

### 6-2. UIA와의 하이브리드

가로채기로 받은 항목은 진짜 아이콘으로, 받지 못한 항목은 UIA의 글리프로 채웁니다. 아이콘이 아예 사라지는 상황은 없어집니다.

문제는 두 소스의 항목을 짝지을 안정적인 키가 없다는 점입니다. 가로채기는 `GUID` 또는 `(hwnd, uid)`를, UIA는 `RuntimeId`를 씁니다. 둘을 잇는 값이 없어서 툴팁 문자열로 맞춰야 하는데, 같은 툴팁을 쓰는 아이콘이 있으면 오매칭이 생깁니다.

6-1절을 먼저 적용해 응답률을 측정한 뒤, 그래도 부족할 때 착수합니다.

### 6-3. 진단 로그

지금 로그로는 어떤 앱이 응답했고 어떤 앱이 응답하지 않았는지 가릴 수 없습니다. `TASK-TRAY-MIRROR.md` 10절이 "개별 항목의 툴팁은 적지 않습니다"로 정한 결과입니다.

가로채기가 받은 항목의 툴팁을 진단 목적으로 한 번만 남기면, UIA가 보는 여덟 개와 대조해 원인을 1절(구현하지 않음)과 3절(도달하지 않음)로 나눌 수 있습니다. 4절의 타이밍 문제는 6-1절을 적용한 뒤 개수가 늘어나는지로 판별됩니다.

---

## 출처

- `eythaann/Seelen-UI` 커밋 `4216b3e1a3`(2025-11-08) `src/background/modules/system_tray/application/tray_spy.rs`, `util.rs`
- `eythaann/Seelen-UI` 커밋 `5dc081a9a2`(2025-09-26) 트레이 모듈 제거
- Seelen-UI 이슈 #282, #1037
- `PROBE-TRAY.md` (2026-08-28, 이 컴퓨터의 `ToolbarWindow32` 부재 확인)
- `%USERPROFILE%\.bamti\bamti.log` 2026-08-29 13:50 구간
