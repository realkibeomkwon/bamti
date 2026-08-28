# Status Item 프로토콜

bamti 상단바에 아이콘과 값, 아이콘과 토글, 아이콘과 팝업 패널을 올리는 UTF-8 NDJSON 규격입니다.

## 원칙

1. 공급자는 의미를 말하고 픽셀 배치를 지시하지 않습니다. 색은 `accent`로만 제안할 수 있으며, 테마 대비가 부족하면 bamti가 조정합니다.
2. 모르는 필드는 무시합니다.
3. 모르는 행 `type`은 `fallback_text`가 있으면 일반 텍스트 행으로 그리고, 없으면 그 행만 생략합니다. 패널은 열립니다.
4. `{"v":1,"op":"upsert","id":...,"text":...}` 형태는 계속 동작합니다.

## 연결

사용자 세션 로컬 named pipe `\\.\pipe\bamti-status`. 한 줄에 레코드 하나.

연결 직후 bamti가 먼저 보냅니다. 클라이언트가 이 줄을 읽지 않고 `upsert`를 보내도 됩니다.

```json
{"v":2,"op":"hello","renderer":"bamti","version":"1.2.0","proto":[1,2],
 "features":["icon_glyph","icon_png","gauge","kv","toggle","button","text","separator","events"],
 "limits":{"segment_text":32,"panel_rows":32,"panel_text":128,"icon_png_bytes":8192,"upserts_per_sec":10}}
```

클라이언트는 쓸 버전을 각 줄의 `v`에 담습니다. 모르는 `v`는 조용히 버립니다.

## 클라이언트 → bamti

| op | 뜻 |
|---|---|
| `upsert` | 항목 전체 교체. 같은 `id`는 덮어씁니다. |
| `patch` | 있는 항목만 부분 갱신. 없는 `id`는 무시합니다. 필드가 없으면 유지하고, 빈 문자열이 오면 지웁니다. |
| `remove` | 자신이 올린 항목만 제거합니다. |
| `ping` | 하트비트. 상태를 바꾸지 않습니다. |

`id`는 `벤더/앱/항목` 형태의 역방향 도메인 접두를 권장합니다. 다른 클라이언트가 소유한 `id`는 덮어쓸 수 없습니다.

### v2 upsert

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
| `icon` | 객체 (선택) | 아래 아이콘. 생략하면 텍스트만 그립니다. |
| `text` | 문자열 (선택) | 바 텍스트. 32자를 넘으면 말줄임합니다. |
| `tooltip` | 문자열 (선택) | 호버 툴팁. 128자로 자릅니다. |
| `state` | `normal` \| `warn` \| `error` \| `on` \| `off` \| `busy` | 렌더러가 색을 정합니다. |
| `accent` | `#RRGGBB` 또는 정수 (선택) | `state`가 `normal`일 때 제안색. 파싱 실패 시 0. |
| `priority` | 정수 (기본 0) | 큰 값이 시계에 가깝습니다. |
| `visible` | 불리언 (기본 true) | false면 바에서 숨기고 오버플로 팝업에는 남깁니다. |

`icon`과 `text`가 모두 비면 그 `upsert`는 무시합니다.

#### panel

`panel`이 없으면 클릭 시 팝업을 열지 않고 이벤트만 보냅니다. `rows`는 최대 32개입니다.

| type | 필드 | 렌더링 |
|---|---|---|
| `gauge` | `label`, `value`(0.0~1.0), `value_text`(선택), `detail`(선택), `note`(선택) | 레이블과 우측 값, 진행 바, 상세와 비고 |
| `kv` | `label`, `value` | 좌측 레이블, 우측 값 |
| `text` | `text`, `style`(`body` \| `note`) | 한 줄. `note`면 흐린 색 |
| `separator` | 없음 | 구분선 |
| `toggle` | `row_id`, `label`, `on` | 레이블과 스위치 |
| `button` | `row_id`, `label`, `style`(`normal` \| `danger`) | 버튼. `danger`면 경고색 |

연속한 `button`은 한 줄에 최대 세 개입니다.

`value`는 0.0~1.0으로 클램프합니다. NaN과 무한대, 문자 값은 0입니다.

### 아이콘

```json
{"kind":"glyph","glyph":"◐"}
{"kind":"png","data":"<base64>"}
{"kind":"file","path":"C:\\Program Files\\App\\app.ico"}
```

- `glyph`: 최대 8 UTF-16 단위.
- `png`: 디코드 후 8KB 이하, 64×64 이하. 초과·실패 시 텍스트만 그리고 로그를 남깁니다.
- `file`: `.ico`와 `.png`. bamti가 읽습니다. 실패 시 아이콘을 생략합니다. 파일 감시는 하지 않습니다.

파이프로 `hicon`을 보내지 마십시오. 프로세스 내부 전용입니다.

### v1

`text`, `icon` 또는 `icon_glyph`, `tooltip`, `accent`(정수), `priority`, `panel.title` / `subtitle` / `updated`, `panel.gauges[]`, `panel.actions[]`를 받습니다. 게이지는 `gauge` 행이 되고, 액션은 구분선 뒤에 `button` 행이 됩니다. `button`의 `row_id`는 원래 인덱스 문자열입니다.

## bamti → 클라이언트

v2 연결:

```json
{"v":2,"op":"event","id":"...","event":"click","button":"left"}
{"v":2,"op":"event","id":"...","event":"click","button":"right"}
{"v":2,"op":"event","id":"...","event":"panel_open"}
{"v":2,"op":"event","id":"...","event":"panel_close"}
{"v":2,"op":"event","id":"...","event":"invoke","row_id":"settings"}
{"v":2,"op":"event","id":"...","event":"toggle","row_id":"auto_refresh","on":false}
```

v1 연결에는 `{"v":1,"op":"click","id":"...","button":"..."}`만 보냅니다. 버튼의 `button` 값은 액션 문자열입니다.

`panel_open`은 값을 즉시 고칠 기회입니다.

## 토글

1. 스위치를 누르면 bamti가 즉시 시각 상태를 뒤집습니다.
2. `toggle` 이벤트를 보냅니다.
3. 2초 안에 그 `id`의 `upsert` 또는 `patch`가 없으면 스위치를 되돌리고 항목을 `error`로 표시합니다.

## 상한

| 항목 | 상한 | 초과 시 |
|---|---|---|
| 갱신 빈도 | 초당 10회 수준 | 마지막 값만 남기고 UI 통지를 50ms로 묶습니다. |
| 클라이언트당 항목 | 16 | 새 `upsert`를 무시하고 로그를 한 번 남깁니다. |
| 전체 항목 | 64 | 위와 같습니다. |
| 한 줄 | 64KB | 그 연결을 끊습니다. |
| 하트비트 | 15초 | 무응답이면 그 클라이언트의 항목을 제거합니다. |

바 텍스트 32자, 패널 문자열 128자, 글리프 8자. 파서를 통과한 값은 UI에서 다시 검증하지 않습니다.
