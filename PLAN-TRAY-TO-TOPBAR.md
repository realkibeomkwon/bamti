# bamti: 트레이를 상단바로 옮기는 작업 계획

이 문서는 Windows 알림 영역(트레이)의 역할을 상단바로 옮기고, CodexBar와 동등한 LLM 사용량 표시를 Windows에서 구현하기 위한 구현 지시서입니다. 구현자가 그대로 따라갈 수 있도록 파일, 자료 구조, 프로토콜, 판정 기준을 명시합니다.

상위 문서인 `ARCHITECTURE.md`의 설계 원칙을 그대로 따릅니다. 다만 8절(Status Item 프로토콜)과 9절(기존 앱과 위젯)의 일부 판단은 이 문서가 갱신합니다. 갱신 내용은 12절에 정리했습니다.

---

## 0. 목표와 결론

사용자가 제시한 목표 조건은 네 가지입니다.

| 번호 | 조건 | 이 계획의 답 |
|---|---|---|
| 1 | 구현 후에도 극한으로 가볍고 빠른 성능을 유지한다 | 상주 프로세스는 렌더링과 입력만 담당합니다. 네트워크 통신, 자격 증명 해석, JSON 파싱 같은 무거운 작업은 전부 별도 프로세스나 작업자 스레드로 밀어냅니다. 5절에 측정 가능한 예산을 둡니다. |
| 2 | 기존 Windows 트레이 기능을 계속 사용할 수 있어야 한다 | 트레이를 가로채지 않고 **미러링**합니다. explorer의 알림 영역은 그대로 살아 있고, bamti는 그 내용을 읽어서 상단바에 다시 그리며 클릭을 원래 주인에게 전달합니다. |
| 3 | Windows 위젯도 상단바에 추가할 수 있어야 한다 | Windows 11 위젯 보드 자체는 임베드하지 않습니다(공개 호스트 API가 없습니다). 대신 위젯을 **Status Item 프로토콜의 공급자**로 재정의하고, 내장 위젯(배터리, CPU, 네트워크, 날씨)과 외부 위젯을 같은 규격으로 받습니다. 위젯 보드는 여는 버튼만 제공합니다. |
| 4 | LLM별 실시간 사용량을 상단바에 표시한다 | CodexBar의 공급자 로직을 이식한 별도 헬퍼 프로세스(`bamti-usage.exe`)가 사용량을 조회해 파이프로 밀어 넣습니다. bamti 본체는 그리기만 합니다. |

가장 중요한 판단부터 말씀드립니다. **상단바에 무언가를 표시하는 경로는 단 하나여야 합니다.** 트레이 미러, LLM 사용량, 비트코인 시세, 배터리, 토글 스위치가 각각 별도의 렌더링 경로를 가지면, 레이아웃과 오버플로와 팝업 동작을 네 벌 유지하게 되고 성능 예산도 무너집니다. 따라서 이 계획의 중심은 **Status Item 프로토콜 v2**이며, 나머지 기능은 전부 이 프로토콜의 공급자로 구현합니다.

---

## 1. 현재 코드의 상태

이미 구현되어 있어 재사용할 것은 다음과 같습니다.

| 자산 | 위치 | 이 작업에서의 역할 |
|---|---|---|
| 상단바 창, AppBar 등록, 전체화면 회피 | `menu_bar.cpp` | 그대로 씁니다. |
| Direct2D 바 렌더링, 히트 테스트 산출 | `clock_renderer.cpp` `Draw()` | 세그먼트 모델 기반으로 재구성합니다(2단계). |
| 캡처 기반 팝업 표면 | `popup_surface.{hpp,cpp}` | 상태 패널과 오버플로 팝업이 그대로 씁니다. 새로 만들지 않습니다. |
| Named pipe 서버, NDJSON 파서 | `pipe_server.cpp`, `json_line.hpp` | v2 스키마로 확장합니다(3단계). |
| StatusItem, StatusPanel, StatusGauge | `status_item.hpp` | v2 모델로 확장합니다. |
| 태스크바 숨김과 복구, 트레이 창 감시 | `taskbar_controller.cpp` | 미러 백엔드가 요구하는 "숨김 대신 화면 밖 주차" 모드를 추가합니다(5단계). |
| HICON을 WIC로 D2D 비트맵으로 바꾸는 코드 | `dock.cpp:416`, `dock.cpp:2166` 부근 | 공용 `icon_cache`로 추출해 바와 독이 함께 씁니다. |
| 워치독 | `watchdog.cpp` | 헬퍼 프로세스 감시에 확장 적용합니다. |

비어 있는 것은 다음과 같습니다.

- 바 세그먼트 모델이 없습니다. `ClockRenderer::Draw()`가 시작 버튼, 상태 항목, 시계를 한 함수에서 계산하고 그립니다. 항목이 늘어나면 오버플로와 부분 무효화를 넣을 자리가 없습니다.
- 아이콘이 유니코드 글리프만 가능합니다(`icon_glyph`, 최대 8자). 비트맵 아이콘 경로가 없습니다.
- 프로토콜에 토글, 값 표시, 이벤트 역방향 전달, 능력 협상이 없습니다.
- 트레이 미러가 없습니다.
- 사용량 공급자가 없습니다.

---

## 2. 설계 판단

### 판단 1. 렌더링은 bamti가 독점하고, 데이터만 외부에서 받는다

외부 공급자가 bamti 프로세스 안에서 코드를 실행하지 못하게 합니다. 플러그인 DLL 방식은 상주 프로세스의 안정성과 성능을 공급자 품질에 종속시키므로 채택하지 않습니다. 공급자는 선언적인 JSON만 보내고, 그리기는 전부 bamti의 Direct2D 경로가 수행합니다.

이 판단의 이득은 세 가지입니다. 첫째로 어떤 공급자가 죽어도 바는 살아 있습니다. 둘째로 모든 항목의 글꼴, 간격, 다크 테마, DPI 대응이 자동으로 일치합니다. 셋째로 성능 예산을 bamti가 단독으로 통제할 수 있습니다.

### 판단 2. 트레이는 가로채지 않고 미러링한다

트레이 아이콘을 흡수하는 방법은 원리상 두 가지입니다.

1. **가로채기**: `Shell_TrayWnd` 클래스로 창을 만들어 `Shell_NotifyIcon`이 보내는 `WM_COPYDATA`를 대신 받습니다. 데이터 충실도가 가장 높지만, 어느 창이 선택되는지가 `FindWindowW`의 열거 순서에 달려 있어 결정적이지 않습니다. 앱마다 explorer에 붙기도 하고 bamti에 붙기도 하는 상태가 되면 목표 조건 2를 정면으로 위반합니다.
2. **미러링**: explorer의 알림 영역을 읽기 전용으로 관찰해 상단바에 다시 그리고, 클릭은 원래 주인에게 전달합니다. explorer의 트레이는 조금도 건드리지 않으므로 목표 조건 2가 구조적으로 보장됩니다.

**미러링을 채택합니다.** 가로채기는 설정으로 숨긴 실험 기능으로만 남기고 기본값은 꺼 둡니다(5-6절).

미러링의 데이터 원천은 Windows 빌드에 따라 다르므로 백엔드를 두 개 두고 시작 시 탐지합니다. 어떤 백엔드가 이 컴퓨터(Windows 11 빌드 26200)에서 실제로 동작하는지는 **추측하지 말고 4단계의 탐침 도구로 측정한 뒤** 결정합니다.

### 판단 3. LLM 사용량은 별도 프로세스가 담당한다

사용량 조회는 HTTPS 요청, 토큰 만료 처리, 재시도, JSON 파싱을 포함합니다. 이 코드를 상주 UI 프로세스에 넣으면 다음 문제가 생깁니다. 네트워크 지연이 UI 스레드에 닿을 위험이 생기고, 공급자 API가 바뀔 때마다 셸 본체를 다시 배포해야 하며, 상주 작업 집합이 늘어납니다.

따라서 `bamti-usage.exe`를 별도 타깃으로 만들고, bamti가 자식 프로세스로 띄워 감시합니다. 헬퍼가 죽어도 바는 영향을 받지 않고, 해당 항목만 사라집니다.

### 판단 4. Windows 11 위젯 보드는 임베드하지 않는다

위젯 보드는 별도 프로세스가 소유하는 WebView 기반 표면이며, 이를 다른 프로세스의 창 안에 넣는 공개 API가 없습니다. 창을 리페어런팅하는 방식은 누적 업데이트마다 깨지고 `ARCHITECTURE.md` 2절의 원칙에 어긋납니다.

대신 세 가지를 제공합니다.

1. 위젯 보드를 여는 상단바 버튼을 둡니다(`Win+W` 상당).
2. 사용자가 실제로 원하는 위젯 값(배터리, CPU, 네트워크, 날씨, 시세)은 내장 공급자와 외부 공급자로 제공합니다.
3. 후순위로 bamti가 Windows 위젯 **공급자**가 되는 방향은 열어 둡니다. 이는 호스팅과 반대 방향이며 공개 API가 존재합니다.

---

## 3. Status Item 프로토콜 v2

