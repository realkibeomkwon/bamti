#pragma once

#include <windows.h>

namespace bamti {

// 트레이 아이콘을 대신 눌러 준 직후, 대상 앱이 띄우는 팝업이 상단바가 아니라 원래
// 알림 영역(화면 하단) 근처에서 열리는 경우가 있다. 짧은 시간 동안 새로 나타나는
// 최상위 팝업을 지켜보다가 그런 창을 아이콘 바로 아래로 옮긴다.
//
// anchor: 상단바에서 그 아이콘이 차지하는 화면 좌표 사각형.
// owner_pid: 아이콘을 등록한 프로세스. 모르면 0을 넘긴다(프로세스를 가리지 않는다).
//
// 반드시 메시지 펌프를 도는 UI 스레드에서 부를 것. WinEvent 훅은 설치한 스레드로만
// 콜백을 보낸다.
void TrayPopupGuardArm(const RECT& anchor, DWORD owner_pid);

// 프로세스를 내릴 때 훅과 타이머를 정리한다.
void TrayPopupGuardShutdown();

}  // namespace bamti
