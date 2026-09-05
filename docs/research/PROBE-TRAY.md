# 알림 영역 탐침 결과

> 조사 당시의 측정 기록입니다. 현재 코드의 설명이 아닙니다.

측정 일시: 2026-08-28 (현지 21:09 / 21:11)
OS: Windows 11 Pro, RtlGetVersion 10.0.26200
아키텍처: x64 (IsWow64Process2 process_machine=UNKNOWN, native=AMD64)
권한: elevated=no
보고서 파일: `%USERPROFILE%\.bamti\probe-tray.txt` (실행마다 덮어씀). 아래는 복사본.

측정 시 알림 영역에 보이던 항목 (PrintWindow 캡처 및 UIA SystemTrayIcon / NotifyItemIcon):

1. 숨겨진 아이콘 표시
2. Windows Update (다시 시작 필요)
3. 트레이 입력 표시기 한/영 전환
4. 네트워크 (kibeomkwon_5G, Tailscale)
5. 볼륨
6. 배터리 99%
7. 시계
8. 바탕 화면 보기

오버플로 창은 상태를 바꾸지 않기 위해 열지 않았다. TopLevelWindowForOverflowXamlIsland의 ControlView 자식은 닫힌 상태에서 비어 있었다.

## 판정

| 항목 | 결과 |
|---|---|
| ToolbarWindow32 버튼 수 (상주 중 / 정상) | 없음 / 없음 |
| 검증 통과 버튼 수 | 0 |
| UIA 트레이 아이콘 후보 수 | 상주 중 51 / 정상 76 (3-2 트리 HWND를 여러 번 순회한 중복 포함). AutomationId가 SystemTrayIcon 또는 NotifyItemIcon인 고유 이름은 8개 |
| PrintWindow 비검정 픽셀 비율 (상주 중 / 정상) | TrayNotifyWnd 0.00% / 99.78%. Shell_TrayWnd 0.00% / 99.98% |
| 계획 문서 5-0절 판정표 적용 결과 | UIA |

적용 메모 (코드가 아니라 사람이 표에 맞춘 것):

- 첫 줄(레거시): ToolbarWindow32가 두 상태 모두 없고 검증 통과 버튼이 0이므로 `tray_backend_toolbar.cpp`를 만들지 않는다.
- 둘째 줄: UIA ControlView가 알림 영역 버튼을 열거한다. 상주 중 SW_HIDE 주차에서는 Shell_TrayWnd 루트만 나오고, 같은 트리의 Windows.UI.Composition.DesktopWindowContentBridge에서 버튼이 보인다. Invoke 패턴은 조회만 했고 호출하지 않았다.
- 셋째 줄(축소 대안): 태스크바가 보이는 정상 상태에서 PrintWindow(PW_RENDERFULLCONTENT)는 검지 않다. 상주 중의 0%는 SW_HIDE 때문이다. 화면 밖이되 보이는 주차(kParkedVisible)는 탐침이 시스템 상태를 바꾸지 않아 측정하지 못했다. 미러는 포기하지 않고 UIA 백엔드를 만들며, 픽셀은 5-7절의 kParkedVisible에 의존한다.

## 원본 보고서

## 1. 환경
local=2026-08-28 21:09:49.018
utc=2026-08-28 12:09:49.018
os=10.0.26200 platform=2
arch compile=x64 process_machine=0x0000(UNKNOWN) native_machine=0x8664(AMD64)
elevated=no
bamti_resident=no mutex=Local\bamti.singleton
handles_start=153

## 2. 창 트리
형식: depth  hwnd  class  title  style  exstyle  rect(l,t,r,b)  visible  pid

### Shell_TrayWnd hwnd=0x10124
0  0x10124  Shell_TrayWnd  ""  0x96000000  0x00000088  rect(0,2088,5120,2160)  yes  9692
1  0x10278  Windows.UI.Composition.DesktopWindowContentBridge  "DesktopWindowXamlSource"  0x50000000  0x00200000  rect(0,2088,5120,2160)  yes  9692
2  0x1027A  Windows.UI.Input.InputSite.WindowClass  ""  0x50000000  0x00000000  rect(0,2088,0,2088)  yes  9692
1  0x10270  Windows.UI.Core.CoreWindow  "DesktopWindowXamlSource"  0x44000000  0x00200080  rect(0,2088,65535,67623)  no  9692
1  0x10138  Start  "시작"  0x44000000  0x00000000  rect(1998,2088,2066,2160)  no  9692
1  0x1013A  TrayDummySearchControl  ""  0x50010000  0x00000000  rect(0,2088,0,2088)  yes  9692
1  0x1013C  TrayNotifyWnd  ""  0x54000000  0x00000000  rect(4670,2088,5120,2160)  yes  9692
1  0x10142  ReBarWindow32  ""  0x5600B25D  0x00000080  rect(2066,2088,2924,2160)  yes  9692
2  0x1014C  MSTaskSwWClass  "실행 중인 응용 프로그램"  0x56010000  0x00000000  rect(2066,2088,2924,2160)  yes  9692
3  0x10150  MSTaskListWClass  "실행 중인 응용 프로그램"  0x46000000  0x00000000  rect(2066,2088,2924,2160)  no  9692