이 절이 이 계획의 핵심 산출물입니다. "아이콘과 값", "아이콘과 토글", "아이콘과 팝업 패널"을 하나의 규격으로 표현합니다. 구현 후에는 `docs/STATUS-PROTOCOL.md`로 복사해 공개 명세로 유지합니다.

### 3.1 원칙

1. **의미를 기술하고 배치를 기술하지 않습니다.** 공급자는 "값이 0.62이고 경고 상태"라고 말하고, 픽셀 좌표나 색상 코드를 지시하지 않습니다. 색은 `accent`로만 제안할 수 있으며, 테마 대비가 부족하면 bamti가 조정합니다.
2. **모르는 필드는 무시합니다.** 앞으로 추가될 필드를 위해 파서는 미지의 키를 오류로 처리하지 않습니다.
3. **모르는 행 종류는 건너뜁니다.** 각 패널 행은 `fallback_text`를 가질 수 있고, 렌더러가 해당 `type`을 모르면 `fallback_text`를 일반 텍스트 행으로 그립니다. 없으면 행 자체를 생략하되 패널은 정상적으로 엽니다.
4. **v1 클라이언트는 계속 동작합니다.** 기존 `{"v":1,"op":"upsert","id":...,"text":...}` 형태는 내부에서 v2 모델로 변환합니다. `examples/status_push.py`의 기존 예제가 수정 없이 작동해야 합니다.

### 3.2 연결과 능력 협상

전송 계층은 기존과 같습니다. 사용자 세션 로컬 named pipe `\.\pipe\bamti-status`, UTF-8 NDJSON, 한 줄에 레코드 하나입니다.

연결 직후 bamti가 먼저 한 줄을 보냅니다.

```json
{"v":2,"op":"hello","renderer":"bamti","version":"1.2.0","proto":[1,2],
 "features":["icon_glyph","icon_png","gauge","kv","toggle","button","text","separator","events"],
 "limits":{"segment_text":32,"panel_rows":32,"panel_text":128,"icon_png_bytes":8192,"upserts_per_sec":10}}
```

클라이언트는 자신이 쓸 버전을 `v`에 담아 보내기만 하면 됩니다. `hello`를 읽지 않고 바로 `upsert`를 보내도 동작해야 합니다. `features`에 없는 기능을 쓴 경우 bamti는 조용히 무시하고, 로그에 한 번만 기록합니다.

### 3.3 클라이언트에서 bamti로 보내는 연산

| op | 뜻 |
|---|---|
| `upsert` | 항목 전체를 갱신합니다. 같은 `id`는 덮어씁니다. |
| `patch` | 항목의 일부만 갱신합니다. `segment.text`처럼 자주 바뀌는 값만 보낼 때 씁니다. 존재하지 않는 `id`면 무시합니다. |
| `remove` | 항목을 제거합니다. |
| `ping` | 하트비트입니다. 아무 상태도 바꾸지 않습니다. |

`id`는 `벤더/앱/항목` 형태의 역방향 도메인 접두 문자열을 권장합니다(예: `dev.steipete.codexbar/codex`). 다른 클라이언트가 소유한 `id`는 덮어쓸 수 없습니다. 이 규칙은 현재 코드에 이미 있으므로 유지합니다.

### 3.4 항목 스키마

```json
{
  "v": 2,
  "op": "upsert",
  "id": "dev.steipete.codexbar/codex",
  "segment": {
    "icon":     {"kind": "glyph", "glyph": "◐"},
    "text":     "10%",
    "tooltip":  "Codex · Session 6% · Weekly 10%",
    "state":    "normal",
    "accent":   "#FF7A45",
    "priority": 10,
    "visible":  true
  },
  "panel": {
    "title":    "Codex",
    "subtitle": "Prolite",
    "updated":  "Updated 13s ago",
    "rows": [
      {"type":"gauge","label":"Session (5h) — Codex","value":0.06,"value_text":"6%",
       "detail":"Resets 18:31 · in 3h 38m"},
      {"type":"gauge","label":"Weekly — Codex","value":0.10,"value_text":"10%",
       "detail":"Resets 17 Jul 08:15 · in 6d 17h","note":"Ahead of pace"},
      {"type":"separator"},
      {"type":"kv","label":"Plan","value":"Pro"},
      {"type":"toggle","row_id":"auto_refresh","label":"Auto refresh","on":true},
      {"type":"button","row_id":"settings","label":"Settings…"},
      {"type":"button","row_id":"quit","label":"Quit","style":"danger"}
    ]
  }
}
```

#### segment

| 필드 | 형 | 뜻 |
|---|---|---|
| `icon` | 객체 (선택) | 3.5절 참고. 생략하면 텍스트만 그립니다. |
| `text` | 문자열 (선택) | 바에 그릴 짧은 값입니다. 32자를 넘으면 말줄임합니다. 생략하면 아이콘만 그립니다. |
| `tooltip` | 문자열 (선택) | 호버 툴팁입니다. |
| `state` | `normal` \| `warn` \| `error` \| `on` \| `off` \| `busy` | 렌더러가 색과 표시를 결정합니다. `warn`은 주의색, `error`는 경고색, `off`는 흐리게, `busy`는 갱신 중 표시입니다. |
| `accent` | 문자열 또는 정수 (선택) | `#RRGGBB` 또는 v1 호환의 정수입니다. `state`가 `normal`일 때만 적용합니다. |
| `priority` | 정수 (기본 0) | 큰 값이 시계에 가깝게 놓입니다. 폭이 부족하면 작은 값부터 오버플로로 접습니다. |
| `visible` | 불리언 (기본 true) | false면 항목을 유지한 채 바에서만 감춥니다. 오버플로 팝업에는 남습니다. |

`icon`과 `text`가 모두 비어 있으면 그 항목은 유효하지 않으므로 무시합니다.

#### panel

`panel`이 없으면 클릭 시 팝업을 열지 않고 `event`만 클라이언트에 보냅니다. 이 구분이 중요합니다. 자체 창을 띄우고 싶은 앱은 패널을 생략하면 됩니다.

`rows`는 최대 32개입니다. 행 종류는 다음과 같습니다.

| type | 필드 | 렌더링 |
|---|---|---|
| `gauge` | `label`, `value`(0.0~1.0), `value_text`(선택), `detail`(선택), `note`(선택) | 레이블과 우측 값, 그 아래 진행 바, 그 아래 상세와 비고 |
| `kv` | `label`, `value` | 좌측 레이블, 우측 값 |
| `text` | `text`, `style`(`body` \| `note`) | 한 줄 문단 |
| `separator` | 없음 | 얇은 구분선 |
| `toggle` | `row_id`, `label`, `on` | 좌측 레이블, 우측 스위치 |
| `button` | `row_id`, `label`, `style`(`normal` \| `danger`) | 버튼 |

`button` 행이 연속으로 오면 한 줄에 나란히 배치합니다(첨부 이미지의 `Settings…` / `Quit` 배치). 세 개를 넘으면 줄바꿈합니다.

### 3.5 아이콘

```json
{"kind":"glyph","glyph":"◐"}
{"kind":"png","data":"<base64>"}
{"kind":"file","path":"C:\Program Files\App\app.ico"}
```

- `glyph`: 최대 8 UTF-16 단위입니다. 바 글꼴과 Segoe Fluent Icons 대체 글꼴로 그립니다.
- `png`: base64 디코딩 후 8KB 이하, 64x64 픽셀 이하여야 합니다. 초과하면 아이콘 없이 텍스트만 그리고 로그에 남깁니다.
- `file`: `.ico`, `.png`를 받습니다. 경로는 클라이언트 프로세스가 아니라 bamti가 읽으므로, 읽을 수 없으면 아이콘을 생략합니다. 파일 감시는 하지 않습니다. 아이콘을 바꾸려면 다시 `upsert` 하십시오.

아이콘은 `(kind, 내용 해시)`를 키로 캐시합니다. 같은 아이콘을 매초 다시 보내도 디코딩은 한 번만 일어나야 합니다.

내부 공급자(트레이 미러)만 쓰는 `{"kind":"hicon","handle":...}` 형태가 하나 더 있습니다. 이는 프로세스 내부 전용이며 파이프로는 받지 않습니다.

### 3.6 bamti에서 클라이언트로 보내는 이벤트

```json
{"v":2,"op":"event","id":"...","event":"click","button":"left"}
{"v":2,"op":"event","id":"...","event":"click","button":"right"}
{"v":2,"op":"event","id":"...","event":"panel_open"}
{"v":2,"op":"event","id":"...","event":"panel_close"}
{"v":2,"op":"event","id":"...","event":"invoke","row_id":"settings"}
{"v":2,"op":"event","id":"...","event":"toggle","row_id":"auto_refresh","on":false}
```

`panel_open`은 클라이언트가 값을 즉시 새로 고칠 기회입니다. CodexBar가 메뉴를 열 때 갱신하는 동작을 이것으로 구현합니다.

### 3.7 토글의 낙관적 갱신

토글은 왕복 지연이 눈에 보이면 안 됩니다. 다음 규칙을 따릅니다.

