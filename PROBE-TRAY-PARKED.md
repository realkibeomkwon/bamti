# 주차 상태 캡처 탐침 결과

측정 일시: 2026-08-29 (현지 10:24)
OS: Windows 11 Pro, RtlGetVersion 10.0.26200
아키텍처: x64 (IsWow64Process2 process_machine=UNKNOWN, native=AMD64)
권한: elevated=no
bamti 상주: no (`Local\bamti.singleton` 없음)
보고서 파일: `%USERPROFILE%\.bamti\probe-tray-parked.txt` (실행마다 덮어씀). 아래는 복사본.
PNG: `%USERPROFILE%\.bamti\probe-parked-bridge.png`, `probe-parked-notify.png`

측정 명령: `bamti.exe --probe-tray-parked`

측정 시 알림 영역에 보이던 항목 (UIA, AutomationId가 SystemTrayIcon 또는 NotifyItemIcon):

1. 숨겨진 아이콘 표시
2. 트레이 입력 표시기 한/영 전환
3. 네트워크 (kibeomkwon_5G, Tailscale)
4. 볼륨
5. 배터리 100%
6. 시계
7. 바탕 화면 보기

서드파티 `NotifyItemIcon`은 보이지 않았다. 오버플로 창은 열지 않았다.

## 판정

| 항목 | 결과 |
|---|---|
| 주차 (`SetWindowPos` y=32000, `ShowWindow` 없음) | Win32 `GetWindowRect`는 즉시 `(0,32000)-(5120,32072)` |
| 주차 동안 화면 가장자리 | 원래 자리 `rect(0,2158,5120,2230)`의 화면 픽셀이 3초 동안 바뀌지 않음 (mean abs diff max=0.00). 샘플 50회 모두 가장자리에 태스크바가 남아 있음 |
| 복귀 | `rect(0,2158,5120,2230)`로 되돌아옴. `pos_ok=yes` |
| UIA 버튼 수 | 7 (모두 `SystemTrayIcon`). 열거 31ms |
| UIA BoundingRectangle | 주차 좌표(32000대)를 따르지 않고 원래 화면 y=2158에 머물렀다 |
| DesktopWindowContentBridge PrintWindow | 성공, timeout 없음, 5120x72, 비검정 0.00%, 알파 비0 100.00% |
| TrayNotifyWnd PrintWindow | 성공, timeout 없음, 423x72, 비검정 0.00%, 알파 비0 100.00% |
| 아이콘 사각형 표준편차 ≥ 8 | 0 / 7 (bridge), 0 / 7 (notify). 전부 0.00 |
| 2-2절 표 적용 결과 | **캡처 경로 포기. 4-5절 글리프 폴백만 구현한다.** |

적용 메모 (코드가 아니라 사람이 표에 맞춘 것):

- 아이콘 사각형 안쪽 표준편차가 8 이상인 사각형이 절반을 넘지 않는다. 두 창 모두 캡처 대상이 될 수 없다.
- PNG는 사람 눈으로도 완전 검정이다. 경계값 판정이 필요 없다.
- 캡처 경로를 포기한 결론은 그대로다. 원인은 확정하지 못했다. 관측된 것은 태스크바를 `SetWindowPos`로 옮긴 뒤에는 대상 창이 원위치로 돌아온 상태에서도 `PrintWindow`가 검정을 돌려준다는 사실이다. 근거: 3·4절의 `Shell_TrayWnd` rect `(0,32000,5120,32072)`, 5절 UIA `BoundingRectangle` y=2158, 6절 캡처 대상 rect `(0,2158,5120,2230)`인데도 `non_black_pct=0.00`. 정상 상태 `PROBE-TRAY.md`의 같은 `TrayNotifyWnd`는 99.78%였다.
- 따라서 14절 3번 커밋(`HideMode`)과 5번 커밋(캡처·배경 제거)은 건너뛴다.
- UIA 열거는 주차 뒤에도 동작한다(7개, 31ms). 글리프 폴백 미러의 열거 경로는 유효하다.

## 원본 보고서

## 1. 환경
local=2026-08-29 10:24:46.463
utc=2026-08-29 01:24:46.463
os=10.0.26200 platform=2
arch compile=x64 process_machine=0x0000(UNKNOWN) native_machine=0x8664(AMD64)
elevated=no
bamti_resident=no mutex=Local\bamti.singleton
handles_start=157

## 2. 주차 전 상태
hwnd=0x20794 class=Shell_TrayWnd visible=yes rect(0,2158,5120,2230)

