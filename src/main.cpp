#include "host.hpp"

#include <windows.h>

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  HeapSetInformation(nullptr, HeapEnableTerminationOnCorruption, nullptr, 0);
  return bamti::Run(instance);
}