1. 사용자가 스위치를 누르면 bamti가 **즉시** 시각 상태를 뒤집습니다.
2. `toggle` 이벤트를 보냅니다.
3. 2초 안에 해당 `id`의 `upsert` 또는 `patch`가 오지 않으면 시각 상태를 원래대로 되돌리고 항목을 `state:"error"`로 표시합니다.

### 3.8 속도 제한과 유효성

| 항목 | 상한 | 초과 시 |
|---|---|---|
| 항목당 갱신 빈도 | 초당 10회 | 마지막 값만 남기고 합칩니다. 다시 그리기는 다음 프레임에 한 번만 합니다. |
| 클라이언트당 항목 수 | 16개 | 새 `upsert`를 무시하고 로그에 남깁니다. |
| 전체 항목 수 | 64개 | 위와 같습니다. |
| 한 줄 길이 | 64KB | 연결을 끊습니다. |
| 하트비트 | 30초 | 무응답 시 항목을 제거합니다(`DropStale`이 이미 수행합니다). |

값 검증은 파서에서 끝냅니다. `value`는 0.0~1.0으로 클램프하고, NaN과 무한대는 0으로 바꾸며, 문자열은 전부 길이 상한으로 자릅니다. **UI 스레드에 도달하는 데이터는 이미 안전하다는 것이 불변식입니다.**

### 3.9 내부 공급자 인터페이스

파이프 클라이언트와 내장 위젯이 같은 경로를 쓰도록 다음 인터페이스를 둡니다. 이것이 없으면 트레이 미러와 배터리 위젯이 각자 렌더링 경로를 만들게 됩니다.

```cpp
// src/status_source.hpp
namespace bamti {

struct StatusEvent {
  std::string id;
  std::string event;    // "click" | "invoke" | "toggle" | "panel_open" | "panel_close"
  std::string row_id;
  std::string button;   // "left" | "right"
  bool on = false;
};

// 어느 스레드에서 호출해도 안전해야 한다. 내부에서 잠그고 UI에 PostMessage 한다.
class StatusSink {
 public:
  virtual ~StatusSink() = default;
  virtual void Upsert(StatusItem item) = 0;
  virtual void Remove(const std::string& id) = 0;
};

class StatusSource {
 public:
  virtual ~StatusSource() = default;
  virtual const char* Name() const = 0;
  virtual bool Start(StatusSink* sink) = 0;
  virtual void Stop() = 0;
  virtual void OnEvent(const StatusEvent& ev) {}
  // 전체화면 가림, 세션 잠금 등으로 갱신이 필요 없을 때 호출한다.
  virtual void SetActive(bool active) {}
};

}  // namespace bamti
```

`StatusRegistry`가 모든 공급자의 항목을 모아 `priority` 내림차순, 같으면 `id` 오름차순으로 정렬한 스냅숏을 제공합니다. `PipeServer`는 이 인터페이스를 구현하는 공급자 중 하나가 됩니다.

---

## 4. 단계별 구현

각 단계는 독립적으로 빌드되고 검증됩니다. 순서를 지켜 주십시오. 특히 2단계를 건너뛰고 5단계를 하면 레이아웃 코드가 다시 한 함수로 뭉칩니다.

### 1단계: 탐침 도구로 이 컴퓨터의 트레이 구조를 측정한다

**추측 금지.** Windows 11 최신 빌드에서 알림 영역의 내부 구조는 이전 세대와 다릅니다. 코드를 쓰기 전에 실제 값을 봅니다.

이 단계의 상세 지시서는 `TASK-TRAY-PROBE.md`에 따로 있습니다. 구현 시 그 문서를 따르고, 아래 요약은 무엇을 재는지에 대한 개요로만 읽으십시오.

`bamti.exe --probe-tray` 스위치를 추가하고, 결과를 표준 출력과 `%USERPROFILE%\.bamti\logs`에 남깁니다. 이 코드는 진단 전용이므로 `src/tray_probe.cpp`에 격리하고 상주 경로에서는 호출하지 않습니다.

측정 항목은 다음과 같습니다.

1. `Shell_TrayWnd`와 `Shell_SecondaryTrayWnd`의 자식 트리를 깊이 4까지 덤프합니다. 각 노드마다 클래스명, 창 제목, 스타일, 화면 사각형, 표시 여부를 남깁니다.
2. 트리에서 `ToolbarWindow32`를 찾고, 찾을 때마다 `TB_BUTTONCOUNT`를 보냅니다. 결과가 0보다 크면 레거시 백엔드가 가능합니다. `NotifyIconOverflowWindow`와 `TopLevelWindowForOverflowXamlIsland`도 최상위 창에서 각각 찾습니다.
3. 버튼이 있으면 `VirtualAllocEx`로 대상 프로세스에 버퍼를 잡고 `TB_GETBUTTON`, `TB_GETBUTTONTEXTW`를 보낸 뒤 `ReadProcessMemory`로 읽어 `TBBUTTON.dwData`가 가리키는 구조를 덤프합니다. `hWnd`, `uID`, `uCallbackMessage`, `hIcon`이 그럴듯한 값인지 확인합니다.
4. UI Automation으로 태스크바 아래의 `ToolBar` 및 `Button` 요소를 열거하고, 각 요소의 `Name`, `AutomationId`, `BoundingRectangle`, 지원 패턴(`Invoke`, `LegacyIAccessible`)을 남깁니다.
5. `PrintWindow(tray_hwnd, dc, PW_RENDERFULLCONTENT)`로 알림 영역을 캡처해 `%USERPROFILE%\.bamti\probe-tray.png`로 저장합니다. 태스크바가 화면 밖에 주차된 상태에서도 픽셀이 나오는지 확인하는 것이 목적입니다.

**이 단계의 산출물은 코드가 아니라 측정 결과입니다.** 결과를 `PROBE-TRAY.md`에 붙여 넣고, 그 결과로 5단계에서 **무엇을 구현할지** 결정합니다. 판정 표는 5-0절에 있습니다. 요약하면 3번이 성공하면 레거시 백엔드를 만들고, 실패하면 레거시 백엔드는 아예 만들지 않은 채 UIA 백엔드만 만들며, 4번과 5번까지 실패하면 5-5절의 축소 대안으로 갑니다.

**검증**: `--probe-tray`가 30초 안에 끝나고, 트레이 아이콘이 5개 이상 있는 상태에서 최소한 어떤 경로로 몇 개를 발견했는지 로그로 확인할 수 있습니다.

---

### 2단계: 바를 세그먼트 모델로 재구성한다

`ClockRenderer::Draw()`의 레이아웃 계산과 그리기를 분리합니다. 이 단계에서는 화면에 보이는 결과가 바뀌지 않아야 합니다.

#### 2-1. 새 파일

```
src/bar_layout.hpp / .cpp     레이아웃 계산 (그리기 없음)
```

`icon_cache`는 이 단계에서 만들지 않습니다. 지금 바에는 비트맵 아이콘 소비자가 없고, 트레이와 위젯 아이콘은 3단계와 5단계에 들어옵니다. 소비자가 없는 상태에서 `dock.cpp`의 아이콘 코드를 공용 모듈로 뽑으면 인터페이스가 어긋날 수 있으므로, 3단계에서 프로토콜 v2의 `icon` 필드를 그릴 때 함께 추출합니다.

```cpp
// src/bar_layout.hpp
enum class SegmentKind { kStart, kStatus, kTray, kOverflow, kClock };

struct BarSegment {
  SegmentKind kind = SegmentKind::kStatus;
  std::string id;            // kStatus, kTray에서만 의미가 있다
  const StatusItem* item = nullptr;  // 스냅숏 수명 동안만 유효하다
  RECT rect{};               // 클라이언트 좌표, 픽셀
  bool hot = false;
  bool pressed = false;
};

struct BarLayoutResult {
  std::vector<BarSegment> segments;
  std::vector<StatusItem> overflow;  // 폭이 부족해 접힌 항목
  RECT clock_rect{};
  RECT start_rect{};
};

BarLayoutResult LayoutBar(const RECT& client, UINT dpi, const std::wstring& clock_text,
                          const std::vector<StatusItem>& items);
```

레이아웃 규칙은 다음과 같습니다.

- 좌측에 시작 버튼을 둡니다(현재와 동일).
- 우측 끝에 시계를 둡니다(현재와 동일).
- 상태 항목은 시계 왼쪽으로 `priority` 내림차순으로 채웁니다.
- 남은 폭이 다음 항목을 담기에 부족하면 그 항목부터 `overflow`로 보내고, 오버플로 세그먼트(갈매기 모양 글리프)를 하나 둡니다. 오버플로 세그먼트는 항목이 하나라도 접혔을 때만 나타납니다.
- 좌측 절반은 앞으로 앱 메뉴가 들어갈 자리이므로 상태 항목을 놓지 않습니다.

#### 2-2. 부분 무효화

`MenuBar::Paint()`가 매번 바 전체를 다시 그리고 있습니다. 항목이 늘면 낭비이므로 다음으로 바꿉니다.

