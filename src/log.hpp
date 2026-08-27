#pragma once

namespace bamti {

void LogInit();
void LogShutdown();
void Log(const wchar_t* area, const wchar_t* fmt, ...);

}  // namespace bamti
