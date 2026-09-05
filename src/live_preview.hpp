#pragma once

#include <windows.h>

namespace bamti {

bool LivePreviewAvailable();
void SetLivePreview(bool on, HWND exclude);

}  // namespace bamti