- 레이아웃 결과를 `MenuBar`가 보관합니다.
- 상태가 바뀌면 이전 레이아웃과 새 레이아웃을 비교해 **달라진 세그먼트의 사각형만** `InvalidateRect` 합니다. 세그먼트 개수나 위치가 바뀌면 전체를 무효화합니다.
- 시계는 1초마다 `clock_rect`만 무효화합니다.

#### 2-3. 다시 그리기 합치기

`kStatusChangedMsg`를 받으면 곧바로 그리지 말고 16ms 단발 타이머를 겁니다. 이미 걸려 있으면 아무것도 하지 않습니다. 타이머가 만료될 때 한 번만 무효화합니다. 이렇게 하면 공급자 다섯 개가 동시에 값을 밀어 넣어도 프레임은 하나만 그립니다.

**검증**
- [ ] 기존 화면과 픽셀 단위로 같습니다.
- [ ] 창 폭을 좁게 만든 가상 모니터에서 항목이 오버플로로 접히고, 갈매기를 누르면 팝업에 접힌 항목이 나옵니다.
- [ ] 시계만 바뀔 때 `Paint()`가 받는 `ps.rcPaint`가 시계 영역으로 한정됩니다(로그로 확인).

---

### 3단계: 프로토콜 v2와 공급자 레지스트리

이 단계에서 `icon_cache`를 추출합니다. 대상은 `dock.cpp`의 `WicFactory()`, `BitmapFromIcon()`, 그리고 `RenderLayered()` 안의 `d2d_icons_` 캐시입니다. 프로토콜 v2의 `icon` 필드가 첫 소비자입니다.

#### 3-1. 모델 확장

`status_item.hpp`를 3절 스키마에 맞게 확장합니다.

```cpp
enum class StatusState { kNormal, kWarn, kError, kOn, kOff, kBusy };
enum class IconKind { kNone, kGlyph, kPng, kFile, kHicon };

struct StatusIcon {
  IconKind kind = IconKind::kNone;
  std::wstring glyph;
  std::vector<uint8_t> bytes;   // kPng
  std::wstring path;            // kFile
  HICON hicon = nullptr;        // kHicon, 내부 전용
  uint64_t cache_key = 0;       // 내용 해시
};

enum class RowType { kGauge, kKeyValue, kText, kSeparator, kToggle, kButton };

struct StatusRow {
  RowType type = RowType::kText;
  std::string row_id;
  std::wstring label;
  std::wstring value_text;
  std::wstring detail;
  std::wstring note;
  std::wstring fallback_text;
  float value = 0.0f;
  bool on = false;
  bool danger = false;
};

struct StatusPanel {
  std::wstring title, subtitle, updated_text;
  std::vector<StatusRow> rows;
};

struct StatusItem {
  std::string id;
  std::string source;          // 공급자 이름. 충돌 판정에 쓴다
  StatusIcon icon;
  std::wstring text;
  std::wstring tooltip;
  StatusState state = StatusState::kNormal;
  uint32_t accent = 0;
  int priority = 0;
  bool visible = true;
  std::optional<StatusPanel> panel;
};
```

기존 `StatusGauge`와 `panel.actions`는 v1 파싱 경로에서 `StatusRow`로 변환합니다. 즉 모델은 하나만 남깁니다.

#### 3-2. 파서

`pipe_server.cpp`의 `HandleLine`을 `v` 값으로 분기합니다. v1 경로는 지금 코드를 그대로 두고, v2 경로를 새로 작성한 뒤 마지막에 같은 `StatusItem`으로 합류시킵니다. `json_line.hpp`에 필요한 것은 중첩 배열 순회와 실수 파싱이며, 이미 `ForEachArray`와 `GetRaw`가 있으므로 확장은 작습니다.

#### 3-3. 레지스트리

```
src/status_source.hpp     3-9절 인터페이스
src/status_registry.hpp / .cpp
```

`MenuBar`는 `PipeServer`를 직접 들고 있지 않고 `StatusRegistry`만 봅니다. 등록 순서는 `PipeServer`, `TrayMirror`, `BuiltinWidgets`입니다.

#### 3-4. 패널 렌더러 확장

`StatusPanelContent`(현재 `menu_bar.cpp` 안 익명 영역)를 `src/status_panel.cpp`로 옮기고 새 행 종류를 그립니다. 토글 스위치와 버튼의 히트 영역은 `PopupContent::HitTest`가 반환하는 인덱스로 구분하고, `Invoke(index)`에서 행 종류에 따라 `toggle` 또는 `invoke` 이벤트를 만듭니다. 토글은 3-7절의 낙관적 갱신을 적용합니다.

#### 3-5. 명세와 예제

- `docs/STATUS-PROTOCOL.md`에 3절 내용을 옮깁니다.
- `examples/status_push.py`에 v2 예제를 추가합니다. 기존 v1 예제는 회귀 검증용으로 남깁니다.
- `examples/ticker.py`를 새로 만듭니다. 임의의 값(예: 비트코인 시세)을 30초마다 갱신하고, 패널에 `kv` 행과 `toggle` 행 하나씩을 두는 30줄 내외의 예제입니다. 이 예제가 "아이콘 + 값", "아이콘 + 토글"이 같은 규격으로 가능한지 증명하는 회귀 시험입니다.

**검증**
- [ ] 기존 v1 예제가 수정 없이 그대로 표시됩니다.
- [ ] v2 예제의 게이지, 키값, 토글, 버튼이 모두 그려지고 눌립니다.
- [ ] 토글을 누르면 즉시 뒤집히고, 클라이언트를 죽인 채 누르면 2초 뒤 원래대로 돌아옵니다.
- [ ] 미지의 `type`을 가진 행을 보내면 패널이 열리고 그 행만 `fallback_text`로 나옵니다.
- [ ] 초당 200회 `patch`를 보내도 CPU 사용률이 1%를 넘지 않고 프레임이 초당 60회를 넘지 않습니다.

---

### 4단계: 내장 위젯 공급자

프로토콜을 밖에서만 쓰면 검증이 늦어집니다. bamti 자신이 첫 소비자가 되도록 내장 공급자를 만듭니다.

```
src/widgets/builtin.hpp / .cpp
```

| 위젯 | 자료 원천 | 갱신 |
|---|---|---|
| 배터리 | `GetSystemPowerStatus` | `WM_POWERBROADCAST` 수신 시, 폴백으로 60초 |
| CPU | `NtQuerySystemInformation`이 아니라 `GetSystemTimes` | 5초 |
| 네트워크 | `GetIfTable2` 바이트 카운터 차분 | 2초 |
| 위젯 보드 열기 | 클릭 시 `Win+W` 상당 동작 | 갱신 없음 |
| 날씨 | 없음. 5단계 헬퍼가 담당합니다 | 해당 없음 |

세 위젯 모두 하나의 작업자 스레드에서 대기 가능 타이머로 처리합니다. 스레드를 세 개 만들지 마십시오. 전체화면 가림 상태와 세션 잠금 상태에서는 `SetActive(false)`로 타이머를 멈춥니다.

각 위젯은 설정으로 개별로 끌 수 있어야 하고, **기본값은 전부 꺼짐**입니다. 성능 예산이 사용자 선택으로만 소비되도록 합니다.

**검증**
- [ ] 세 위젯을 모두 켠 상태로 10분간 유휴 CPU 사용률이 0.2% 미만입니다.
- [ ] 모두 끈 상태에서 작업자 스레드가 생성되지 않습니다.

---

### 5단계: 트레이 미러

이 단계가 목표 조건 2를 만족시키는 부분입니다. 원칙은 하나입니다. **explorer의 알림 영역에 쓰기를 하지 않습니다.** 읽기와 메시지 전달만 합니다.

#### 5-0. 백엔드 선택은 1단계 탐침 결과가 결정한다

**이 절을 읽지 않고 5-2절을 구현하지 마십시오.**

이 문서는 백엔드를 두 개 서술하지만, 두 개를 다 만들라는 뜻이 아닙니다. 무엇을 구현할지는 1단계 탐침의 측정 결과로 정합니다.

| 1단계 탐침 결과 | 구현할 것 |
|---|---|
| `ToolbarWindow32`의 `TB_BUTTONCOUNT`가 0보다 크고 `dwData` 구조가 검증을 통과한다 | 5-2절(레거시 백엔드)을 1순위로 구현합니다. UIA 백엔드는 좌클릭 전달(5-4절 1번)에만 쓰고, 열거용으로는 만들지 않습니다. |
| 버튼이 0개다 | **5-2절을 구현하지 않습니다.** `tray_backend_toolbar.cpp`를 만들지 말고 UIA 백엔드만 작성합니다. |
| UIA 열거도 실패하거나 `PrintWindow` 캡처가 검게 나온다 | 미러를 포기하고 5-5절의 축소 대안만 구현합니다. |

