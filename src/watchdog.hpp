#pragma once

#include <windows.h>

namespace bamti {

// UI 스레드가 지금 무엇을 하고 있는지 나타낸다.
// 문자열 리터럴만 넘긴다. 할당하지 않으므로 어느 지점에서 불러도 안전하다.
void WatchdogStage(const wchar_t* stage);

// 감시 스레드를 시작하고 멈춘다. hwnd는 UI 스레드가 소유한 창이다.
bool WatchdogStart(HWND ui_window);
void WatchdogStop();

}  // namespace bamti
