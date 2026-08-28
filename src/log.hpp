#pragma once

namespace bamti {

void LogInit();
void LogShutdown();
void Log(const wchar_t* area, const wchar_t* fmt, ...);
// Log()가 지금 뮤텍스를 쥔 채 실행 중인지 알려 준다. 감시 스레드 전용이다.
bool LogBusy();
void NotePostedStorm(const wchar_t* label, unsigned& count, unsigned long long& window_start);

}  // namespace bamti
