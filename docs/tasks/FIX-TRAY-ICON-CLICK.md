# 수정 지시서: 미러 아이콘의 픽셀과 클릭 전달을 고친다

> 작업 당시의 진단과 지시를 남긴 기록입니다. 현재 코드의 설명이 아닙니다.

작성일: 2026-08-31
선행 문서: `TASK-TRAY-PREEMPT.md`, `FIX-TRAY-PREEMPT-DIAG.md`
대상: `src/tray_intercept.cpp`의 아이콘 변환과 클릭 전달

---

## 0. 증상

재부팅 측정으로 **아이콘을 받는 문제는 해결됐습니다.** 서드파티 넷을 전부 받았습니다. 그런데 받은 것을 쓰는 단계가 셋 다 망가져 있습니다.

1. 아이콘 가장자리가 지저분합니다. 특히 일부 아이콘은 형체를 알아보기 어려울 만큼 이상하게 보입니다.
2. **좌클릭이 거의 동작하지 않습니다.** 블루투스만 반응하는데 메뉴가 매우 느리게 뜹니다.
3. 우클릭도 일부 앱에서만 동작합니다.

1번과 2·3번은 원인이 다릅니다. 따로 다룹니다.

---

## 1. 아이콘 픽셀

### 1-1. 원인

`src/tray_intercept.cpp`의 `IconToPng`(241행)가 이렇게 되어 있습니다.

```cpp
HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
const HDC dc = CreateCompatibleDC(nullptr);
const HGDIOBJ old = SelectObject(dc, bmp);
DrawIconEx(dc, 0, 0, icon, kIconPx, kIconPx, 0, nullptr, DI_NORMAL);
```

`DrawIconEx`로 24px DIB에 한 번 그리고 끝입니다. 다음이 전부 빠져 있습니다.

| 빠진 것 | 결과 |
|---|---|
| 알파 채널 처리 | GDI는 32비트 DIB에 그릴 때 알파 바이트를 보장하지 않습니다. 마스크 기반 레거시 아이콘은 알파가 0이거나 쓰레기 값이 됩니다 |
| 스트레이트/프리멀티플라이드 판별 | 둘을 섞으면 반투명 픽셀이 어둡거나 밝게 뜹니다 |
| 디프린지 | 가장자리에 검은 테두리나 색 번짐이 남습니다 |
| 원본 크기 확인 | 16px 원본을 24px로 늘리면서 GDI가 저품질로 확대합니다 |
| 여백 잘라내기 | 큰 캔버스에 작게 그려진 아이콘이 더 작아 보입니다 |

**독 아이콘에서 이미 겪고 해결한 문제입니다.** `src/icon_cache.cpp`에 검증된 파이프라인이 통째로 있고, `src/icon_cache.hpp`가 전부 공개하고 있습니다.

```cpp
HBITMAP BitmapFromIcon(HICON icon, int px);       // 원본 크기 확인 + WIC 축소 + 아래 마감
HBITMAP FinalizeIconBitmap(HBITMAP source, int px, bool straight_alpha);
void CropPaddedJumbo(BgraImage& image);
void ZeroTransparentRgb(BgraImage& image);
bool HasStraightAlpha(const BgraImage& image);
void StraightToPremul(BgraImage& image);
void DefringePremul(BgraImage& image);
```

### 1-2. 어떻게 고치는가

`IconToPng`가 `DrawIconEx`를 직접 쓰지 말고 **`icon_cache`의 파이프라인을 통과**하게 하십시오. 대략 이 흐름입니다.

```cpp
HBITMAP src = BitmapFromIcon(icon, kIconPx);   // 여기서 크기·알파·디프린지가 끝난다
BgraImage image;
BitmapToBgra(src, image);
DeleteObject(src);
// 알파 형식을 맞춘 뒤
EncodePng(image, out);
```

### 1-3. 반드시 확인할 함정: 알파 형식이 어긋납니다

**이 절을 건너뛰면 아이콘이 전부 어두워집니다.**

`EncodePng`(184행)는 `GUID_WICPixelFormat32bppBGRA`로 인코딩합니다. 이것은 **스트레이트 알파**입니다.