Windows 11 22H2 이후 알림 영역이 XAML 기반으로 바뀌면서 이 툴바가 비어 있다는 보고가 많습니다. 대상 컴퓨터의 빌드 26200에서는 **비어 있을 가능성이 높다고 예상합니다.** 그럼에도 탐침에 레거시 검사를 남기는 이유는 두 가지입니다. Windows 10과, 클래식 태스크바를 되살린 환경에서는 이 경로가 살아 있고, 그런 환경이 bamti 사용자층과 겹칠 수 있기 때문입니다.

**1단계 측정 결과 (`PROBE-TRAY.md`):** 이 컴퓨터에서 ToolbarWindow32는 없고 검증 통과 버튼은 0개다. UIA ControlView는 알림 영역 버튼을 열거한다. PrintWindow는 태스크바가 보이면 TrayNotifyWnd 비검정 99.78%이고, bamti가 `SW_HIDE`로 주차하면 0%이다. **구현할 백엔드는 UIA(`tray_backend_uia.cpp`)뿐이다. `tray_backend_toolbar.cpp`는 만들지 않는다.** 아이콘 픽셀은 5-7절의 `kParkedVisible`이 필요하다.

레거시 경로가 1순위인 이유는 그것만이 미러링에 필요한 값을 한 번에 주기 때문입니다. 없어도 되는 값이 아니라, 없으면 미러가 반쪽이 되는 값들입니다.

| 필요한 값 | 레거시 백엔드 | UIA 백엔드 |
|---|---|---|
| 아이콘 픽셀 | `HICON`을 직접 얻습니다 | 없습니다. 5-7절의 화면 캡처로 잘라내야 합니다 |
| 소유 창과 콜백 메시지 번호 | 있습니다 | 없습니다. 모든 요소가 explorer 소유로 보입니다 |
| 툴팁 문자열 | 있습니다 | 있습니다(`Name`) |
| 좌클릭 전달 | 메시지를 직접 보냅니다 | `Invoke`로 explorer에 위임합니다 |
| 우클릭 컨텍스트 메뉴 | `WM_CONTEXTMENU`를 전달합니다 | **대응하는 패턴이 없습니다** |
| 열거 1회 비용 | 수백 마이크로초, COM 없음 | COM 크로스 프로세스 순회로 훨씬 비쌉니다 |

따라서 UIA 백엔드만 남는 경우에는 두 가지 기능 저하를 문서와 설정 화면에 분명히 적어야 합니다. 첫째로 아이콘 충실도가 화면 캡처 품질에 종속됩니다. 둘째로 우클릭 메뉴를 띄우지 못하는 앱이 생깁니다. 이를 숨기지 마십시오.

#### 5-1. 파일 구성

```
src/tray_mirror.hpp / .cpp        StatusSource 구현, 폴링과 변경 감지
src/tray_backend.hpp              백엔드 인터페이스
src/tray_backend_toolbar.cpp      레거시 ToolbarWindow32 백엔드
src/tray_backend_uia.cpp          UI Automation 백엔드
```

```cpp
struct TrayIconInfo {
  uint64_t key = 0;          // 안정적인 식별자. 아래 5-3 참고
  HWND owner = nullptr;      // 레거시 백엔드에서만 채운다
  UINT uid = 0;
  UINT callback_msg = 0;
  HICON icon = nullptr;      // 소유하지 않는다. 캐시가 복사한다
  std::wstring tip;
  bool hidden = false;       // Windows가 오버플로로 감춘 아이콘인가
};

class TrayBackend {
 public:
  virtual ~TrayBackend() = default;
  virtual const char* Name() const = 0;
  virtual bool Probe() = 0;                                    // 이 컴퓨터에서 쓸 수 있는가
  virtual bool Enumerate(std::vector<TrayIconInfo>* out) = 0;   // 작업자 스레드에서 호출된다
  virtual bool Invoke(const TrayIconInfo& icon, bool right, POINT screen) = 0;
};
```

`TrayMirror::Start()`가 백엔드를 순서대로 `Probe()` 하고 처음 성공한 것을 씁니다. 구현할 백엔드와 그 순서는 5-0절의 표가 정합니다. 백엔드가 하나뿐이면 인터페이스는 그대로 두되 구현체도 하나만 둡니다. 어떤 백엔드를 골랐는지 반드시 로그에 남깁니다.

#### 5-2. 레거시 백엔드 (탐침이 버튼을 찾았을 때만 구현합니다)

`Shell_TrayWnd > TrayNotifyWnd > SysPager > ToolbarWindow32`와 최상위 `NotifyIconOverflowWindow > ToolbarWindow32`에서 버튼을 읽습니다. 대상 프로세스는 explorer이므로 다음 절차가 필요합니다.

1. `GetWindowThreadProcessId`로 PID를 얻고 `PROCESS_VM_OPERATION`, `PROCESS_VM_READ`, `PROCESS_VM_WRITE`, `PROCESS_QUERY_LIMITED_INFORMATION` 권한으로 엽니다.
2. `VirtualAllocEx`로 버퍼 하나를 잡고 **미러가 사는 동안 재사용합니다.** 열거할 때마다 할당하고 해제하지 마십시오.
3. `TB_BUTTONCOUNT`, `TB_GETBUTTON`, `TB_GETBUTTONTEXTW`를 `SendMessageTimeoutW`로 보냅니다. 시간 제한은 100ms이며 `SMTO_ABORTIFHUNG`을 씁니다. **`SendMessageW`를 그대로 쓰지 마십시오.** explorer가 멈추면 우리 작업자 스레드가 함께 멈춥니다.
4. `TBBUTTON.dwData`가 가리키는 트레이 구조를 `ReadProcessMemory`로 읽습니다. 이 구조는 비공개이므로 다음 방어를 넣습니다. `hWnd`가 `IsWindow()`를 통과해야 하고, `uCallbackMessage`가 `WM_USER` 이상이어야 하며, `hIcon`이 `GetIconInfo`를 통과해야 합니다. 하나라도 실패하면 그 항목을 버리고, 연속 세 번 전부 실패하면 백엔드를 비활성으로 표시한 뒤 UIA 백엔드로 내려갑니다.
5. 아이콘은 `CopyIcon`으로 우리 프로세스에 복제해 보관합니다. explorer가 원본을 파괴한 뒤에도 그리기가 안전해야 합니다.

#### 5-3. 변경 감지와 폴링

트레이 변경을 알려 주는 공개 이벤트는 없습니다. 다음처럼 비용을 눌러 폴링합니다.

- 작업자 스레드에서 **1초** 주기로 열거합니다. 항목이 20개일 때 한 번의 열거 비용은 수백 마이크로초 수준이어야 하며, 그렇지 않으면 구현이 잘못된 것입니다.
- 각 항목의 `key`는 `hash(owner, uid)`로 만듭니다. UIA 백엔드에서는 `hash(AutomationId, Name)`을 씁니다.
- `(key, icon_handle, tip, hidden)` 튜플의 해시를 이전 회차와 비교합니다. 같으면 **아무것도 하지 않습니다.** `Upsert`도 부르지 않습니다.
- `RegisterWindowMessageW(L"TaskbarCreated")` 브로드캐스트를 받으면 즉시 한 번 열거합니다.
- 전체화면 가림, 세션 잠금, 모니터 꺼짐 상태에서는 폴링을 멈춥니다(`SetActive(false)`).

#### 5-4. 클릭 전달

이것이 가장 까다로운 부분입니다. 아이콘의 콜백 규약 버전을 우리가 알 수 없기 때문입니다. 다음 우선순위로 처리합니다.

1. **UIA `Invoke`를 먼저 시도합니다.** 레거시 백엔드를 쓰는 경우에도 좌클릭은 이 경로가 우선입니다. 아이콘이 구형 규약과 `NOTIFYICON_VERSION_4` 중 무엇으로 등록되었는지 우리는 알 수 없지만 explorer는 알고 있기 때문입니다. 다만 우클릭에는 대응하는 패턴이 없으므로 좌클릭에 대해서만 신뢰할 수 있습니다.
2. 실패하거나 우클릭이면 메시지를 직접 보냅니다. 이 경로는 레거시 백엔드가 주는 `owner`와 `callback_msg`가 있어야 하므로, UIA 백엔드만 있는 환경에서는 우클릭을 전달하지 못합니다. 그 사실을 툴팁이나 설정 화면에 밝히고, 대신 5-5절의 축소 대안을 함께 제공합니다.

```cpp
// 앱이 컨텍스트 메뉴를 띄우려면 포그라운드 권한이 필요하다.
DWORD pid = 0;
GetWindowThreadProcessId(icon.owner, &pid);
AllowSetForegroundWindow(pid);

// 구형 규약. 대부분의 앱이 이 형태로 등록되어 있다.
PostMessageW(icon.owner, icon.callback_msg, icon.uid, right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN);
PostMessageW(icon.owner, icon.callback_msg, icon.uid, right ? WM_RBUTTONUP   : WM_LBUTTONUP);
if (right) {
  PostMessageW(icon.owner, icon.callback_msg, icon.uid, WM_CONTEXTMENU);
}
```

