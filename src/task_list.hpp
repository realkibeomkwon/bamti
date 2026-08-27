#pragma once

#include <string>
#include <vector>
#include <windows.h>

namespace bamti {

struct DockApp {
  std::wstring key;
  std::wstring exe_path;
  std::wstring aumid;
  std::wstring icon_resource;
  std::wstring display_name;
  HWND hwnd = nullptr;
  std::vector<HWND> windows;
  bool running = false;
  bool pinned = false;
  bool can_pin = false;
};

std::vector<std::wstring> LoadDockPins();
bool SaveDockPins(const std::vector<std::wstring>& paths);

std::vector<DockApp> CollectDockApps(const std::vector<std::wstring>& pinned_paths);

bool ActivateHwnd(HWND hwnd);
bool LaunchExe(const std::wstring& path);
void CloseHwnds(const std::vector<HWND>& windows);

std::wstring CanonicalPath(const std::wstring& path);
std::wstring WindowTitle(HWND hwnd);
bool IsSelfExecutable(const std::wstring& path);

}  // namespace bamti