반면 `FinalizeIconBitmap`은 `StraightToPremul`과 `DefringePremul`을 거쳐 **프리멀티플라이드**로 끝납니다. `BitmapFromIcon`이 내부에서 `FinalizeIconBitmap`을 부르므로 결과도 프리멀티플라이드입니다.

프리멀티플라이드 픽셀을 그대로 `EncodePng`에 넘기면 반투명 영역이 원래보다 어둡게 저장됩니다.

**해결 방향을 직접 정하고 근거를 보고하십시오.** 두 가지가 있습니다.

- **(가) 되돌리기.** `icon_cache.cpp`에 `StraightToPremul`의 짝인 `PremulToStraight`를 더하고, `EncodePng` 직전에 부릅니다. 알파가 0인 픽셀은 RGB를 0으로 두십시오. 나누기 전에 0 나눗셈을 막아야 합니다.
- **(나) 앞에서 멈추기.** `BitmapFromIcon`을 쓰지 말고 그 안의 단계를 직접 조립해서 프리멀티플라이드로 넘어가기 전에 멈춥니다. 다만 `DefringePremul`이 프리멀티플라이드 데이터를 전제하므로 디프린지를 포기하게 됩니다.

**(가)를 권합니다.** 디프린지가 이번 증상의 "가장자리가 지저분함"에 직접 대응하기 때문입니다. `DefringePremul`을 포기하면 절반만 고치는 셈입니다.

### 1-4. `kIconPx`를 다시 정하십시오

`kIconPx = 24`(59행)가 고정입니다. 상단바가 아이콘을 실제로 몇 픽셀로 그리는지, 그리고 고DPI에서 얼마가 되는지 확인해서 정하십시오. **표시 크기보다 작으면 확대되어 흐려집니다.**

독이 `IconCache::Get(icon, px)`로 크기를 받는 구조를 참고하되, 지금 구조를 뜯어고치지는 마십시오. 상수를 표시 크기 이상으로 올리는 선에서 끝내고, 어떤 값으로 왜 정했는지 보고하십시오.

### 1-5. 눈으로 확인하십시오

고친 결과를 파일로 떨어뜨려 직접 보십시오. 진단용으로 각 항목의 PNG를 `%USERPROFILE%\.bamti\icondump\<uid>.png`에 저장하는 임시 경로를 두고, 확인이 끝나면 **그 코드를 지우고** 커밋 대상에서 빼십시오. 임시 코드를 남기지 마십시오.

---

## 2. 클릭 전달

### 2-1. 원인

`Invoke`(302행)가 버전을 **두 갈래로만** 나눕니다.

```cpp
if (version >= 4) {
  // MAKEWPARAM(x, y) / MAKELPARAM(down, uid)
  // down = WM_LBUTTONDOWN, up = WM_LBUTTONUP
} else {
  PostMessageW(owner, callback, uid, down);
  PostMessageW(owner, callback, uid, up);
  if (right) PostMessageW(owner, callback, uid, WM_CONTEXTMENU);
}
```

`Shell_NotifyIcon` 콜백 규약은 버전이 셋이고 **좌클릭 이벤트가 버전마다 다릅니다.**

| 버전 | 좌클릭에 보내는 것 | 우클릭에 보내는 것 | 인자 패킹 |
|---|---|---|---|
| 0 (`NIM_SETVERSION` 없음) | `WM_LBUTTONDOWN` → `WM_LBUTTONUP` | `WM_RBUTTONDOWN` → `WM_RBUTTONUP` | `wParam` = uid, `lParam` = 메시지 |
| 3 (`NOTIFYICON_VERSION`) | `WM_LBUTTONDOWN` → **`NIN_SELECT`** | `WM_RBUTTONDOWN` → **`WM_CONTEXTMENU`** | `wParam` = uid, `lParam` = 메시지 |
| 4 (`NOTIFYICON_VERSION_4`) | **`NIN_SELECT`** | **`WM_CONTEXTMENU`** | `wParam` = `MAKEWPARAM(x, y)`, `lParam` = `MAKELPARAM(메시지, uid)` |

`NIN_SELECT`는 `WM_USER + 0`(0x0400)이고 `NIN_KEYSELECT`는 `WM_USER + 1`입니다.