### TopLevelWindowForOverflowXamlIsland hwnd=0x903D0
0  0x903D0  TopLevelWindowForOverflowXamlIsland  "시스템 트레이 오버플로 창입니다."  0x84000000  0x00200088  rect(4518,1917,4869,2088)  no  9692
1  0x60074  Windows.UI.Composition.DesktopWindowContentBridge  "DesktopWindowXamlSource"  0x50000000  0x00200000  rect(4518,1917,4869,2088)  no  9692
2  0x3033C  Windows.UI.Input.InputSite.WindowClass  ""  0x50000000  0x00000000  rect(4518,1917,4518,1917)  no  9692
Shell_SecondaryTrayWnd: 없음
NotifyIconOverflowWindow: 없음
nodes=13 elapsed_ms=16

## 3. ToolbarWindow32
형식: hwnd  부모 경로  pid  버튼수  실패사유
ToolbarWindow32: 없음

## 4. 버튼 데이터
sizeof(TBBUTTON)=32 sizeof(TrayItemData)=32
해당 없음
검증 통과 버튼 수 합계=0

## 5. UI Automation

### Shell_TrayWnd hwnd=0x10124
0  Pane(50033)  name="작업 표시줄"  AutomationId=""  ClassName="Shell_TrayWnd"  BoundingRectangle(0,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name="팝업 호스트"  AutomationId=""  ClassName="Xaml_WindowedPopupClass"  BoundingRectangle(0,2088,3413,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Window(50032)  name="팝업"  AutomationId=""  ClassName="Popup"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  ToolTip(50022)  name=""  AutomationId=""  ClassName="ToolTip"  BoundingRectangle(2253,2022,2431,2071)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
4  Text(50020)  name="Kibeom - Chrome"  AutomationId=""  ClassName="TextBlock"  BoundingRectangle(2268,2033,2415,2057)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(0,2088,3413,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Pane(50033)  name=""  AutomationId="TaskbarFrame"  ClassName="Taskbar.TaskbarFrameAutomationPeer"  BoundingRectangle(0,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="시작"  AutomationId="StartButton"  ClassName="ToggleButton"  BoundingRectangle(1998,2088,2066,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="검색"  AutomationId="SearchButton"  ClassName="ToggleButton"  BoundingRectangle(2066,2088,2132,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
4  Image(50006)  name=""  AutomationId="Icon"  ClassName="Microsoft.UI.Xaml.Controls.AnimatedVisualPlayer"  BoundingRectangle(2081,2106,2117,2142)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="제어판 고정됨"  AutomationId="Appid: Microsoft.Windows.ControlPanel"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2132,2088,2198,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="파일 탐색기 고정됨"  AutomationId="Appid: Microsoft.Windows.Explorer"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2198,2088,2264,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Kibeom (IMTS) - Chrome 고정됨"  AutomationId="Appid: Chrome"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2264,2088,2330,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Kibeom - Chrome 고정됨"  AutomationId="Appid: Chrome.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2330,2088,2396,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Firefox - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: 308046B0AF4A39CB"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2396,2088,2462,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Outlook 고정됨"  AutomationId="Appid: Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2462,2088,2528,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Microsoft Teams (work or school) 고정됨"  AutomationId="Appid: MSTeams_8wekyb3d8bbwe!MSTeams"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2528,2088,2594,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="GitKraken 고정됨"  AutomationId="Appid: com.squirrel.gitkraken.gitkraken"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2594,2088,2660,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Zed - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: ZedIndustries.Zed"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2660,2088,2726,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Visual Studio 2022 고정됨"  AutomationId="Appid: VisualStudio.71ed4dcb"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2726,2088,2792,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Cursor 고정됨"  AutomationId="Appid: Anysphere.Cursor"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2792,2088,2858,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Claude - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: Claude_pzs8sxrjxfjjc!CLAUDE"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2858,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Grok 고정됨"  AutomationId="Appid: Chrome._crx_ggjoclfcnjemagj.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2924,2088,2990,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="카카오톡 - 2개의 실행 중인 창 고정됨"  AutomationId="Appid: {6D809377-6AF0-444B-8957-A3773F02200E}\Kakao\KakaoTalk\KakaoTalk.exe"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2990,2088,3056,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="터미널 - 1개의 실행 중인 창"  AutomationId="Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(3056,2088,3122,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="숨겨진 아이콘 표시"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4669,2088,4717,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4717,2088,4765,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4729,2112,4753,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="트레이 입력 표시기 한/영 전환"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4765,2088,4813,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4777,2112,4801,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4813,2088,4855,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButtonCenter"  BoundingRectangle(4855,2088,4891,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="힘 배터리 상태: 99% 남음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4891,2088,4979,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="시계 오후 9:09:49 ‎2026-‎8-‎28"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButton"  BoundingRectangle(4979,2088,5102,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="바탕 화면 보기"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.ShowDesktopButton"  BoundingRectangle(5102,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="TrayNotifyWnd"  BoundingRectangle(4670,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskSwWClass"  BoundingRectangle(2066,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### TopLevelWindowForOverflowXamlIsland hwnd=0x903D0
0  Pane(50033)  name="시스템 트레이 오버플로 창입니다."  AutomationId=""  ClassName="TopLevelWindowForOverflowXamlIsland"  BoundingRectangle(4518,1917,4869,2088)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Composition.DesktopWindowContentBridge hwnd=0x10278
0  Pane(50033)  name="DesktopWindowXamlSource"  AutomationId=""  ClassName="Windows.UI.Composition.DesktopWindowContentBridge"  BoundingRectangle(0,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(0,2088,3413,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Pane(50033)  name=""  AutomationId="TaskbarFrame"  ClassName="Taskbar.TaskbarFrameAutomationPeer"  BoundingRectangle(0,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="시작"  AutomationId="StartButton"  ClassName="ToggleButton"  BoundingRectangle(1998,2088,2066,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="검색"  AutomationId="SearchButton"  ClassName="ToggleButton"  BoundingRectangle(2066,2088,2132,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
4  Image(50006)  name=""  AutomationId="Icon"  ClassName="Microsoft.UI.Xaml.Controls.AnimatedVisualPlayer"  BoundingRectangle(2081,2106,2117,2142)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="제어판 고정됨"  AutomationId="Appid: Microsoft.Windows.ControlPanel"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2132,2088,2198,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="파일 탐색기 고정됨"  AutomationId="Appid: Microsoft.Windows.Explorer"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2198,2088,2264,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Kibeom (IMTS) - Chrome 고정됨"  AutomationId="Appid: Chrome"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2264,2088,2330,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Kibeom - Chrome 고정됨"  AutomationId="Appid: Chrome.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2330,2088,2396,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Firefox - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: 308046B0AF4A39CB"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2396,2088,2462,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Outlook 고정됨"  AutomationId="Appid: Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2462,2088,2528,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Microsoft Teams (work or school) 고정됨"  AutomationId="Appid: MSTeams_8wekyb3d8bbwe!MSTeams"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2528,2088,2594,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="GitKraken 고정됨"  AutomationId="Appid: com.squirrel.gitkraken.gitkraken"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2594,2088,2660,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Zed - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: ZedIndustries.Zed"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2660,2088,2726,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Visual Studio 2022 고정됨"  AutomationId="Appid: VisualStudio.71ed4dcb"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2726,2088,2792,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Cursor 고정됨"  AutomationId="Appid: Anysphere.Cursor"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2792,2088,2858,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Claude - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: Claude_pzs8sxrjxfjjc!CLAUDE"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2858,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Grok 고정됨"  AutomationId="Appid: Chrome._crx_ggjoclfcnjemagj.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2924,2088,2990,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="카카오톡 - 2개의 실행 중인 창 고정됨"  AutomationId="Appid: {6D809377-6AF0-444B-8957-A3773F02200E}\Kakao\KakaoTalk\KakaoTalk.exe"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2990,2088,3056,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="터미널 - 1개의 실행 중인 창"  AutomationId="Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(3056,2088,3122,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="숨겨진 아이콘 표시"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4669,2088,4717,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4717,2088,4765,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4729,2112,4753,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="트레이 입력 표시기 한/영 전환"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4765,2088,4813,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4777,2112,4801,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4813,2088,4855,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButtonCenter"  BoundingRectangle(4855,2088,4891,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="힘 배터리 상태: 99% 남음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4891,2088,4979,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="시계 오후 9:09:49 ‎2026-‎8-‎28"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButton"  BoundingRectangle(4979,2088,5102,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="바탕 화면 보기"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.ShowDesktopButton"  BoundingRectangle(5102,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="TrayNotifyWnd"  BoundingRectangle(4670,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskSwWClass"  BoundingRectangle(2066,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Input.InputSite.WindowClass hwnd=0x1027A
0  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(0,2088,3413,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId="TaskbarFrame"  ClassName="Taskbar.TaskbarFrameAutomationPeer"  BoundingRectangle(0,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="시작"  AutomationId="StartButton"  ClassName="ToggleButton"  BoundingRectangle(1998,2088,2066,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="검색"  AutomationId="SearchButton"  ClassName="ToggleButton"  BoundingRectangle(2066,2088,2132,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId="Icon"  ClassName="Microsoft.UI.Xaml.Controls.AnimatedVisualPlayer"  BoundingRectangle(2081,2106,2117,2142)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="제어판 고정됨"  AutomationId="Appid: Microsoft.Windows.ControlPanel"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2132,2088,2198,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="파일 탐색기 고정됨"  AutomationId="Appid: Microsoft.Windows.Explorer"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2198,2088,2264,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Kibeom (IMTS) - Chrome 고정됨"  AutomationId="Appid: Chrome"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2264,2088,2330,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Kibeom - Chrome 고정됨"  AutomationId="Appid: Chrome.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2330,2088,2396,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Firefox - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: 308046B0AF4A39CB"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2396,2088,2462,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Outlook 고정됨"  AutomationId="Appid: Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2462,2088,2528,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Microsoft Teams (work or school) 고정됨"  AutomationId="Appid: MSTeams_8wekyb3d8bbwe!MSTeams"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2528,2088,2594,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="GitKraken 고정됨"  AutomationId="Appid: com.squirrel.gitkraken.gitkraken"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2594,2088,2660,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Zed - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: ZedIndustries.Zed"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2660,2088,2726,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Visual Studio 2022 고정됨"  AutomationId="Appid: VisualStudio.71ed4dcb"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2726,2088,2792,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Cursor 고정됨"  AutomationId="Appid: Anysphere.Cursor"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2792,2088,2858,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Claude - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: Claude_pzs8sxrjxfjjc!CLAUDE"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2858,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Grok 고정됨"  AutomationId="Appid: Chrome._crx_ggjoclfcnjemagj.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2924,2088,2990,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="카카오톡 - 2개의 실행 중인 창 고정됨"  AutomationId="Appid: {6D809377-6AF0-444B-8957-A3773F02200E}\Kakao\KakaoTalk\KakaoTalk.exe"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2990,2088,3056,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="터미널 - 1개의 실행 중인 창"  AutomationId="Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(3056,2088,3122,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="숨겨진 아이콘 표시"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4669,2088,4717,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4717,2088,4765,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4729,2112,4753,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="트레이 입력 표시기 한/영 전환"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4765,2088,4813,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4777,2112,4801,2136)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4813,2088,4855,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButtonCenter"  BoundingRectangle(4855,2088,4891,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="힘 배터리 상태: 99% 남음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4891,2088,4979,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="시계 오후 9:09:49 ‎2026-‎8-‎28"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButton"  BoundingRectangle(4979,2088,5102,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="바탕 화면 보기"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.ShowDesktopButton"  BoundingRectangle(5102,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Core.CoreWindow hwnd=0x10270
0  Pane(50033)  name="DesktopWindowXamlSource"  AutomationId=""  ClassName="Windows.UI.Core.CoreWindow"  BoundingRectangle(0,2088,65535,67623)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Start hwnd=0x10138
0  Button(50000)  name="시작"  AutomationId=""  ClassName="Start"  BoundingRectangle(1998,2088,2066,2160)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no

### TrayDummySearchControl hwnd=0x1013A
0  Pane(50033)  name=""  AutomationId="4100"  ClassName="TrayDummySearchControl"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### TrayNotifyWnd hwnd=0x1013C
0  Pane(50033)  name=""  AutomationId=""  ClassName="TrayNotifyWnd"  BoundingRectangle(4670,2088,5120,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### ReBarWindow32 hwnd=0x10142
0  Pane(50033)  name=""  AutomationId="40965"  ClassName="ReBarWindow32"  BoundingRectangle(2066,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskSwWClass"  BoundingRectangle(2066,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### MSTaskSwWClass hwnd=0x1014C
0  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskSwWClass"  BoundingRectangle(2066,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### MSTaskListWClass hwnd=0x10150
0  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskListWClass"  BoundingRectangle(2066,2088,2924,2160)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Composition.DesktopWindowContentBridge hwnd=0x60074
0  Pane(50033)  name="DesktopWindowXamlSource"  AutomationId=""  ClassName="Windows.UI.Composition.DesktopWindowContentBridge"  BoundingRectangle(4518,1917,4869,2088)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(4518,1917,4752,2031)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Everything"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name=""  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="KakaoTalk"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="오피스키퍼"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name=" Tailscale: Connected. Click for options."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Bluetooth 장치"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Input.InputSite.WindowClass hwnd=0x3033C
0  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(4518,1917,4752,2031)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="Everything"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name=""  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="KakaoTalk"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="오피스키퍼"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name=" Tailscale: Connected. Click for options."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="Bluetooth 장치"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
nodes=136 elapsed_ms=407
tray_icon_candidates=76
  candidate_name="시작"
  candidate_name="검색"
  candidate_name="제어판 고정됨"
  candidate_name="파일 탐색기 고정됨"
  candidate_name="Kibeom (IMTS) - Chrome 고정됨"
  candidate_name="Kibeom - Chrome 고정됨"
  candidate_name="Firefox - 1개의 실행 중인 창 고정됨"
  candidate_name="Outlook 고정됨"
  candidate_name="Microsoft Teams (work or school) 고정됨"
  candidate_name="GitKraken 고정됨"
  candidate_name="Zed - 1개의 실행 중인 창 고정됨"
  candidate_name="Visual Studio 2022 고정됨"
  candidate_name="Cursor 고정됨"
  candidate_name="Claude - 1개의 실행 중인 창 고정됨"
  candidate_name="Grok 고정됨"
  candidate_name="카카오톡 - 2개의 실행 중인 창 고정됨"
  candidate_name="터미널 - 1개의 실행 중인 창"
  candidate_name="숨겨진 아이콘 표시"
  candidate_name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."
  candidate_name="트레이 입력 표시기 한/영 전환"
  candidate_name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"
  candidate_name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"
  candidate_name="힘 배터리 상태: 99% 남음"
  candidate_name="시계 오후 9:09:49 ‎2026-‎8-‎28"
  candidate_name="바탕 화면 보기"
  candidate_name="시작"
  candidate_name="검색"
  candidate_name="제어판 고정됨"
  candidate_name="파일 탐색기 고정됨"
  candidate_name="Kibeom (IMTS) - Chrome 고정됨"
  candidate_name="Kibeom - Chrome 고정됨"
  candidate_name="Firefox - 1개의 실행 중인 창 고정됨"
  candidate_name="Outlook 고정됨"
  candidate_name="Microsoft Teams (work or school) 고정됨"
  candidate_name="GitKraken 고정됨"
  candidate_name="Zed - 1개의 실행 중인 창 고정됨"
  candidate_name="Visual Studio 2022 고정됨"
  candidate_name="Cursor 고정됨"
  candidate_name="Claude - 1개의 실행 중인 창 고정됨"
  candidate_name="Grok 고정됨"
  candidate_name="카카오톡 - 2개의 실행 중인 창 고정됨"
  candidate_name="터미널 - 1개의 실행 중인 창"
  candidate_name="숨겨진 아이콘 표시"
  candidate_name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."
  candidate_name="트레이 입력 표시기 한/영 전환"
  candidate_name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"
  candidate_name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"
  candidate_name="힘 배터리 상태: 99% 남음"
  candidate_name="시계 오후 9:09:49 ‎2026-‎8-‎28"
  candidate_name="바탕 화면 보기"
  candidate_name="시작"
  candidate_name="검색"
  candidate_name="제어판 고정됨"
  candidate_name="파일 탐색기 고정됨"
  candidate_name="Kibeom (IMTS) - Chrome 고정됨"
  candidate_name="Kibeom - Chrome 고정됨"
  candidate_name="Firefox - 1개의 실행 중인 창 고정됨"
  candidate_name="Outlook 고정됨"
  candidate_name="Microsoft Teams (work or school) 고정됨"
  candidate_name="GitKraken 고정됨"
  candidate_name="Zed - 1개의 실행 중인 창 고정됨"
  candidate_name="Visual Studio 2022 고정됨"
  candidate_name="Cursor 고정됨"
  candidate_name="Claude - 1개의 실행 중인 창 고정됨"
  candidate_name="Grok 고정됨"
  candidate_name="카카오톡 - 2개의 실행 중인 창 고정됨"
  candidate_name="터미널 - 1개의 실행 중인 창"
  candidate_name="숨겨진 아이콘 표시"
  candidate_name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."
  candidate_name="트레이 입력 표시기 한/영 전환"
  candidate_name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"
  candidate_name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"
  candidate_name="힘 배터리 상태: 99% 남음"
  candidate_name="시계 오후 9:09:49 ‎2026-‎8-‎28"
  candidate_name="바탕 화면 보기"
  candidate_name="시작"

## 6. PrintWindow
notify_area_hwnd=0x1013C first_tray_hwnd=0x10124
target hwnd=0x1013C class=TrayNotifyWnd visible=yes rect(4670,2088,5120,2160)
  PrintWindow=yes timeout=no flags=0x2 size=450x72
  non_black_pct=99.78 alpha_nz_pct=100.00 pixels=32400
  png=C:\Users\KIBEOMKWON\.bamti\probe-tray.png saved=yes
shell_tray hwnd=0x10124 class=Shell_TrayWnd visible=yes rect(0,2088,5120,2160)
  PrintWindow=yes timeout=no flags=0x2 size=5120x72
  non_black_pct=99.98 alpha_nz_pct=100.00 pixels=368640

handles_end=259
elapsed_ms=469

## 상주 중 보고서

## 1. 환경
local=2026-08-28 21:11:24.631
utc=2026-08-28 12:11:24.631
os=10.0.26200 platform=2
arch compile=x64 process_machine=0x0000(UNKNOWN) native_machine=0x8664(AMD64)
elevated=no
bamti_resident=yes mutex=Local\bamti.singleton
handles_start=153

## 2. 창 트리
형식: depth  hwnd  class  title  style  exstyle  rect(l,t,r,b)  visible  pid

### Shell_TrayWnd hwnd=0x10124
0  0x10124  Shell_TrayWnd  ""  0x86000000  0x00000088  rect(0,32000,5120,32072)  no  9692
1  0x10278  Windows.UI.Composition.DesktopWindowContentBridge  "DesktopWindowXamlSource"  0x40000000  0x00200000  rect(0,32000,5120,32072)  no  9692
2  0x1027A  Windows.UI.Input.InputSite.WindowClass  ""  0x50000000  0x00000000  rect(0,32000,0,32000)  no  9692
1  0x10270  Windows.UI.Core.CoreWindow  "DesktopWindowXamlSource"  0x44000000  0x00200080  rect(0,32000,65535,97535)  no  9692
1  0x10138  Start  "시작"  0x44000000  0x00000000  rect(1998,32000,2066,32072)  no  9692
1  0x1013A  TrayDummySearchControl  ""  0x50010000  0x00000000  rect(0,32000,0,32000)  no  9692
1  0x1013C  TrayNotifyWnd  ""  0x54000000  0x00000000  rect(4670,32000,5120,32072)  no  9692
1  0x10142  ReBarWindow32  ""  0x5600B25D  0x00000080  rect(2066,32000,2924,32072)  no  9692
2  0x1014C  MSTaskSwWClass  "실행 중인 응용 프로그램"  0x56010000  0x00000000  rect(2068,32000,2924,32060)  no  9692
3  0x10150  MSTaskListWClass  "실행 중인 응용 프로그램"  0x46000000  0x00000000  rect(2068,32000,2924,32060)  no  9692

### TopLevelWindowForOverflowXamlIsland hwnd=0x903D0
0  0x903D0  TopLevelWindowForOverflowXamlIsland  "시스템 트레이 오버플로 창입니다."  0x84000000  0x00200088  rect(4518,1917,4869,2088)  no  9692
1  0x60074  Windows.UI.Composition.DesktopWindowContentBridge  "DesktopWindowXamlSource"  0x50000000  0x00200000  rect(4518,1917,4869,2088)  no  9692
2  0x3033C  Windows.UI.Input.InputSite.WindowClass  ""  0x50000000  0x00000000  rect(4518,1917,4518,1917)  no  9692
Shell_SecondaryTrayWnd: 없음
NotifyIconOverflowWindow: 없음
nodes=13 elapsed_ms=0

## 3. ToolbarWindow32
형식: hwnd  부모 경로  pid  버튼수  실패사유
ToolbarWindow32: 없음

## 4. 버튼 데이터
sizeof(TBBUTTON)=32 sizeof(TrayItemData)=32
해당 없음
검증 통과 버튼 수 합계=0

## 5. UI Automation

### Shell_TrayWnd hwnd=0x10124
0  Pane(50033)  name="작업 표시줄"  AutomationId=""  ClassName="Shell_TrayWnd"  BoundingRectangle(0,32000,5120,32072)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### TopLevelWindowForOverflowXamlIsland hwnd=0x903D0
0  Pane(50033)  name="시스템 트레이 오버플로 창입니다."  AutomationId=""  ClassName="TopLevelWindowForOverflowXamlIsland"  BoundingRectangle(4518,1917,4869,2088)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Composition.DesktopWindowContentBridge hwnd=0x10278
0  Pane(50033)  name="DesktopWindowXamlSource"  AutomationId=""  ClassName="Windows.UI.Composition.DesktopWindowContentBridge"  BoundingRectangle(0,32000,5120,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(0,32000,3413,32048)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Pane(50033)  name=""  AutomationId="TaskbarFrame"  ClassName="Taskbar.TaskbarFrameAutomationPeer"  BoundingRectangle(0,32000,5120,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="시작"  AutomationId="StartButton"  ClassName="ToggleButton"  BoundingRectangle(1998,32000,2066,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="검색"  AutomationId="SearchButton"  ClassName="ToggleButton"  BoundingRectangle(2066,32000,2132,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
4  Image(50006)  name=""  AutomationId="Icon"  ClassName="Microsoft.UI.Xaml.Controls.AnimatedVisualPlayer"  BoundingRectangle(2081,32018,2117,32054)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="제어판 고정됨"  AutomationId="Appid: Microsoft.Windows.ControlPanel"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2132,32000,2198,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="파일 탐색기 고정됨"  AutomationId="Appid: Microsoft.Windows.Explorer"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2198,32000,2264,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Kibeom (IMTS) - Chrome 고정됨"  AutomationId="Appid: Chrome"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2264,32000,2330,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Kibeom - Chrome 고정됨"  AutomationId="Appid: Chrome.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2330,32000,2396,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Firefox - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: 308046B0AF4A39CB"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2396,32000,2462,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Outlook 고정됨"  AutomationId="Appid: Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2462,32000,2528,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Microsoft Teams (work or school) 고정됨"  AutomationId="Appid: MSTeams_8wekyb3d8bbwe!MSTeams"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2528,32000,2594,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="GitKraken 고정됨"  AutomationId="Appid: com.squirrel.gitkraken.gitkraken"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2594,32000,2660,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Zed - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: ZedIndustries.Zed"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2660,32000,2726,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Visual Studio 2022 고정됨"  AutomationId="Appid: VisualStudio.71ed4dcb"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2726,32000,2792,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Cursor 고정됨"  AutomationId="Appid: Anysphere.Cursor"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2792,32000,2858,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Claude - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: Claude_pzs8sxrjxfjjc!CLAUDE"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2858,32000,2924,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="Grok 고정됨"  AutomationId="Appid: Chrome._crx_ggjoclfcnjemagj.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2924,32000,2990,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="카카오톡 - 3개의 실행 중인 창 고정됨"  AutomationId="Appid: {6D809377-6AF0-444B-8957-A3773F02200E}\Kakao\KakaoTalk\KakaoTalk.exe"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2990,32000,3056,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Button(50000)  name="터미널 - 1개의 실행 중인 창"  AutomationId="Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(3056,32000,3122,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="숨겨진 아이콘 표시"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4669,32000,4717,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4717,32000,4765,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4729,32024,4753,32048)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="트레이 입력 표시기 한/영 전환"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4765,32000,4813,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4777,32024,4801,32048)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4813,32000,4855,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButtonCenter"  BoundingRectangle(4855,32000,4891,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="힘 배터리 상태: 99% 남음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4891,32000,4979,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="시계 오후 9:11:24 ‎2026-‎8-‎28"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButton"  BoundingRectangle(4979,32000,5102,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="바탕 화면 보기"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.ShowDesktopButton"  BoundingRectangle(5102,32000,5120,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Input.InputSite.WindowClass hwnd=0x1027A
0  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(0,32000,3413,32048)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId="TaskbarFrame"  ClassName="Taskbar.TaskbarFrameAutomationPeer"  BoundingRectangle(0,32000,5120,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="시작"  AutomationId="StartButton"  ClassName="ToggleButton"  BoundingRectangle(1998,32000,2066,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="검색"  AutomationId="SearchButton"  ClassName="ToggleButton"  BoundingRectangle(2066,32000,2132,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId="Icon"  ClassName="Microsoft.UI.Xaml.Controls.AnimatedVisualPlayer"  BoundingRectangle(2081,32018,2117,32054)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="제어판 고정됨"  AutomationId="Appid: Microsoft.Windows.ControlPanel"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2132,32000,2198,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="파일 탐색기 고정됨"  AutomationId="Appid: Microsoft.Windows.Explorer"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2198,32000,2264,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Kibeom (IMTS) - Chrome 고정됨"  AutomationId="Appid: Chrome"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2264,32000,2330,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Kibeom - Chrome 고정됨"  AutomationId="Appid: Chrome.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2330,32000,2396,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Firefox - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: 308046B0AF4A39CB"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2396,32000,2462,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Outlook 고정됨"  AutomationId="Appid: Microsoft.OutlookForWindows_8wekyb3d8bbwe!Microsoft.OutlookforWindows"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2462,32000,2528,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Microsoft Teams (work or school) 고정됨"  AutomationId="Appid: MSTeams_8wekyb3d8bbwe!MSTeams"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2528,32000,2594,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="GitKraken 고정됨"  AutomationId="Appid: com.squirrel.gitkraken.gitkraken"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2594,32000,2660,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Zed - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: ZedIndustries.Zed"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2660,32000,2726,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Visual Studio 2022 고정됨"  AutomationId="Appid: VisualStudio.71ed4dcb"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2726,32000,2792,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Cursor 고정됨"  AutomationId="Appid: Anysphere.Cursor"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2792,32000,2858,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Claude - 1개의 실행 중인 창 고정됨"  AutomationId="Appid: Claude_pzs8sxrjxfjjc!CLAUDE"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2858,32000,2924,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Grok 고정됨"  AutomationId="Appid: Chrome._crx_ggjoclfcnjemagj.UserData.Profile1"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2924,32000,2990,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="카카오톡 - 3개의 실행 중인 창 고정됨"  AutomationId="Appid: {6D809377-6AF0-444B-8957-A3773F02200E}\Kakao\KakaoTalk\KakaoTalk.exe"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(2990,32000,3056,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="터미널 - 1개의 실행 중인 창"  AutomationId="Appid: Microsoft.WindowsTerminal_8wekyb3d8bbwe!App"  ClassName="Taskbar.TaskListButtonAutomationPeer"  BoundingRectangle(3056,32000,3122,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="숨겨진 아이콘 표시"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4669,32000,4717,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4717,32000,4765,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4729,32024,4753,32048)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="트레이 입력 표시기 한/영 전환"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(4765,32000,4813,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(4777,32024,4801,32048)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4813,32000,4855,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButtonCenter"  BoundingRectangle(4855,32000,4891,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="힘 배터리 상태: 99% 남음"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.AccentButton"  BoundingRectangle(4891,32000,4979,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="시계 오후 9:11:24 ‎2026-‎8-‎28"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.OmniButton"  BoundingRectangle(4979,32000,5102,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="바탕 화면 보기"  AutomationId="SystemTrayIcon"  ClassName="SystemTray.ShowDesktopButton"  BoundingRectangle(5102,32000,5120,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Core.CoreWindow hwnd=0x10270
0  Pane(50033)  name="DesktopWindowXamlSource"  AutomationId=""  ClassName="Windows.UI.Core.CoreWindow"  BoundingRectangle(0,32000,65535,97535)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Start hwnd=0x10138
0  Button(50000)  name="시작"  AutomationId=""  ClassName="Start"  BoundingRectangle(1998,32000,2066,32072)  IsOffscreen=no
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no

### TrayDummySearchControl hwnd=0x1013A
0  Pane(50033)  name=""  AutomationId="4100"  ClassName="TrayDummySearchControl"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### TrayNotifyWnd hwnd=0x1013C
0  Pane(50033)  name=""  AutomationId=""  ClassName="TrayNotifyWnd"  BoundingRectangle(4670,32000,5120,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### ReBarWindow32 hwnd=0x10142
0  Pane(50033)  name=""  AutomationId="40965"  ClassName="ReBarWindow32"  BoundingRectangle(2066,32000,2924,32072)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### MSTaskSwWClass hwnd=0x1014C
0  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskSwWClass"  BoundingRectangle(2068,32000,2924,32060)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### MSTaskListWClass hwnd=0x10150
0  Pane(50033)  name="실행 중인 응용 프로그램"  AutomationId=""  ClassName="MSTaskListWClass"  BoundingRectangle(2068,32000,2924,32060)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Composition.DesktopWindowContentBridge hwnd=0x60074
0  Pane(50033)  name="DesktopWindowXamlSource"  AutomationId=""  ClassName="Windows.UI.Composition.DesktopWindowContentBridge"  BoundingRectangle(4518,1917,4869,2088)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(4518,1917,4752,2031)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Everything"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name=""  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="KakaoTalk"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="오피스키퍼"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name=" Tailscale: Connected. Click for options."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
2  Button(50000)  name="Bluetooth 장치"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
3  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no

### Windows.UI.Input.InputSite.WindowClass hwnd=0x3033C
0  Pane(50033)  name=""  AutomationId=""  ClassName="Windows.UI.Input.InputSite.WindowClass"  BoundingRectangle(4518,1917,4752,2031)  IsOffscreen=no
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="Everything"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name=""  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="KakaoTalk"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="오피스키퍼"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name=" Tailscale: Connected. Click for options."  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
1  Button(50000)  name="Bluetooth 장치"  AutomationId="NotifyItemIcon"  ClassName="SystemTray.NormalButton"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=yes  LegacyIAccessible=yes  ExpandCollapse=no
2  Image(50006)  name=""  AutomationId=""  ClassName="Image"  BoundingRectangle(0,0,0,0)  IsOffscreen=yes
       patterns: Invoke=no  LegacyIAccessible=yes  ExpandCollapse=no
nodes=97 elapsed_ms=390
tray_icon_candidates=51
  candidate_name="시작"
  candidate_name="검색"
  candidate_name="제어판 고정됨"
  candidate_name="파일 탐색기 고정됨"
  candidate_name="Kibeom (IMTS) - Chrome 고정됨"
  candidate_name="Kibeom - Chrome 고정됨"
  candidate_name="Firefox - 1개의 실행 중인 창 고정됨"
  candidate_name="Outlook 고정됨"
  candidate_name="Microsoft Teams (work or school) 고정됨"
  candidate_name="GitKraken 고정됨"
  candidate_name="Zed - 1개의 실행 중인 창 고정됨"
  candidate_name="Visual Studio 2022 고정됨"
  candidate_name="Cursor 고정됨"
  candidate_name="Claude - 1개의 실행 중인 창 고정됨"
  candidate_name="Grok 고정됨"
  candidate_name="카카오톡 - 3개의 실행 중인 창 고정됨"
  candidate_name="터미널 - 1개의 실행 중인 창"
  candidate_name="숨겨진 아이콘 표시"
  candidate_name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."
  candidate_name="트레이 입력 표시기 한/영 전환"
  candidate_name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"
  candidate_name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"
  candidate_name="힘 배터리 상태: 99% 남음"
  candidate_name="시계 오후 9:11:24 ‎2026-‎8-‎28"
  candidate_name="바탕 화면 보기"
  candidate_name="시작"
  candidate_name="검색"
  candidate_name="제어판 고정됨"
  candidate_name="파일 탐색기 고정됨"
  candidate_name="Kibeom (IMTS) - Chrome 고정됨"
  candidate_name="Kibeom - Chrome 고정됨"
  candidate_name="Firefox - 1개의 실행 중인 창 고정됨"
  candidate_name="Outlook 고정됨"
  candidate_name="Microsoft Teams (work or school) 고정됨"
  candidate_name="GitKraken 고정됨"
  candidate_name="Zed - 1개의 실행 중인 창 고정됨"
  candidate_name="Visual Studio 2022 고정됨"
  candidate_name="Cursor 고정됨"
  candidate_name="Claude - 1개의 실행 중인 창 고정됨"
  candidate_name="Grok 고정됨"
  candidate_name="카카오톡 - 3개의 실행 중인 창 고정됨"
  candidate_name="터미널 - 1개의 실행 중인 창"
  candidate_name="숨겨진 아이콘 표시"
  candidate_name="업데이트를 설치하려면 장치를 다시 시작해야 합니다.  다시 시작할 시간을 선택하세요."
  candidate_name="트레이 입력 표시기 한/영 전환"
  candidate_name="네트워크 kibeomkwon_5G 인터넷 액세스  Tailscale 인터넷 액세스 없음"
  candidate_name="볼륨 DELL U4025QW(2- HD Audio Driver for Display Audio): 100%"
  candidate_name="힘 배터리 상태: 99% 남음"
  candidate_name="시계 오후 9:11:24 ‎2026-‎8-‎28"
  candidate_name="바탕 화면 보기"
  candidate_name="시작"

## 6. PrintWindow
notify_area_hwnd=0x1013C first_tray_hwnd=0x10124
target hwnd=0x1013C class=TrayNotifyWnd visible=no rect(4670,32000,5120,32072)
  PrintWindow=yes timeout=no flags=0x2 size=450x72
  non_black_pct=0.00 alpha_nz_pct=0.00 pixels=32400
  png=C:\Users\KIBEOMKWON\.bamti\probe-tray.png saved=yes
shell_tray hwnd=0x10124 class=Shell_TrayWnd visible=no rect(0,32000,5120,32072)
  PrintWindow=yes timeout=no flags=0x2 size=5120x72
  non_black_pct=0.00 alpha_nz_pct=0.00 pixels=368640

handles_end=259
elapsed_ms=422
