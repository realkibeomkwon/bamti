#pragma once

namespace bamti {

// 나열 순서는 menu_bar.cpp의 kStartExplorerCmd~kStartShutdownCmd와 같아야 한다.
enum class StartAction { kExplorer, kSettings, kRun, kSleep, kRestart, kShutdown };

// 시작 메뉴 항목의 동작을 실행한다. 메뉴 창은 이 함수가 불리기 전에 이미 닫혀 있어야 한다.
void InvokeStartAction(StartAction action);

}  // namespace bamti
