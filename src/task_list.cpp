#include "task_list.hpp"

#include <appmodel.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <propkey.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <unordered_map>

namespace bamti {
namespace {

constexpr wchar_t kPinFile[] = L"dock-pins.txt";

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
  return lstrcmpiW(a.c_str(), b.c_str()) == 0;
}

std::wstring JoinPath(const std::wstring& dir, const wchar_t* file) {
  std::wstring path = dir;
  if (!path.empty() && path.back() != L'\\') {
    path.push_back(L'\\');
  }
  path += file;
  return path;
}

std::wstring BamtiDir() {
  PWSTR root = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &root)) || root == nullptr) {
    return {};
  }
  std::wstring dir = root;
  CoTaskMemFree(root);
  dir = JoinPath(dir, L"bamti");
  CreateDirectoryW(dir.c_str(), nullptr);
  return dir;
}

std::wstring PinPath() {
  const std::wstring dir = BamtiDir();
  if (dir.empty()) {
    return {};
  }
  return JoinPath(dir, kPinFile);
}

std::wstring Lower(std::wstring text) {
  if (!text.empty()) {
    CharLowerBuffW(text.data(), static_cast<DWORD>(text.size()));
  }
  return text;
}

std::wstring FileStem(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos && EqualsIgnoreCase(name.substr(dot), L".exe")) {
    name.resize(dot);
  }
  return name;
}

bool SkipClass(const wchar_t* cls) {
  static const wchar_t* kSkip[] = {
      L"Shell_TrayWnd",
      L"Shell_SecondaryTrayWnd",
      L"NotifyIconOverflowWindow",
      L"Progman",
      L"WorkerW",
      L"ForegroundStaging",
      L"bamti.MenuBar",
      L"bamti.Dock",
      L"bamti.DockHot",
      L"bamti.StartMenu",
      L"IME",
      L"MSCTFIME UI",
      L"tooltips_class32",
  };
  for (const wchar_t* skip : kSkip) {
    if (lstrcmpiW(cls, skip) == 0) {
      return true;
    }
  }
  return false;
}

bool IsCloaked(HWND hwnd) {
  DWORD cloaked = 0;
  if (FAILED(DwmGetWindowAttribute(hwnd, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)))) {
    return false;
  }
  return cloaked != 0;
}

bool IsTaskWindow(HWND hwnd) {
  if (!IsWindow(hwnd) || GetAncestor(hwnd, GA_ROOT) != hwnd) {
    return false;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == GetCurrentProcessId()) {
    return false;
  }

  const LONG style = GetWindowLongW(hwnd, GWL_STYLE);
  const LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
  const bool iconic = IsIconic(hwnd) != FALSE;
  if ((style & WS_VISIBLE) == 0 && !iconic) {
    return false;
  }
  if ((ex & WS_EX_TOOLWINDOW) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
    return false;
  }
  if ((ex & WS_EX_APPWINDOW) == 0 && GetWindow(hwnd, GW_OWNER) != nullptr) {
    return false;
  }
  if (IsCloaked(hwnd)) {
    return false;
  }

  wchar_t cls[256]{};
  GetClassNameW(hwnd, cls, 256);
  if (cls[0] != L'\0' && _wcsnicmp(cls, L"bamti.", 6) == 0) {
    return false;
  }
  if (SkipClass(cls)) {
    return false;
  }
  return true;
}

bool OnCurrentDesktop(IVirtualDesktopManager* vdm, HWND hwnd) {
  if (vdm == nullptr) {
    return true;
  }
  BOOL on = TRUE;
  if (FAILED(vdm->IsWindowOnCurrentVirtualDesktop(hwnd, &on))) {
    return true;
  }
  return on != FALSE;
}

bool SkipChromeExe(const std::wstring& path) {
  const std::wstring stem = Lower(FileStem(path));
  static const wchar_t* kSkip[] = {
      L"bamti",
      L"searchhost",
      L"startmenuexperiencehost",
      L"shellexperiencehost",
      L"textinputhost",
      L"lockapp",
  };
  for (const wchar_t* skip : kSkip) {
    if (stem == skip) {
      return true;
    }
  }
  return false;
}