`NOTIFYICON_VERSION_4`로 등록한 앱은 `wParam`에 좌표를, `lParam`에 이벤트와 `uid`를 함께 담은 값을 기대합니다. 두 형태를 모두 보내면 메뉴가 두 번 뜨는 앱이 생기므로 보내지 마십시오. 대신 설정에 항목별 "새 규약으로 보내기" 예외 목록을 두고, 반응하지 않는 앱만 사용자가 바꾸게 합니다. 기본값은 구형 규약입니다.

앱은 보통 `WM_CONTEXTMENU`를 받으면 `GetCursorPos()` 위치에 메뉴를 띄웁니다. 사용자의 커서가 상단바 위에 있으므로 메뉴가 상단바 아래에 자연스럽게 열립니다. 별도 좌표 보정은 하지 않습니다.

**클릭을 전달하기 전에 반드시 `PopupSurface`를 닫으십시오.** 우리 팝업이 마우스를 캡처한 상태에서 다른 프로세스가 메뉴를 띄우면 두 모달이 겹칩니다.

#### 5-5. 백엔드가 모두 실패할 때

읽기 경로가 전혀 없는 빌드에서는 미러를 포기하고 **접근 경로만 보장합니다.** 상단바에 갈매기 아이콘 하나를 두고, 누르면 explorer의 알림 영역을 원래 위치로 잠시 되돌려 보여 줍니다. 사용자가 그 바깥을 클릭하면 다시 주차합니다. 기능 손실은 로그와 설정 화면에 분명히 알립니다. **미러가 안 된다고 해서 트레이 앱을 쓸 수 없는 상태를 만들지 마십시오.**

#### 5-6. 가로채기 방식은 기본값으로 넣지 않는다

`Shell_TrayWnd` 클래스로 창을 만들어 `WM_COPYDATA`를 대신 받는 방식은 데이터 충실도가 가장 높지만, `FindWindowW`가 어느 창을 돌려줄지가 보장되지 않아 일부 앱의 트레이 등록이 explorer가 아닌 bamti로 가고 그 반대도 일어납니다. 목표 조건 2와 충돌하므로 다음 조건에서만 허용합니다.

- 설정에서 명시적으로 켠 경우에만 동작하고, 기본값은 꺼짐입니다.
- 켜져 있으면 받은 모든 `WM_COPYDATA`를 explorer의 실제 `Shell_TrayWnd`로 전달합니다.
- 이 코드는 `src/tray_intercept.cpp` 한 파일에 격리하고, 다른 모듈이 참조하지 않습니다.

시간이 부족하면 이 절은 통째로 생략하십시오. 필수가 아닙니다.

#### 5-7. 태스크바 주차 방식 조정

`taskbar_controller.cpp`는 현재 트레이 창을 `SW_HIDE` 한 뒤 `kParkY`(32000)로 옮깁니다. UIA 백엔드가 `PrintWindow`로 아이콘 픽셀을 얻어야 한다면 창이 `SW_HIDE` 상태여서는 안 됩니다. 따라서 `TaskbarController`에 다음을 추가합니다.

```cpp
enum class HideMode { kHidden, kParkedVisible };
bool Hide(HideMode mode);
```

`kParkedVisible`은 `ShowWindow`를 부르지 않고 화면 밖으로만 옮깁니다. 이 모드는 UIA 백엔드가 선택되었을 때만 씁니다. 자동 숨김 설정과 작업 영역 반환은 두 모드에서 동일하게 적용합니다. 전환할 때 태스크바가 화면 가장자리에서 잠깐이라도 보이면 안 됩니다.

**검증**
- [ ] 트레이 앱을 5개 이상 실행한 상태에서 아이콘이 전부 상단바에 나타납니다.
- [ ] 아이콘 좌클릭이 원래 앱의 동작을 실행합니다(예: 창 복원).
- [ ] 아이콘 우클릭이 원래 앱의 컨텍스트 메뉴를 띄우고, 메뉴 항목을 고르면 실행됩니다. UIA 백엔드만 있는 환경이면 이 항목은 해당 없음으로 두되, 그 사실이 설정 화면에 표시되는지 확인합니다.
- [ ] 트레이 앱을 종료하면 1초 안에 상단바에서 사라집니다.
- [ ] explorer를 강제로 재시작해도 미러가 자동으로 복구됩니다.
- [ ] 태스크바를 다시 표시하면 explorer의 알림 영역에 아이콘이 그대로 있습니다. bamti가 아무것도 가져가지 않았음을 확인합니다.
- [ ] 아이콘 20개 상태에서 열거 1회 소요 시간이 1ms 미만입니다(로그로 확인).
- [ ] explorer가 응답하지 않는 상태를 만든 뒤에도 상단바가 멈추지 않습니다.

---

### 6단계: LLM 사용량 헬퍼 (CodexBar 이식)

#### 6-1. 별도 타깃

```
tools/usage/CMakeLists.txt      bamti-usage.exe
tools/usage/main.cpp            단일 인스턴스, 파이프 연결과 재연결, 스케줄러
tools/usage/pipe_client.cpp     NDJSON 클라이언트, 지수 백오프 재연결
tools/usage/http.cpp            WinHTTP 래퍼. 타임아웃, 시스템 프록시, 재시도
tools/usage/provider.hpp        공통 인터페이스
tools/usage/provider_codex.cpp
tools/usage/provider_claude.cpp
tools/usage/format.cpp          UsageSnapshot을 Status Item JSON으로 변환
```

의존성은 WinHTTP와 표준 라이브러리로 제한합니다. JSON은 `src/json_line.hpp`를 공유합니다. 실행 파일 크기와 상주 메모리를 작게 유지하는 것이 이 타깃의 설계 목표입니다.

#### 6-2. 공급자 인터페이스

```cpp
struct UsageWindow {
  std::wstring label;                 // "Session (5h)", "Weekly"
  double used = 0.0;                  // 0.0 ~ 1.0
  std::chrono::system_clock::time_point starts_at{};
  std::chrono::system_clock::time_point resets_at{};
};

struct UsageSnapshot {
  std::wstring account;               // 패널 부제에 쓴다
  std::wstring plan;
  std::vector<UsageWindow> windows;
  std::chrono::system_clock::time_point fetched_at{};
  std::wstring error;                 // 비어 있지 않으면 오류 상태
};

class UsageProvider {
 public:
  virtual ~UsageProvider() = default;
  virtual std::string Id() const = 0;          // "codex", "claude"
  virtual std::wstring DisplayName() const = 0;
  virtual bool Available() = 0;                // 자격 증명이 있는가
  virtual UsageSnapshot Fetch() = 0;           // 동기 호출. 작업자 스레드에서만 부른다
};
```

#### 6-3. CodexBar에서 가져와야 하는 것

상류 저장소는 `https://github.com/steipete/CodexBar` 입니다. 다음 다섯 가지를 이식합니다. **엔드포인트 경로와 응답 구조는 기억에 의존하지 말고 실제 상류 소스와 실제 응답을 확인해 확정하십시오.**

1. 자격 증명의 위치와 형식. Codex CLI는 `%USERPROFILE%\.codex\auth.json`, Claude Code는 `%USERPROFILE%\.claude` 아래 또는 Windows 자격 증명 관리자에 있을 가능성이 높습니다. macOS 키체인을 쓰는 경로는 Windows 등가물로 바꿔야 합니다.
2. 사용량 조회 엔드포인트, 필요한 헤더, 토큰 갱신 절차.
3. 사용 창(session, weekly 등)의 정의와 초기화 시각 계산 방법.
4. 진도 판정식. 창의 경과 비율보다 사용 비율이 낮으면 "Ahead of pace"로 표시합니다.
5. 표시 형식. 바에는 창들 중 최대 사용률을 정수 퍼센트로 그립니다.

#### 6-4. 스케줄과 상태 표시

| 상황 | 주기 |
|---|---|
| 정상 | 60초 |
| 오류 | 60초에서 시작해 두 배씩 늘리고 900초에서 멈춥니다 |
| 자격 증명 없음 | 조회하지 않고 항목을 제거합니다 |
| `panel_open` 이벤트 수신 | 즉시 한 번 |
| 패널의 새로 고침 버튼 | 즉시 한 번 |

대기는 `SetWaitableTimerEx`에 허용 지연을 크게 주어 시스템이 타이머를 합칠 수 있게 합니다. 절전과 배터리에 유리합니다. 컴퓨터가 절전에서 복귀하면 즉시 한 번 조회합니다.

바 표시 규칙은 다음과 같습니다.

- `state`는 최대 사용률이 80% 미만이면 `normal`, 80% 이상 95% 미만이면 `warn`, 95% 이상이면 `error`입니다.
- 조회 실패가 연속 3회 이상이면 `state`를 `error`로 두고 `text`는 마지막 성공값을 그대로 유지하되 툴팁에 실패 사실을 적습니다. 숫자를 지우지 마십시오. 사용자는 마지막으로 알려진 값을 보고 싶어 합니다.
- 패널의 `updated` 문구는 마지막 성공 시각을 기준으로 계산한 경과 시간입니다.

#### 6-5. 보안

