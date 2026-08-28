#pragma once

namespace bamti {

void LogInit();
void LogShutdown();
void Log(const wchar_t* area, const wchar_t* fmt, ...);
// 잠금을 얻지 못하면 기록을 포기하고 즉시 돌아온다. 감시 스레드 전용이다.
void LogTry(const wchar_t* tag, const wchar_t* fmt, ...);

}  // namespace bamti
