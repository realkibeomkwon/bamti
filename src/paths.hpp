#pragma once

#include <string>

namespace bamti {

std::wstring JoinPath(const std::wstring& dir, const wchar_t* file);
std::wstring DataDir();
std::wstring DockPinsPath();
std::wstring TaskbarGuardPath();
std::wstring WorkAreaGuardPath();
std::wstring LogFilePath();
std::wstring NightLightBackupPath();

}  // namespace bamti