**여기서 두 가지 결함이 나옵니다.**

첫째, **버전 3에 `WM_LBUTTONUP`을 보내고 있습니다.** 버전 3으로 등록한 앱은 `NIN_SELECT`만 처리하므로 좌클릭에 아무 반응이 없습니다. 버전 3은 가장 흔한 등록 방식이며, 이것이 "좌클릭이 거의 동작하지 않는" 이유입니다. 버전 4 갈래도 `WM_LBUTTONUP`을 보내므로 같은 문제가 있습니다.

둘째, **버전 0과 3에서 우클릭 때 `WM_RBUTTONDOWN`·`WM_RBUTTONUP`·`WM_CONTEXTMENU`를 전부 보냅니다.** 버전 0 앱은 `WM_RBUTTONUP`으로 한 번, 버전 3 앱은 `WM_CONTEXTMENU`로 한 번 반응하는데 우리가 셋을 다 보내므로 앱에 따라 메뉴가 두 번 뜨거나 두 번째 신호에 메뉴가 닫힙니다. 이것이 "우클릭이 일부만 동작하는" 이유일 가능성이 큽니다. 블루투스 아이콘이 느린 것도 같은 계열로 봅니다. explorer가 중복 신호를 순서대로 처리하느라 늦어지는 것입니다.

### 2-2. 어떻게 고치는가

**버전을 셋으로 나누고, 각 버전이 규정한 것만 보내십시오.** 지금처럼 여러 개를 함께 보내 맞기를 기대하는 방식은 버리십시오.

- 버전 0: 좌클릭은 `WM_LBUTTONDOWN` 다음 `WM_LBUTTONUP`, 우클릭은 `WM_RBUTTONDOWN` 다음 `WM_RBUTTONUP`. `WM_CONTEXTMENU`를 보내지 마십시오.
- 버전 3: 좌클릭은 `WM_LBUTTONDOWN` 다음 `NIN_SELECT`, 우클릭은 `WM_RBUTTONDOWN` 다음 `WM_CONTEXTMENU`.
- 버전 4: 좌클릭은 `NIN_SELECT` 하나, 우클릭은 `WM_CONTEXTMENU` 하나. 앞의 버튼 다운을 보내지 마십시오.

버전 4의 좌표 패킹은 지금 방식(`MAKEWPARAM(x, y)`)을 유지합니다. 앱이 `GET_X_LPARAM`으로 부호 확장해서 읽으므로 음수 좌표도 맞습니다. 다만 좌표가 32767을 넘는 배치에서는 깨지므로, 그때 로그를 남기는 정도만 더하십시오. 지금 고치지는 마십시오.

`AllowSetForegroundWindow(pid)` 호출은 그대로 두십시오. 앱이 스스로 `SetForegroundWindow`를 부를 수 있게 하는 장치이며 필요합니다.

### 2-3. 버전이 실제로 무엇인지 확인해야 합니다

지금 로그로는 각 앱이 어느 버전으로 등록했는지 알 수 없습니다. 고치기 전에 이것부터 보이게 하십시오.

**`intercept item` 줄에 `version=`을 더하십시오.**

```
intercept item tip="KakaoTalk" exe=KakaoTalk.exe hwnd=0x2017A uid=222 guid=0 version=3
```

주의할 점이 있습니다. `NIM_SETVERSION`은 `NIM_ADD` **뒤에** 오는 별개의 메시지입니다. 항목 줄은 `NIM_ADD` 시점에 한 번만 남기므로 그때는 버전이 아직 0일 수 있습니다. 그러므로 항목 줄의 `version`은 참고값으로만 쓰고, **버전이 나중에 바뀌면 그 사실을 따로 한 줄 남기십시오.**

```
intercept version key=0x... uid=222 version=0->3
```

이 줄도 키당 한 번으로 제한하십시오.

### 2-4. 클릭마다 무엇을 보냈는지 남기십시오

"일부만 동작한다"를 가려내려면 클릭 시점의 기록이 필요합니다. `Invoke`가 성공하든 실패하든 한 줄 남기십시오.

```
intercept invoke key=0x... uid=222 version=3 right=0 event=NIN_SELECT owner=0x2017A posted=1
```

