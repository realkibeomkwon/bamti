#include "autostart.hpp"

#include "log.hpp"

#include <windows.h>

#include <string>
#include <vector>

namespace bamti {
namespace {

constexpr wchar_t kRunSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"bamti";

std::wstring ModuleCommand() {
  std::wstring path(MAX_PATH, L'\0');
  for (;;) {
    const DWORD n = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
    if (n == 0) {
      return {};
    }
    if (n < path.size()) {
      path.resize(n);
      break;
    }
    path.assign(path.size() * 2, L'\0');
  }
  if (path.empty()) {
    return {};
  }
  return L"\"" + path + L"\"";
}

bool RunValueExists() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD size = 0;
  const LONG st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
  RegCloseKey(key);
  return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool StartupApprovedAllows() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return true;
  }
  DWORD type = 0;
  DWORD size = 0;
  LONG st = RegQueryValueExW(key, kValueName, nullptr, &type, nullptr, &size);
  if (st != ERROR_SUCCESS || type != REG_BINARY || size == 0) {
    RegCloseKey(key);
    return true;
  }
  std::vector<BYTE> data(size);
  st = RegQueryValueExW(key, kValueName, nullptr, &type, data.data(), &size);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || size == 0) {
    return true;
  }
  return (data[0] & 1u) == 0;
}

bool IsBamtiRunName(const wchar_t* name) {
  return name != nullptr && wcscmp(name, kValueName) == 0;
}

void DeleteApprovedValue(const wchar_t* name) {
  if (!IsBamtiRunName(name)) {
    return;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedSubkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  const LONG st = RegDeleteValueW(key, name);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
    Log(L"host", L"autostart approved delete failed err=%lu", static_cast<unsigned long>(st));
  }
}

}  // namespace

bool AutostartEnabled() {
  return RunValueExists() && StartupApprovedAllows();
}

bool SetAutostart(bool on) {
  HKEY key = nullptr;
  LONG st = RegCreateKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr);
  if (st != ERROR_SUCCESS) {
    Log(L"host", L"autostart registry open failed err=%lu", static_cast<unsigned long>(st));
    return false;
  }
  if (on) {
    const std::wstring command = ModuleCommand();
    if (command.empty()) {
      RegCloseKey(key);
      Log(L"host", L"autostart module path failed err=%lu", GetLastError());
      return false;
    }
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    st = RegSetValueExW(key, kValueName, 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    RegCloseKey(key);
    if (st != ERROR_SUCCESS) {
      Log(L"host", L"autostart set failed err=%lu", static_cast<unsigned long>(st));
      return false;
    }
    Log(L"host", L"autostart added cmd=%s", command.c_str());
    DeleteApprovedValue(kValueName);
    return true;
  }
  st = RegDeleteValueW(key, kValueName);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
    Log(L"host", L"autostart delete failed err=%lu", static_cast<unsigned long>(st));
    return false;
  }
  Log(L"host", L"autostart removed");
  return true;
}

}  // namespace bamti
