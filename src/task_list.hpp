#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <windows.h>

namespace bamti {

enum class DockItemKind { kApp, kSpotlight };

inline constexpr wchar_t kSpotlightPin[] = L"bamti.spotlight";

struct DockApp {
  DockItemKind kind = DockItemKind::kApp;
  std::wstring key;
  std::wstring exe_path;
  std::wstring aumid;
  std::wstring icon_resource;
  std::wstring display_name;
  std::wstring relaunch_command;
  HWND hwnd = nullptr;
  std::vector<HWND> windows;
  bool running = false;
  bool pinned = false;
  bool can_pin = false;
};

std::vector<std::wstring> LoadDockPins();
bool SaveDockPins(const std::vector<std::wstring>& paths);
size_t RepairDockPins(std::vector<std::wstring>& pins);

std::vector<DockApp> CollectDockApps(const std::vector<std::wstring>& pinned_paths);
uint64_t TaskWindowFingerprint();
void ForgetCachedWindow(HWND hwnd);

bool ActivateHwnd(HWND hwnd);
bool LaunchExe(const std::wstring& path);
bool LaunchDockApp(const DockApp& app);
std::wstring DockLaunchCommandLine(const DockApp& app);
void RestoreHwnds(const std::vector<HWND>& windows);
void HideHwnds(const std::vector<HWND>& windows);
// 바탕 화면을 드러내려면 치워야 하는 창. Z 순서 앞에서 뒤 순서다.
std::vector<HWND> CollectDesktopClearWindows();
void CloseHwnds(const std::vector<HWND>& windows);

DockApp MakeSpotlightDockApp();
bool IsSpotlightPin(const std::wstring& pin);
std::wstring DockPinId(const DockApp& app);
bool SameDockPin(const std::wstring& a, const std::wstring& b);
std::wstring DockPinCompareForm(const std::wstring& pin);
void ResetPinCmpLog();

std::wstring CanonicalPath(const std::wstring& path);
std::wstring WindowTitle(HWND hwnd);
bool IsSelfExecutable(const std::wstring& path);

}  // namespace bamti