std::wstring WindowExePath(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0) {
    return {};
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    return {};
  }
  wchar_t buf[MAX_PATH]{};
  DWORD n = MAX_PATH;
  std::wstring path;
  if (QueryFullProcessImageNameW(process, 0, buf, &n) != FALSE) {
    path.assign(buf, n);
  } else {
    std::wstring grow(32768, L'\0');
    n = static_cast<DWORD>(grow.size());
    if (QueryFullProcessImageNameW(process, 0, grow.data(), &n) != FALSE) {
      grow.resize(n);
      path = std::move(grow);
    }
  }
  CloseHandle(process);
  return path;
}

bool IsHostExe(const std::wstring& path) {
  const std::wstring stem = Lower(FileStem(path));
  return stem == L"applicationframehost" || stem == L"wwahost" || stem == L"dllhost" ||
         stem == L"runtimebroker";
}

std::wstring WindowPropString(HWND hwnd, const PROPERTYKEY& key) {
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  if (FAILED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) || !store) {
    return {};
  }
  PROPVARIANT value;
  PropVariantInit(&value);
  std::wstring out;
  if (SUCCEEDED(store->GetValue(key, &value)) && value.vt == VT_LPWSTR && value.pwszVal != nullptr &&
      value.pwszVal[0] != L'\0') {
    out = value.pwszVal;
  }
  PropVariantClear(&value);
  return out;
}

std::wstring ProcessAumid(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  if (pid == 0) {
    return {};
  }
  HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
  if (process == nullptr) {
    return {};
  }
  UINT32 len = 0;
  LONG rc = GetApplicationUserModelId(process, &len, nullptr);
  std::wstring id;
  if (rc == ERROR_INSUFFICIENT_BUFFER && len > 1) {
    id.assign(len, L'\0');
    rc = GetApplicationUserModelId(process, &len, id.data());
    if (rc == ERROR_SUCCESS) {
      id.resize(wcsnlen(id.c_str(), id.size()));
    } else {
      id.clear();
    }
  }
  CloseHandle(process);
  return id;
}

std::wstring WindowAumid(HWND hwnd) {
  std::wstring aumid = WindowPropString(hwnd, PKEY_AppUserModel_ID);
  if (aumid.empty()) {
    aumid = ProcessAumid(hwnd);
  }
  return aumid;
}

std::wstring PathAumid(const std::wstring& path) {
  if (path.empty()) {
    return {};
  }
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  if (FAILED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))) ||
      !store) {
    return {};
  }
  PROPVARIANT value;
  PropVariantInit(&value);
  std::wstring out;
  if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR &&
      value.pwszVal != nullptr && value.pwszVal[0] != L'\0') {
    out = value.pwszVal;
  }
  PropVariantClear(&value);
  return out;
}

std::wstring LoadIndirect(const std::wstring& value) {
  if (value.empty()) {
    return {};
  }
  if (value[0] != L'@') {
    return value;
  }
  wchar_t buf[1024]{};
  if (SUCCEEDED(SHLoadIndirectString(value.c_str(), buf, 1024, nullptr)) && buf[0] != L'\0') {
    return buf;
  }
  return {};
}

std::wstring AppsFolderDisplayName(const std::wstring& aumid) {
  if (aumid.empty()) {
    return {};
  }
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, aumid.c_str(), IID_PPV_ARGS(&item))) || !item) {
    item.Reset();
    if (aumid.find(L'!') == std::wstring::npos) {
      const std::wstring alt = aumid + L"!App";
      SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, alt.c_str(), IID_PPV_ARGS(&item));
    }
  }
  if (!item) {
    const std::wstring parsing = L"shell:AppsFolder\\" + aumid;
    SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item));
  }
  if (!item) {
    return {};
  }
  PWSTR name = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) || name == nullptr) {
    return {};
  }
  std::wstring out = name;
  CoTaskMemFree(name);
  return out;
}

std::wstring DisplayNameFor(const std::wstring& path, const std::wstring& title) {
  if (!path.empty()) {
    SHFILEINFOW info{};
    if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) != 0 &&
        info.szDisplayName[0] != L'\0') {
      return info.szDisplayName;
    }
    const std::wstring stem = FileStem(path);
    if (!stem.empty() && !IsHostExe(path)) {
      return stem;
    }
  }
  return title;
}

struct Group {
  DockApp app;
};

}  // namespace