- 토큰과 자격 증명 파일의 내용을 로그에 절대 남기지 않습니다. 오류 로그에는 HTTP 상태 코드와 공급자 식별자만 남깁니다.
- 자격 증명 파일은 읽기 전용으로만 엽니다.
- 요청은 해당 공급자 호스트로만 보냅니다. 통계 수집이나 원격 보고 경로를 만들지 않습니다.

#### 6-6. bamti 본체와의 결합

`bamti.exe`가 설정에 따라 `bamti-usage.exe`를 자식으로 띄웁니다. 죽으면 지수 백오프로 다시 띄우고, 1분 안에 5회 이상 죽으면 포기한 뒤 로그에 남깁니다. `watchdog.cpp`의 기존 구조를 확장하십시오. 헬퍼가 없어도 bamti는 정상 동작해야 합니다.

**검증**
- [ ] Codex 자격 증명이 있는 계정에서 상단바에 퍼센트가 나타납니다.
- [ ] 항목을 클릭하면 첨부 이미지와 같은 구조의 패널이 열립니다. 제목, 부제, 갱신 시각, 창별 게이지, 초기화 시각, 하단 버튼이 있습니다.
- [ ] 네트워크를 끊으면 마지막 값이 유지되고 툴팁에 오류가 표시되며, 복구하면 60초 안에 갱신됩니다.
- [ ] `bamti-usage.exe`를 강제 종료해도 상단바가 멈추지 않고, 항목만 사라진 뒤 다시 살아납니다.
- [ ] 헬퍼의 유휴 작업 집합이 12MB 미만이고 유휴 CPU 사용률이 0.05% 미만입니다.
- [ ] 로그 전체를 검색해 토큰 문자열이 하나도 나오지 않습니다.

---

### 7단계: 설정과 마무리

상단바 항목이 늘어나면 사용자가 무엇을 보일지 고를 수 있어야 합니다. 설정 저장소는 `%USERPROFILE%\.bamti\settings.json` 한 파일이며, 읽기와 쓰기는 UI 스레드가 아닌 곳에서만 합니다.

```json
{
  "topbar": {
    "tray_mirror": {"enabled": true, "backend": "auto", "show_hidden": false},
    "widgets": {"battery": false, "cpu": false, "network": false, "widget_board_button": false},
    "usage_helper": {"enabled": true, "providers": ["codex", "claude"]},
    "item_order": ["dev.steipete.codexbar/codex", "bamti.tray/*"],
    "hidden_items": []
  }
}
```

상단바 빈 곳을 우클릭하면 `PopupSurface`로 항목 표시 여부를 토글하는 메뉴를 엽니다. 새 팝업 인프라를 만들지 마십시오.

---

## 5. 성능 예산

이 표를 만족하지 못하면 그 단계는 완료가 아닙니다. 각 항목은 측정 방법까지 적었습니다.

| 항목 | 예산 | 측정 방법 |
|---|---|---|
| bamti.exe 유휴 CPU (모든 기능 켬) | 0.2% 미만 | 작업 관리자 5분 평균 |
| bamti.exe 유휴 CPU (트레이 미러만) | 0.1% 미만 | 위와 같음 |
| bamti.exe 상주 작업 집합 | 40MB 미만 | 작업 관리자 |
| bamti-usage.exe 상주 작업 집합 | 12MB 미만 | 작업 관리자 |
| 바 전체 다시 그리기 | 2.8ms | `Log(L"perf", ...)` 워밍 창 평균. 원래 목표 2ms. [^bar-draw-budget] |
| 세그먼트 하나만 다시 그리기 | 1.4ms | 워밍 창 평균. 원래 목표 0.3ms. [^bar-draw-budget] |
| 상태 갱신에서 화면 반영까지 | 50ms 미만 | 갱신 시각과 그리기 시각의 차이를 로그로 |
| 트레이 열거 1회 (아이콘 20개) | 1ms 미만 | 작업자 스레드에서 로그로 |
| 트레이 아이콘 클릭 응답 | 100ms 미만 | 체감 및 로그 |
| 유휴 상태 컨텍스트 스위치 | 초당 5회 이하 | 성능 모니터 |

지켜야 할 규칙은 다음과 같습니다.

1. **UI 스레드에서 블로킹 호출을 하지 않습니다.** 크로스 프로세스 `SendMessage`, 파일 입출력, 레지스트리 조회, 네트워크 호출이 전부 해당합니다. 필요하면 작업자 스레드에서 하고 결과만 `PostMessage`로 넘깁니다.
2. **변화가 없으면 아무 일도 하지 않습니다.** 폴링 결과가 이전과 같으면 `Upsert`도 `InvalidateRect`도 부르지 않습니다.
3. **다시 그리기는 프레임당 한 번으로 합칩니다.** 3-3절의 16ms 합치기 타이머를 지킵니다.
4. **가려져 있으면 멈춥니다.** 전체화면 가림, 세션 잠금, 모니터 꺼짐에서 모든 공급자가 `SetActive(false)`를 받습니다.
5. **할당을 반복 경로에서 하지 않습니다.** 트레이 열거 버퍼, 아이콘 비트맵, 텍스트 레이아웃은 전부 캐시합니다.

[^bar-draw-budget]: `ID2D1DCRenderTarget`를 `D2D1_RENDER_TARGET_TYPE_SOFTWARE`로 바꾼 뒤 도달한 값이다. 워밍 창 구간 분해는 `end=0.65ms`, `begin=0.39ms`, `bind=0.29ms`, `bpbegin=0.19ms`, `bpend=0.13ms`, `draw=0.05ms`이다. 원래 목표 0.3ms/2ms에 닿으려면 DC 렌더 타깃을 창에 붙는 타깃으로 바꾸거나(BindDC 제거) 버퍼드 페인트를 재사용 DIB로 바꿔야 하며, 둘 다 그리기 경로 구조 변경이라 이 단계 범위 밖이다.

---

## 6. 하지 말아야 할 것

1. **explorer에 인젝션하지 않습니다.** DLL 주입, IAT 후킹, `SetWindowsHookEx`의 전역 DLL 훅을 쓰지 않습니다. 읽기와 `PostMessage`만 허용합니다.
2. **explorer의 트레이 창을 파괴하거나 리페어런팅하지 않습니다.** 옮기고 숨기는 것까지가 한계이며, 이는 이미 구현되어 있습니다.
3. **상주 프로세스에 새 UI 프레임워크를 들이지 않습니다.** Direct2D와 DirectWrite로 그립니다.
4. **플러그인 DLL 방식을 만들지 않습니다.** 확장은 파이프 프로토콜로만 받습니다.
5. **저수준 마우스 훅을 상시 설치하지 않습니다.** 팝업이 열린 동안의 마우스 캡처로 충분하며, 이는 `PopupSurface`에 이미 있습니다.
6. **새 팝업 인프라를 만들지 않습니다.** 오버플로, 상태 패널, 상단바 우클릭 메뉴가 전부 `PopupSurface`를 씁니다.
7. **비공개 구조를 검증 없이 신뢰하지 않습니다.** 5-2절 4번의 세 가지 검사를 반드시 넣습니다. 실패는 정상적인 상황이며 크래시가 되어서는 안 됩니다.
8. **사용량 조회 코드를 bamti.exe 안에 넣지 않습니다.**
9. **기존 v1 프로토콜 클라이언트를 깨뜨리지 않습니다.**

---

## 7. 작업 순서와 커밋 단위

| 순서 | 작업 | 커밋 메시지 예시 |
|---|---|---|
| 1 | 트레이 구조 탐침 도구 | `chore: 알림 영역 구조를 진단하는 탐침을 추가한다` |
| 2 | 바 세그먼트 모델과 부분 무효화 | `refactor: 상단바 레이아웃을 세그먼트 모델로 분리한다` |
| 3 | 프로토콜 v2 모델과 파서 | `feat: 상태 항목 프로토콜에 아이콘, 상태, 행 종류를 추가한다` |
| 4 | 아이콘 캐시 추출 | `refactor: 아이콘 비트맵 캐시를 공용 모듈로 옮긴다` |
| 5 | 공급자 레지스트리 | `refactor: 상태 항목 공급자를 레지스트리로 통합한다` |
| 6 | 패널 행 렌더링과 토글 | `feat: 상태 패널에 토글과 버튼 행을 그린다` |
| 7 | 내장 위젯 | `feat: 배터리, CPU, 네트워크 위젯을 추가한다` |
| 8 | 트레이 미러 백엔드와 열거 (5-0절 판정에 따라 백엔드 하나만) | `feat: 알림 영역 아이콘을 상단바에 미러링한다` |
| 9 | 트레이 클릭 전달 | `feat: 상단바 트레이 아이콘의 클릭을 원래 앱에 전달한다` |
| 10 | 사용량 헬퍼 골격과 파이프 클라이언트 | `feat: 사용량 헬퍼 프로세스를 추가한다` |
| 11 | Codex 공급자 | `feat: Codex 사용량 공급자를 구현한다` |
| 12 | Claude 공급자 | `feat: Claude 사용량 공급자를 구현한다` |
| 13 | 설정과 항목 표시 토글 | `feat: 상단바 항목 표시 설정을 추가한다` |