## 3. 주차
mode=SetWindowPos y=32000 ShowWindow=no
hwnd=0x20794 SetWindowPos=yes now_rect(0,32000,5120,32072) visible=yes
wait_ms=3000 flash_samples=50 flash_hits=50 edge_mean_abs_diff_max=0.00 flashed=yes
park_ok=yes

## 4. 주차 후 대상 창
Shell_TrayWnd hwnd=0x20794 class=Shell_TrayWnd visible=yes rect(0,32000,5120,32072)
DesktopWindowContentBridge hwnd=0x30998 class=Windows.UI.Composition.DesktopWindowContentBridge visible=no rect(0,32000,5120,32072)
TrayNotifyWnd hwnd=0x20740 class=TrayNotifyWnd visible=yes rect(4698,32000,5120,32072)

## 5. UI Automation 알림 영역 버튼
elapsed_ms=31 count=7
형식: order  name  AutomationId  ClassName  BoundingRectangle  IsOffscreen
0  "숨겨진 아이콘 표시"  "SystemTrayIcon"  "SystemTray.NormalButton"  rect(4697,2158,4745,2230)  no
1  "트레이 입력 표시기 한/영 전환"  "SystemTrayIcon"  "SystemTray.NormalButton"  rect(4745,2158,4793,2230)  no
2  "네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  "SystemTrayIcon"  "SystemTray.AccentButton"  rect(4793,2158,4835,2230)  no
3  "볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 50%"  "SystemTrayIcon"  "SystemTray.OmniButtonCenter"  rect(4835,2158,4871,2230)  no
4  "힘 배터리 상태: 완전히 충전됨 100%"  "SystemTrayIcon"  "SystemTray.AccentButton"  rect(4871,2158,4969,2230)  no
5  "시계 오전 10:24:49 ‎2026-‎8-‎29"  "SystemTrayIcon"  "SystemTray.OmniButton"  rect(4969,2158,5102,2230)  no
6  "바탕 화면 보기"  "SystemTrayIcon"  "SystemTray.ShowDesktopButton"  rect(5102,2158,5120,2230)  no

## 6. PrintWindow
bridge hwnd=0x30998 class=Windows.UI.Composition.DesktopWindowContentBridge visible=no rect(0,2158,5120,2230)
  PrintWindow=yes timeout=no flags=0x2 size=5120x72
  non_black_pct=0.00 alpha_nz_pct=100.00 pixels=368640
  png=C:\Users\KIBEOMKWON\.bamti\probe-parked-bridge.png saved=yes
notify hwnd=0x20740 class=TrayNotifyWnd visible=yes rect(4697,2158,5120,2230)
  PrintWindow=yes timeout=no flags=0x2 size=423x72
  non_black_pct=0.00 alpha_nz_pct=100.00 pixels=30456
  png=C:\Users\KIBEOMKWON\.bamti\probe-parked-notify.png saved=yes

## 7. 아이콘 사각형 표준편차
형식: order  name  bridge(in,px,std_r,std_g,std_b,std_max)  notify(...)
0  "숨겨진 아이콘 표시"  bridge(yes,3456,0.00,0.00,0.00,0.00)  notify(yes,3456,0.00,0.00,0.00,0.00)
1  "트레이 입력 표시기 한/영 전환"  bridge(yes,3456,0.00,0.00,0.00,0.00)  notify(yes,3456,0.00,0.00,0.00,0.00)
2  "네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  bridge(yes,3024,0.00,0.00,0.00,0.00)  notify(yes,3024,0.00,0.00,0.00,0.00)
3  "볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 50%"  bridge(yes,2592,0.00,0.00,0.00,0.00)  notify(yes,2592,0.00,0.00,0.00,0.00)
4  "힘 배터리 상태: 완전히 충전됨 100%"  bridge(yes,7056,0.00,0.00,0.00,0.00)  notify(yes,7056,0.00,0.00,0.00,0.00)
5  "시계 오전 10:24:49 ‎2026-‎8-‎29"  bridge(yes,9576,0.00,0.00,0.00,0.00)  notify(yes,9576,0.00,0.00,0.00,0.00)
6  "바탕 화면 보기"  bridge(yes,1296,0.00,0.00,0.00,0.00)  notify(yes,1296,0.00,0.00,0.00,0.00)
considered=7 half=3 bridge_pass=0 notify_pass=0
capture_path=no target=none

## 8. 복귀
hwnd=0x20794 visible=yes rect(0,2158,5120,2230) pos_ok=yes orig_visible=yes
restore_ok=yes flashed_during_park=yes

handles_end=249
elapsed_ms=3062