std::wstring CanonicalPath(const std::wstring& path) {
  if (path.empty()) {
    return {};
  }
  DWORD n = GetLongPathNameW(path.c_str(), nullptr, 0);
  std::wstring full;
  if (n > 1) {
    full.resize(n);
    n = GetLongPathNameW(path.c_str(), full.data(), n);
    if (n > 0) {
      full.resize(n);
    } else {
      full = path;
    }
  } else {
    full = path;
  }
  return Lower(std::move(full));
}

std::wstring WindowTitle(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return {};
  }
  const int n = GetWindowTextLengthW(hwnd);
  if (n <= 0) {
    return {};
  }
  std::wstring text(static_cast<size_t>(n) + 1, L'\0');
  const int written = GetWindowTextW(hwnd, text.data(), n + 1);
  if (written <= 0) {
    return {};
  }
  text.resize(static_cast<size_t>(written));
  return text;
}

bool IsSelfExecutable(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  wchar_t self[MAX_PATH]{};
  const DWORD n = GetModuleFileNameW(nullptr, self, MAX_PATH);
  if (n > 0 && n < MAX_PATH && CanonicalPath(path) == CanonicalPath(std::wstring(self, n))) {
    return true;
  }
  return Lower(FileStem(path)) == L"bamti";
}

std::vector<std::wstring> LoadDockPins() {
  std::vector<std::wstring> pins;
  const std::wstring path = PinPath();
  if (path.empty()) {
    return pins;
  }
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || file == nullptr) {
    return pins;
  }
  wchar_t line[1024]{};
  while (fgetws(line, 1024, file) != nullptr) {
    std::wstring text = line;
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r')) {
      text.pop_back();
    }
    if (text.empty() || text[0] == L'#' || text.rfind(L"v=", 0) == 0) {
      continue;
    }
    pins.push_back(std::move(text));
  }
  fclose(file);
  return pins;
}

