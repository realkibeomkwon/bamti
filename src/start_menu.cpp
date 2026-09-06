#include "start_menu.hpp"

#include "log.hpp"

#include <windows.h>
#include <powrprof.h>
#include <shellapi.h>

#include <string>

namespace bamti {
namespace {

void LaunchPath(const std::wstring& path) {
  if (path.empty()) {
    return;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  ShellExecuteExW(&info);
}

void SendWinChord(WORD vk) {
  INPUT keys[4]{};
  keys[0].type = INPUT_KEYBOARD;
  keys[0].ki.wVk = VK_LWIN;
  keys[1].type = INPUT_KEYBOARD;
  keys[1].ki.wVk = vk;
  keys[2].type = INPUT_KEYBOARD;
  keys[2].ki.wVk = vk;
  keys[2].ki.dwFlags = KEYEVENTF_KEYUP;
  keys[3].type = INPUT_KEYBOARD;
  keys[3].ki.wVk = VK_LWIN;
  keys[3].ki.dwFlags = KEYEVENTF_KEYUP;
  SendInput(4, keys, sizeof(INPUT));
}

bool EnableShutdownPrivilege() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
    Log(L"start", L"OpenProcessToken failed err=%lu", GetLastError());
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) == FALSE) {
    Log(L"start", L"LookupPrivilegeValue failed err=%lu", GetLastError());
    CloseHandle(token);
    return false;
  }
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  const DWORD err = GetLastError();
  CloseHandle(token);
  if (adjusted == FALSE) {
    Log(L"start", L"AdjustTokenPrivileges failed err=%lu", err);
    return false;
  }
  if (err != ERROR_SUCCESS) {
    Log(L"start", L"AdjustTokenPrivileges incomplete err=%lu", err);
    return false;
  }
  return true;
}

}  // namespace

void InvokeStartAction(StartAction action) {
  switch (action) {
    case StartAction::kExplorer:
      LaunchPath(L"explorer.exe");
      break;
    case StartAction::kSettings:
      LaunchPath(L"ms-settings:");
      break;
    case StartAction::kRun:
      SendWinChord(L'R');
      break;
    case StartAction::kSleep:
      SetSuspendState(FALSE, TRUE, FALSE);
      break;
    case StartAction::kRestart:
      if (EnableShutdownPrivilege()) {
        ExitWindowsEx(EWX_REBOOT, 0);
      }
      break;
    case StartAction::kShutdown:
      if (EnableShutdownPrivilege()) {
        ExitWindowsEx(EWX_SHUTDOWN, 0);
      }
      break;
  }
}

}  // namespace bamti
