#include "winx_menu.hpp"

#include "log.hpp"

#include <windows.h>
#include <objbase.h>
#include <powrprof.h>
#include <reason.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <string>
#include <vector>

namespace bamti {
namespace {

struct ComScope {
  bool ok = false;
  bool uninit = false;

  ComScope() {
    const HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log(L"winx", L"CoInitializeEx hr=0x%08lx", static_cast<unsigned long>(hr));
    if (hr == S_OK) {
      ok = true;
      uninit = true;
    } else if (hr == S_FALSE || hr == RPC_E_CHANGED_MODE) {
      ok = true;
    }
  }

  ~ComScope() {
    if (uninit) {
      CoUninitialize();
    }
  }

  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;
};

bool EqualsIgnoreCase(const std::wstring& a, const wchar_t* b) {
  return lstrcmpiW(a.c_str(), b) == 0;
}

bool HasSuffixIgnoreCase(const std::wstring& text, const wchar_t* suffix) {
  const size_t n = wcslen(suffix);
  if (text.size() < n) {
    return false;
  }
  return lstrcmpiW(text.c_str() + (text.size() - n), suffix) == 0;
}

std::wstring FallbackLabel(const std::wstring& filename) {
  std::wstring name = filename;
  if (HasSuffixIgnoreCase(name, L".lnk")) {
    name.resize(name.size() - 4);
  }
  const size_t sep = name.find(L" - ");
  if (sep != std::wstring::npos) {
    return name.substr(sep + 3);
  }
  return name;
}

std::wstring LoadIndirect(const std::wstring& value) {
  if (value.empty() || value[0] != L'@') {
    return {};
  }
  wchar_t buf[1024]{};
  if (SUCCEEDED(SHLoadIndirectString(value.c_str(), buf, 1024, nullptr)) && buf[0] != L'\0') {
    return buf;
  }
  return {};
}

bool IsPowerShellLnk(const std::wstring& filename) {
  return filename.find(L"01a ") == 0 || filename.find(L"02a ") == 0 ||
         filename.find(L"Windows PowerShell") != std::wstring::npos;
}

bool IsControlPanelLnk(const std::wstring& filename) {
  return EqualsIgnoreCase(filename, L"4 - Control Panel.lnk");
}

bool QueryRunAsUser(const std::wstring& lnk_path, bool* runas) {
  if (runas == nullptr) {
    return false;
  }
  *runas = false;
  Microsoft::WRL::ComPtr<IShellLinkW> link;
  if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&link)))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IPersistFile> persist;
  if (FAILED(link.As(&persist)) || FAILED(persist->Load(lnk_path.c_str(), STGM_READ))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IShellLinkDataList> data;
  DWORD flags = 0;
  if (FAILED(link.As(&data)) || FAILED(data->GetFlags(&flags))) {
    return false;
  }
  *runas = (flags & SLDF_RUNAS_USER) != 0;
  return true;
}

bool WtHostPresent() {
  wchar_t found[MAX_PATH]{};
  const DWORD n = SearchPathW(nullptr, L"wt.exe", nullptr, MAX_PATH, found, nullptr);
  return n > 0 && n < MAX_PATH;
}

std::wstring WinXRoot() {
  wchar_t local[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", local, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return {};
  }
  return std::wstring(local) + L"\\Microsoft\\Windows\\WinX";
}

std::vector<std::wstring> ListLnkFiles(const std::wstring& dir) {
  std::vector<std::wstring> files;
  WIN32_FIND_DATAW fd{};
  const HANDLE find = FindFirstFileW((dir + L"\\*.lnk").c_str(), &fd);
  if (find == INVALID_HANDLE_VALUE) {
    return files;
  }
  do {
    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
      files.push_back(fd.cFileName);
    }
  } while (FindNextFileW(find, &fd));
  FindClose(find);
  std::sort(files.begin(), files.end(), std::greater<std::wstring>());
  return files;
}

bool PowerDryRun() {
  wchar_t value[8]{};
  const DWORD n = GetEnvironmentVariableW(L"BAMTI_POWER_DRYRUN", value, 8);
  return n > 0 && n < 8 && lstrcmpW(value, L"1") == 0;
}

const wchar_t* PowerActionName(PowerAction action) {
  switch (action) {
    case PowerAction::kLogoff:
      return L"logoff";
    case PowerAction::kSleep:
      return L"sleep";
    case PowerAction::kHibernate:
      return L"hibernate";
    case PowerAction::kShutdown:
      return L"shutdown";
    case PowerAction::kRestart:
      return L"restart";
  }
  return L"unknown";
}