bool SaveDockPins(const std::vector<std::wstring>& paths) {
  const std::wstring path = PinPath();
  if (path.empty()) {
    return false;
  }
  const std::wstring tmp = path + L".tmp";
  FILE* file = nullptr;
  if (_wfopen_s(&file, tmp.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr) {
    return false;
  }
  fwprintf(file, L"v=1\n");
  for (const auto& item : paths) {
    fwprintf(file, L"%s\n", item.c_str());
  }
  fclose(file);
  if (MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING) == FALSE) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

std::vector<DockApp> CollectDockApps(const std::vector<std::wstring>& pinned_paths) {
  Microsoft::WRL::ComPtr<IVirtualDesktopManager> vdm;
  CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&vdm));

  struct Raw {
    HWND hwnd;
    std::wstring path;
    std::wstring aumid;
    std::wstring title;
    std::wstring icon_resource;
    std::wstring relaunch_name;
  };
  std::vector<Raw> windows;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsTaskWindow(hwnd)) {
          return TRUE;
        }
        auto* out = reinterpret_cast<std::vector<Raw>*>(lp);
        Raw raw{};
        raw.hwnd = hwnd;
        raw.path = WindowExePath(hwnd);
        raw.aumid = WindowAumid(hwnd);
        if (raw.aumid.empty() && !raw.path.empty() && !IsHostExe(raw.path)) {
          raw.aumid = PathAumid(raw.path);
        }
        raw.title = WindowTitle(hwnd);
        raw.icon_resource = WindowPropString(hwnd, PKEY_AppUserModel_RelaunchIconResource);
        raw.relaunch_name = WindowPropString(hwnd, PKEY_AppUserModel_RelaunchDisplayNameResource);
        out->push_back(std::move(raw));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows));

  std::unordered_map<std::wstring, DockApp> groups;
  std::vector<std::wstring> order;

  auto take = [&](const std::wstring& key) -> DockApp& {
    auto it = groups.find(key);
    if (it == groups.end()) {
      DockApp app;
      app.key = key;
      groups.emplace(key, std::move(app));
      order.push_back(key);
      return groups[key];
    }
    return it->second;
  };

  auto ingest = [&](bool filter_desktop) {
    groups.clear();
    order.clear();
    for (const auto& raw : windows) {
      if (filter_desktop && !OnCurrentDesktop(vdm.Get(), raw.hwnd)) {
        continue;
      }
      if (!raw.path.empty() && (IsSelfExecutable(raw.path) || SkipChromeExe(raw.path))) {
        continue;
      }
      std::wstring key;
      if (!raw.aumid.empty()) {
        key = L"aumid:" + Lower(raw.aumid);
      } else if (!raw.path.empty()) {
        key = L"path:" + CanonicalPath(raw.path);
      } else {
        DWORD pid = 0;
        GetWindowThreadProcessId(raw.hwnd, &pid);
        key = L"pid:" + std::to_wstring(pid);
      }

      DockApp& app = take(key);
      app.running = true;
      app.windows.push_back(raw.hwnd);
      if (app.hwnd == nullptr) {
        app.hwnd = raw.hwnd;
      }
      if (app.aumid.empty()) {
        app.aumid = raw.aumid;
      }
      if (app.icon_resource.empty()) {
        app.icon_resource = raw.icon_resource;
      }
      if (app.exe_path.empty() && !raw.path.empty() && !IsHostExe(raw.path)) {
        app.exe_path = raw.path;
      }
      if (app.display_name.empty()) {
        if (!raw.aumid.empty()) {
          app.display_name = AppsFolderDisplayName(raw.aumid);
        }
        if (app.display_name.empty() && !raw.relaunch_name.empty()) {
          app.display_name = LoadIndirect(raw.relaunch_name);
        }
        if (app.display_name.empty()) {
          app.display_name = DisplayNameFor(app.exe_path.empty() ? raw.path : app.exe_path, raw.title);
        }
      }
    }
  };

  ingest(true);
  if (groups.empty() && !windows.empty()) {
    ingest(false);
  }

  std::vector<DockApp> result;
  std::vector<std::wstring> used_keys;

  for (const auto& pin : pinned_paths) {
    if (IsSelfExecutable(pin)) {
      continue;
    }
    const std::wstring canon = CanonicalPath(pin);
    DockApp* found = nullptr;
    for (auto& [key, app] : groups) {
      if (!app.exe_path.empty() && CanonicalPath(app.exe_path) == canon) {
        found = &app;
        break;
      }
    }
    if (found != nullptr) {
      found->pinned = true;
      found->can_pin = true;
      if (found->exe_path.empty()) {
        found->exe_path = pin;
      }
      result.push_back(*found);
      used_keys.push_back(found->key);
    } else {
      DockApp app;
      app.exe_path = pin;
      app.aumid = PathAumid(pin);
      if (!app.aumid.empty()) {
        app.key = L"aumid:" + Lower(app.aumid);
        app.display_name = AppsFolderDisplayName(app.aumid);
      } else {
        app.key = L"path:" + canon;
      }
      if (app.display_name.empty()) {
        app.display_name = DisplayNameFor(pin, {});
      }
      app.pinned = true;
      app.can_pin = true;
      used_keys.push_back(app.key);
      result.push_back(std::move(app));
    }
  }

  for (const auto& key : order) {
    if (std::find(used_keys.begin(), used_keys.end(), key) != used_keys.end()) {
      continue;
    }
    DockApp& app = groups[key];
    app.can_pin = !app.exe_path.empty() && !IsHostExe(app.exe_path);
    result.push_back(app);
  }

  return result;
}

bool ActivateHwnd(HWND hwnd) {
  if (hwnd == nullptr || !IsWindow(hwnd)) {
    return false;
  }
  if (IsIconic(hwnd)) {
    ShowWindow(hwnd, SW_RESTORE);
  }
  const HWND fg = GetForegroundWindow();
  DWORD fg_tid = 0;
  if (fg != nullptr) {
    fg_tid = GetWindowThreadProcessId(fg, nullptr);
  }
  const DWORD self_tid = GetCurrentThreadId();
  if (fg_tid != 0 && fg_tid != self_tid) {
    AttachThreadInput(self_tid, fg_tid, TRUE);
  }
  BringWindowToTop(hwnd);
  const BOOL ok = SetForegroundWindow(hwnd);
  if (fg_tid != 0 && fg_tid != self_tid) {
    AttachThreadInput(self_tid, fg_tid, FALSE);
  }
  return ok != FALSE;
}

bool LaunchExe(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI;
  info.lpVerb = L"open";
  info.lpFile = path.c_str();
  info.nShow = SW_SHOWNORMAL;
  return ShellExecuteExW(&info) != FALSE;
}

void CloseHwnds(const std::vector<HWND>& windows) {
  for (HWND hwnd : windows) {
    if (hwnd != nullptr && IsWindow(hwnd)) {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
    }
  }
}

}  // namespace bamti
