#pragma once

#include <windows.h>

namespace bamti {

bool IsTrueFullscreen(HWND self);
bool StartFullscreenWatch(HWND notify, UINT msg);
void StopFullscreenWatch(HWND notify);

}  // namespace bamti