bool EnableShutdownPrivilege() {
  HANDLE token = nullptr;
  if (OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &token) == FALSE) {
    Log(L"winx", L"OpenProcessToken failed err=%lu", GetLastError());
    return false;
  }
  TOKEN_PRIVILEGES tp{};
  tp.PrivilegeCount = 1;
  tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
  if (LookupPrivilegeValueW(nullptr, SE_SHUTDOWN_NAME, &tp.Privileges[0].Luid) == FALSE) {
    Log(L"winx", L"LookupPrivilegeValue failed err=%lu", GetLastError());
    CloseHandle(token);
    return false;
  }
  SetLastError(ERROR_SUCCESS);
  const BOOL adjusted = AdjustTokenPrivileges(token, FALSE, &tp, sizeof(tp), nullptr, nullptr);
  const DWORD err = GetLastError();
  CloseHandle(token);
  if (adjusted == FALSE) {
    Log(L"winx", L"AdjustTokenPrivileges failed err=%lu", err);
    return false;
  }
  if (err != ERROR_SUCCESS) {
    Log(L"winx", L"AdjustTokenPrivileges incomplete err=%lu", err);
    return false;
  }
  return true;
}

}  // namespace

std::vector<WinXEntry> LoadWinXEntries() {
  ComScope com;
  if (!com.ok) {
    Log(L"winx", L"COM unavailable");
    return {};
  }

  const std::wstring root = WinXRoot();
  if (root.empty()) {
    Log(L"winx", L"LOCALAPPDATA missing");
    return {};
  }

  const bool have_wt = WtHostPresent();
  Log(L"winx", L"wt.exe present=%d", have_wt ? 1 : 0);

  std::vector<WinXEntry> entries;
  for (int group = 3; group >= 1; --group) {
    const std::wstring dir = root + L"\\Group" + std::to_wstring(group);
    const std::wstring ini = dir + L"\\desktop.ini";
    const std::vector<std::wstring> files = ListLnkFiles(dir);
    for (const std::wstring& name : files) {
      wchar_t raw[1024]{};
      GetPrivateProfileStringW(L"LocalizedFileNames", name.c_str(), L"", raw, 1024, ini.c_str());
      std::wstring label = LoadIndirect(raw);
      if (IsControlPanelLnk(name)) {
        Log(L"winx", L"control panel label=%s", label.empty() ? L"" : label.c_str());
      }
      if (label.empty()) {
        label = FallbackLabel(name);
        Log(L"winx", L"fallback label file=%s name=%s", name.c_str(), label.c_str());
      }

      WinXEntry entry;
      entry.label = std::move(label);
      entry.lnk_path = dir + L"\\" + name;
      entry.group = group;

      if (IsPowerShellLnk(name)) {
        bool runas = false;
        if (QueryRunAsUser(entry.lnk_path, &runas)) {
          entry.admin = runas;
          Log(L"winx", L"powershell file=%s runas=%d", name.c_str(), runas ? 1 : 0);
        } else {
          Log(L"winx", L"powershell file=%s runas query failed", name.c_str());
        }
        if (have_wt) {
          entry.label = entry.admin ? L"터미널(관리자)" : L"터미널";
        }
      }

      entries.push_back(std::move(entry));
    }
  }
  return entries;
}

bool HibernateAvailable() {
  SYSTEM_POWER_CAPABILITIES caps{};
  if (GetPwrCapabilities(&caps) == FALSE) {
    Log(L"winx", L"GetPwrCapabilities failed err=%lu", GetLastError());
    return false;
  }
  const bool available = caps.SystemS4 != FALSE && caps.HiberFilePresent != FALSE;
  Log(L"winx", L"hibernate SystemS4=%d HiberFilePresent=%d available=%d", caps.SystemS4 ? 1 : 0,
      caps.HiberFilePresent ? 1 : 0, available ? 1 : 0);
  return available;
}

void InvokePowerAction(PowerAction action) {
  const wchar_t* name = PowerActionName(action);
  const bool need_privilege =
      action == PowerAction::kLogoff || action == PowerAction::kShutdown || action == PowerAction::kRestart;
  bool privilege_ok = true;
  if (need_privilege) {
    privilege_ok = EnableShutdownPrivilege();
  }
  if (PowerDryRun()) {
    Log(L"winx", L"power dryrun action=%s privilege=%d", name, privilege_ok ? 1 : 0);
    return;
  }
  if (need_privilege && !privilege_ok) {
    return;
  }
  const DWORD reason = SHTDN_REASON_MAJOR_OTHER | SHTDN_REASON_FLAG_PLANNED;
  switch (action) {
    case PowerAction::kLogoff:
      ExitWindowsEx(EWX_LOGOFF, reason);
      break;
    case PowerAction::kSleep:
      SetSuspendState(FALSE, FALSE, FALSE);
      break;
    case PowerAction::kHibernate:
      SetSuspendState(TRUE, FALSE, FALSE);
      break;
    case PowerAction::kShutdown:
      ExitWindowsEx(EWX_SHUTDOWN | EWX_POWEROFF, reason);
      break;
    case PowerAction::kRestart:
      ExitWindowsEx(EWX_REBOOT, reason);
      break;
  }
}

void LaunchWinXEntry(const WinXEntry& entry) {
  if (entry.lnk_path.empty()) {
    return;
  }
  ShellExecuteW(nullptr, nullptr, entry.lnk_path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace bamti