- `event`는 실제로 보낸 이벤트의 이름입니다. 숫자가 아니라 이름으로 적으십시오.
- `posted`는 `PostMessageW`의 반환값입니다. 실패하면 `GetLastError`도 함께 남기십시오.
- 이 줄은 사용자가 클릭할 때만 나오므로 폭주하지 않습니다. 횟수 제한을 걸지 마십시오.

### 2-5. 실패 경로를 조용히 넘기지 마십시오

`Invoke` 첫머리에서 소유 창을 찾지 못하거나 `callback == 0`이면 `false`만 돌려주고 아무 기록이 없습니다(320행). 왜 아무 일도 일어나지 않는지 알 수 없습니다. 이 경우에도 이유를 담아 한 줄 남기십시오.

```
intercept invoke skipped key=0x... reason=no_callback
```

---

## 3. 순서

**2절을 먼저 하십시오.** 클릭이 안 되는 것이 아이콘이 흐린 것보다 치명적입니다. 2-3절의 진단 로그를 넣고 실제 버전 분포를 확인한 뒤에 2-2절을 적용하면, 고친 것이 맞는지 근거를 갖고 판단할 수 있습니다.

1절은 그다음입니다. 1-3절의 알파 함정 때문에 한 번에 되지 않을 가능성이 높으므로 시간을 따로 잡으십시오.

---

## 4. 하지 않을 것

- `icon_cache.cpp`의 기존 함수 동작을 바꾸지 마십시오. 1-3절 (가)를 고르면 `PremulToStraight`를 **새로 더하는 것**은 되지만, 기존 함수를 고치면 독 아이콘이 함께 망가집니다.
- UIA 백엔드의 클릭 경로(`Invoke`)를 건드리지 마십시오. 이번 증상은 가로채기 백엔드에서만 나옵니다.
- 상단바의 그리기 코드와 `IconCache`의 캐시 구조를 바꾸지 마십시오.
- 선행 지시서들의 금지 사항은 그대로 유효합니다. explorer 메모리 접근, 권한 승격, 셸 대체는 금지입니다.
- **사용자 화면에 마우스나 키보드 입력을 합성하지 마십시오.** 클릭 검증은 사용자에게 부탁합니다.

---

## 5. 검증

### 5-1. Cursor가 직접 할 것

| 확인 항목 | 방법 |
|---|---|
| 빌드 | Release. 경고가 늘지 않아야 합니다 |
| 버전 분포 | bamti를 띄우고 `intercept item`의 `version=`과 `intercept version` 줄을 모아 보고하십시오 |
| 아이콘 픽셀 | 1-5절의 덤프를 열어 직접 보십시오. 가장자리에 검은 테두리가 없고 형체가 뚜렷해야 합니다 |
| 알파 형식 | 반투명 가장자리가 있는 아이콘을 골라 원본과 비교하십시오. 어두워졌으면 1-3절을 잘못 고른 것입니다 |

### 5-2. 사용자가 할 것

클릭은 사용자만 확인할 수 있습니다. 보고서에 다음을 표로 정리해서 사용자가 채울 수 있게 하십시오.

| 아이콘 | 등록 버전 | 좌클릭 | 우클릭 |
|---|---|---|---|
| KakaoTalk | | | |
| Tailscale | | | |
| Everything | | | |
| 오피스키퍼 | | | |
| Bluetooth | | | |
| 볼륨 | | | |

버전 열은 로그에서 읽어 미리 채워 두십시오. 사용자는 클릭 결과만 적으면 됩니다.

---

## 6. 보고

- 절별로 고친 파일과 함수
- 1-3절에서 (가)와 (나) 중 무엇을 골랐고 왜 그랬는지
- `kIconPx`를 얼마로 정했고 그 근거가 무엇인지
- 관측한 버전 분포. 어느 앱이 0이고 3이고 4인지
- 아이콘 덤프를 보고 판단한 결과. 무엇이 좋아졌고 무엇이 남았는지
- 5-2절 표를 채워 넣은 형태로

작업 트리는 커밋하지 말고 그대로 두십시오. 검수 후에 정리합니다.