표의 1번과 2번은 서로 독립적이므로 순서를 바꿔도 됩니다. 8번은 반드시 1번 이후에, 6번은 반드시 프로토콜 v2(표의 3번) 이후에 진행합니다. 아이콘 캐시(표의 4번)는 3단계 프로토콜의 `icon` 필드와 함께 진행합니다. 표의 번호는 커밋 단위이며, 4절의 단계 번호와는 다릅니다.

---

## 8. 통합 검증 체크리스트

**빌드**
- [ ] `/W4` 경고 없이 컴파일됩니다.
- [ ] Debug와 Release 모두 빌드됩니다.
- [ ] x64와 ARM64 모두 빌드됩니다.

**회귀 (기존 기능이 그대로여야 합니다)**
- [ ] 독의 핀 고정, 해제, 드래그 재배치가 동작합니다.
- [ ] 독 컨텍스트 메뉴가 지연 없이 뜨고 확실히 닫힙니다.
- [ ] `Win+Space` Spotlight와 시작 메뉴가 그대로 열립니다.
- [ ] 전체화면 게임에서 상단바와 독이 즉시 사라지고 즉시 복귀합니다.
- [ ] `bamti.exe --restore-taskbar`가 동작합니다.
- [ ] 기존 v1 파이프 클라이언트가 수정 없이 표시됩니다.

**새 기능**
- [ ] 트레이 아이콘이 상단바에 나타나고 좌클릭이 원래 앱에 전달됩니다. 우클릭은 레거시 백엔드를 쓰는 환경에서만 검증 대상입니다.
- [ ] LLM 사용량이 첨부 이미지와 같은 구조로 표시되고 패널이 열립니다.
- [ ] 아이콘과 값, 아이콘과 토글이 같은 프로토콜로 표현됩니다(`examples/ticker.py`로 확인).
- [ ] 항목이 많아지면 오버플로로 접히고 갈매기를 눌러 볼 수 있습니다.
- [ ] 다크 테마와 라이트 테마, 100%와 150%와 200% DPI에서 정렬이 깨지지 않습니다.

**견고성**
- [ ] explorer를 강제 종료하고 다시 시작해도 상단바와 미러가 복구됩니다.
- [ ] 파이프 클라이언트를 100회 붙였다 떼도 핸들과 GDI 객체가 늘지 않습니다.
- [ ] 잘못된 JSON, 지나치게 긴 문자열, 깨진 UTF-8을 보내도 크래시하지 않고 해당 줄만 버립니다.
- [ ] 모니터를 뽑고 꽂아도, DPI를 바꿔도 레이아웃이 복구됩니다.

**성능**
- [ ] 5절의 예산을 전부 만족합니다.

---

## 9. 사용자 질문에 대한 답

질문은 다음과 같았습니다. CodexBar 기능 외에도 아이콘과 값, 아이콘과 토글을 동시에 보여 주는 기능이 필요할 텐데, 이를 하나의 규격으로 정해 두어야 하는가.

**정해 두어야 합니다.** 그것도 지금, 첫 소비자를 만들기 전에 정하는 것이 맞습니다. 이유는 세 가지입니다.

1. 규격을 나중에 정하면 LLM 사용량 표시가 특수 사례로 하드코딩되고, 비트코인 시세를 붙일 때 두 번째 경로가 생깁니다. 두 경로는 곧 세 경로가 되며, 그 시점에는 레이아웃과 오버플로와 팝업 동작이 서로 어긋납니다.
2. 규격이 있으면 트레이 미러와 내장 위젯도 같은 경로를 타므로, 렌더링 코드가 한 벌로 유지됩니다. 이것이 성능 예산을 지키는 가장 확실한 방법입니다.
3. 외부 개발자가 bamti에 항목을 붙일 때 필요한 것이 문서 한 장으로 끝납니다. 프로세스 안에서 남의 코드를 실행하지 않으므로 안정성도 함께 얻습니다.

다만 규격의 범위를 명확히 해야 합니다. 3-1절에 적었듯이 **의미를 기술하고 배치는 기술하지 않습니다.** 공급자가 픽셀 좌표나 임의의 그리기 명령을 보낼 수 있게 하면 그것은 프로토콜이 아니라 반쪽짜리 UI 프레임워크가 되고, 성능과 일관성이 모두 무너집니다. 공급자가 말할 수 있는 것은 아이콘, 짧은 값, 상태, 강조색 제안, 우선순위, 그리고 정해진 여섯 종류의 패널 행뿐입니다. 이 정도면 사용량, 시세, 토글, 상태 표시등을 모두 표현할 수 있고, 그 이상이 필요한 앱은 자기 창을 띄우면 됩니다.

---

## 10. 미해결 항목

구현 중에 결정이 필요한 것들입니다. 임의로 정하지 말고 결과를 기록한 뒤 이 문서를 갱신하십시오.

1. ~~이 컴퓨터(Windows 11 빌드 26200)에서 레거시 `ToolbarWindow32` 백엔드가 동작하는지 여부.~~ **해결.** 동작하지 않는다. 상주 중과 정상 상태 모두 ToolbarWindow32가 없고 검증 통과 버튼은 0개다. 근거: `PROBE-TRAY.md`. 5-0절에 따라 UIA 백엔드만 구현한다.
2. ~~UIA 백엔드로 아이콘 픽셀을 얻는 방법이 실용적인지 여부.~~ **부분 해결.** 태스크바가 보이면 `PrintWindow(PW_RENDERFULLCONTENT)`가 TrayNotifyWnd에서 비검정 99.78%를 준다. bamti가 `SW_HIDE`로 주차하면 0%다. 화면 밖이되 보이는 주차는 탐침이 시스템 상태를 바꾸지 않아 측정하지 못했다. 5-7절은 `kParkedVisible`로 구현한다.
3. 다중 모니터에서 상단바를 모니터마다 둘 것인지, 주 모니터에만 둘 것인지. 현재 코드는 하나만 만듭니다. 이 작업의 범위 밖이지만 트레이 미러의 위치 결정에 영향을 줍니다.
4. Claude Code의 Windows 자격 증명 저장 위치. 파일인지 자격 증명 관리자인지 실제 설치본에서 확인이 필요합니다.

---

## 11. 참고: 첨부 이미지와의 대응

| 이미지 요소 | 이 계획에서의 구현 |
|---|---|
| 상단바 좌측의 아이콘과 `10%` | `segment.icon` + `segment.text` |
| 불꽃 이모지 강조 | `segment.state` = `warn`, 또는 `accent` |
| 여러 앱 아이콘이 나란히 | 우선순위 정렬된 `kStatus` 세그먼트들 |
| 우측 날짜와 시각 | 기존 `ClockRenderer` |
| 패널 제목 `Codex`, 부제 `Prolite` | `panel.title`, `panel.subtitle` |
| `Updated 13s ago` | `panel.updated` |
| 우상단 새로 고침 단추 | `button` 행, 또는 패널 헤더의 고정 동작 |
| `Session (5h) — Codex`와 진행 바와 `6%` | `gauge` 행의 `label`, `value`, `value_text` |
| `Resets 18:31 · in 3h 38m` | `gauge` 행의 `detail` |
| `Ahead of pace` | `gauge` 행의 `note` |
| 하단 `Settings...`와 `Quit` | 연속된 `button` 행 두 개 |

---

## 12. ARCHITECTURE.md 갱신 사항

이 계획이 승인되면 상위 문서의 다음 항목을 함께 고칩니다. 코드와 문서가 어긋나지 않게 하기 위한 것이며, 구현 마지막 커밋에 포함하십시오.

| 위치 | 기존 서술 | 갱신 후 |
|---|---|---|
| 1절 비범위 | "기존 `NOTIFYICON`을 맥 메뉴바 텍스트로 자동 변환하는 일" | 유지합니다. 미러는 텍스트 변환이 아니라 아이콘 재표시이므로 이 비범위와 충돌하지 않습니다. 이 점을 한 문장으로 덧붙입니다. |
| 8절 | v1 초안 스키마 | `docs/STATUS-PROTOCOL.md`를 참조하도록 바꾸고, 초안 예시는 v2로 교체합니다. |
| 9절 표, "기존 트레이 아이콘" 행 | "시스템 알림 영역이 담당. bamti가 재호스팅하지 않는다" | "explorer가 계속 소유한다. bamti는 읽기 전용으로 미러링하고 클릭을 전달한다"로 바꿉니다. |
| 9절 표, "Windows 11 위젯 보드" 행 | "임베드하지 않는다" | 유지합니다. 보드를 여는 버튼과 내장 위젯 공급자를 제공한다는 문장을 덧붙입니다. |
| 12절 구현 순서 9번 | "(후순위) 트레이 호스팅" | "트레이 미러(읽기 전용)"로 바꾸고 순서를 앞으로 올립니다. |
| 13절 리스크 | "트레이 재호스팅은 코어에 넣지 않는다" | 미러링과 가로채기를 구분해 다시 씁니다. 미러링은 읽기 전용이므로 위험이 낮고, 가로채기는 기본값 꺼짐 실험 기능임을 명시합니다. |
