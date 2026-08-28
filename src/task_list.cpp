#include "task_list.hpp"

#include "log.hpp"
#include "paths.hpp"
#include "watchdog.hpp"

#include <appmodel.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <unordered_map>
#include <unordered_set>

namespace bamti {
namespace {

constexpr wchar_t kAumidPinPrefix[] = L"aumid:";

bool EqualsIgnoreCase(const std::wstring& a, const std::wstring& b) {
  return lstrcmpiW(a.c_str(), b.c_str()) == 0;
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
      L"bamti.Spotlight",
      L"IME",
      L"MSCTFIME UI",
      L"CiceroUIWndFrame",
      L"XamlExplorerHostIslandWindow",
      L"Windows.Internal.Shell.TabProxyWindow",
      L"ImmediateContentWindow",
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
  if ((ex & WS_EX_NOACTIVATE) != 0 && (ex & WS_EX_APPWINDOW) == 0) {
    DWORD_PTR text_len = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 30, &text_len) == 0 ||
        text_len == 0) {
      return false;
    }
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
      L"crossdeviceresume",
      L"widgetservice",
      L"widgets",
      L"widgetboard",
      L"gamebarftw",
      L"gamebarpresencewriter",
      L"phoneexperiencehost",
  };
  for (const wchar_t* skip : kSkip) {
    if (stem == skip) {
      return true;
    }
  }
  return false;
}

bool IsExplorerFolderWindow(HWND hwnd) {
  wchar_t cls[256]{};
  GetClassNameW(hwnd, cls, 256);
  return lstrcmpiW(cls, L"CabinetWClass") == 0 || lstrcmpiW(cls, L"ExploreWClass") == 0;
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

bool SkipGhostWindow(HWND hwnd, const std::wstring& path, const std::wstring& aumid, const std::wstring& title) {
  if (!path.empty() && Lower(FileStem(path)) == L"explorer" && !IsExplorerFolderWindow(hwnd)) {
    return true;
  }
  if (IsHostExe(path) && aumid.empty()) {
    return true;
  }
  return path.empty() && aumid.empty() && title.empty();
}

bool LooksLikeHostedWebApp(const std::wstring& aumid) {
  const std::wstring id = Lower(aumid);
  if (id.find(L"://") != std::wstring::npos || id.find(L"!http") != std::wstring::npos) {
    return true;
  }
  return id.rfind(L"chrome.app.", 0) == 0 || id.rfind(L"chrome._crx_", 0) == 0 ||
         id.rfind(L"chromium.app.", 0) == 0 || id.rfind(L"chromium._crx_", 0) == 0 ||
         id.rfind(L"brave.app.", 0) == 0 || id.rfind(L"msedge-", 0) == 0;
}

std::wstring PinPrimary(const std::wstring& pin) {
  const size_t tab = pin.find(L'\t');
  return tab == std::wstring::npos ? pin : pin.substr(0, tab);
}

std::wstring PinExtra(const std::wstring& pin) {
  const size_t tab = pin.find(L'\t');
  if (tab == std::wstring::npos || tab + 1 >= pin.size()) {
    return {};
  }
  return pin.substr(tab + 1);
}

bool IsAumidPin(const std::wstring& pin) {
  const std::wstring primary = PinPrimary(pin);
  return primary.rfind(kAumidPinPrefix, 0) == 0 && primary.size() > wcslen(kAumidPinPrefix);
}

std::wstring AumidFromPin(const std::wstring& pin) {
  const std::wstring primary = PinPrimary(pin);
  if (primary.rfind(kAumidPinPrefix, 0) != 0 || primary.size() <= wcslen(kAumidPinPrefix)) {
    return {};
  }
  return primary.substr(wcslen(kAumidPinPrefix));
}

std::wstring AumidFallbackName(const std::wstring& aumid) {
  const size_t bang = aumid.find(L'!');
  if (bang == std::wstring::npos || bang + 1 >= aumid.size()) {
    return aumid;
  }
  std::wstring tail = aumid.substr(bang + 1);
  if (tail.rfind(L"https://", 0) == 0) {
    tail.erase(0, 8);
  } else if (tail.rfind(L"http://", 0) == 0) {
    tail.erase(0, 7);
  }
  while (!tail.empty() && tail.back() == L'/') {
    tail.pop_back();
  }
  return tail.empty() ? aumid : tail;
}

bool CanPinApp(const DockApp& app) {
  if (IsSelfExecutable(app.exe_path)) {
    return false;
  }
  if (!app.aumid.empty()) {
    return true;
  }
  return !app.exe_path.empty() && !IsHostExe(app.exe_path);
}

Microsoft::WRL::ComPtr<IShellItem> ShellItemFromAumid(const std::wstring& aumid) {
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (aumid.empty()) {
    return item;
  }
  if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, aumid.c_str(), IID_PPV_ARGS(&item))) && item) {
    return item;
  }
  item.Reset();
  if (aumid.find(L'!') == std::wstring::npos) {
    const std::wstring alt = aumid + L"!App";
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, alt.c_str(), IID_PPV_ARGS(&item))) &&
        item) {
      return item;
    }
    item.Reset();
  }
  const std::wstring parsing = L"shell:AppsFolder\\" + aumid;
  SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item));
  return item;
}

std::wstring FilePathFromShellItem(IShellItem* item) {
  if (item == nullptr) {
    return {};
  }
  PWSTR path = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) || path == nullptr) {
    return {};
  }
  std::wstring out = path;
  CoTaskMemFree(path);
  return out;
}

bool LaunchShellItem(IShellItem* item) {
  if (item == nullptr) {
    return false;
  }
  PIDLIST_ABSOLUTE pidl = nullptr;
  if (FAILED(SHGetIDListFromObject(item, &pidl)) || pidl == nullptr) {
    return false;
  }
  SHELLEXECUTEINFOW info{};
  info.cbSize = sizeof(info);
  info.fMask = SEE_MASK_FLAG_NO_UI | SEE_MASK_IDLIST;
  info.lpIDList = pidl;
  info.nShow = SW_SHOWNORMAL;
  const bool ok = ShellExecuteExW(&info) != FALSE;
  CoTaskMemFree(pidl);
  return ok;
}

bool LaunchAumid(const std::wstring& aumid) {
  if (aumid.empty()) {
    return false;
  }
  if (LaunchShellItem(ShellItemFromAumid(aumid).Get())) {
    return true;
  }
  Microsoft::WRL::ComPtr<IApplicationActivationManager> activator;
  if (SUCCEEDED(CoCreateInstance(CLSID_ApplicationActivationManager, nullptr, CLSCTX_INPROC_SERVER,
                                 IID_PPV_ARGS(&activator))) &&
      activator) {
    DWORD pid = 0;
    if (SUCCEEDED(activator->ActivateApplication(aumid.c_str(), nullptr, AO_NONE, &pid))) {
      return true;
    }
    if (aumid.find(L'!') == std::wstring::npos) {
      const std::wstring alt = aumid + L"!App";
      if (SUCCEEDED(activator->ActivateApplication(alt.c_str(), nullptr, AO_NONE, &pid))) {
        return true;
      }
    }
  }
  if (!LooksLikeHostedWebApp(aumid)) {
    const std::wstring path = FilePathFromShellItem(ShellItemFromAumid(aumid).Get());
    if (!path.empty() && !IsHostExe(path)) {
      return LaunchExe(path);
    }
  }
  return false;
}

std::wstring ProcessAumid(HWND hwnd);

std::wstring ReadStoreString(IPropertyStore* store, const PROPERTYKEY& key) {
  if (store == nullptr) {
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

struct WindowProps {
  std::wstring aumid;
  std::wstring icon_resource;
  std::wstring relaunch_name;
  std::wstring relaunch_command;
};

WindowProps ReadWindowProps(HWND hwnd) {
  WindowProps out;
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  if (SUCCEEDED(SHGetPropertyStoreForWindow(hwnd, IID_PPV_ARGS(&store))) && store) {
    out.aumid = ReadStoreString(store.Get(), PKEY_AppUserModel_ID);
    out.icon_resource = ReadStoreString(store.Get(), PKEY_AppUserModel_RelaunchIconResource);
    out.relaunch_name = ReadStoreString(store.Get(), PKEY_AppUserModel_RelaunchDisplayNameResource);
    out.relaunch_command = ReadStoreString(store.Get(), PKEY_AppUserModel_RelaunchCommand);
  }
  if (out.aumid.empty()) {
    out.aumid = ProcessAumid(hwnd);
  }
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

std::wstring PathAumid(const std::wstring& path) {
  if (path.empty()) {
    return {};
  }
  static std::unordered_map<std::wstring, std::wstring> cache;
  if (const auto it = cache.find(path); it != cache.end()) {
    return it->second;
  }
  Microsoft::WRL::ComPtr<IPropertyStore> store;
  std::wstring out;
  if (SUCCEEDED(SHGetPropertyStoreFromParsingName(path.c_str(), nullptr, GPS_DEFAULT, IID_PPV_ARGS(&store))) &&
      store) {
    PROPVARIANT value;
    PropVariantInit(&value);
    if (SUCCEEDED(store->GetValue(PKEY_AppUserModel_ID, &value)) && value.vt == VT_LPWSTR &&
        value.pwszVal != nullptr && value.pwszVal[0] != L'\0') {
      out = value.pwszVal;
    }
    PropVariantClear(&value);
  }
  cache.emplace(path, out);
  return out;
}

struct WindowCacheEntry {
  std::wstring path;
  WindowProps props;
};

constexpr size_t kWindowCacheMax = 512;
std::unordered_map<HWND, WindowCacheEntry> g_window_cache;

const WindowCacheEntry& CachedWindow(HWND hwnd, bool* from_cache) {
  if (const auto it = g_window_cache.find(hwnd); it != g_window_cache.end()) {
    if (from_cache != nullptr) {
      *from_cache = true;
    }
    return it->second;
  }
  if (from_cache != nullptr) {
    *from_cache = false;
  }
  if (g_window_cache.size() >= kWindowCacheMax) {
    g_window_cache.clear();
  }
  WindowCacheEntry entry;
  entry.path = WindowExePath(hwnd);
  entry.props = ReadWindowProps(hwnd);
  if (entry.props.aumid.empty() && !entry.path.empty() && !IsHostExe(entry.path)) {
    entry.props.aumid = PathAumid(entry.path);
  }
  if (entry.path.empty()) {
    static WindowCacheEntry uncached;
    uncached = std::move(entry);
    return uncached;
  }
  return g_window_cache.emplace(hwnd, std::move(entry)).first->second;
}

void PruneWindowCache() {
  for (auto it = g_window_cache.begin(); it != g_window_cache.end();) {
    if (it->first == nullptr || !IsWindow(it->first)) {
      it = g_window_cache.erase(it);
    } else {
      ++it;
    }
  }
}

IVirtualDesktopManager* DesktopManager() {
  static Microsoft::WRL::ComPtr<IVirtualDesktopManager> vdm;
  static bool tried = false;
  if (!tried) {
    tried = true;
    CoCreateInstance(CLSID_VirtualDesktopManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&vdm));
  }
  return vdm.Get();
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
  static std::unordered_map<std::wstring, std::wstring> cache;
  if (const auto it = cache.find(aumid); it != cache.end()) {
    return it->second;
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
    cache.emplace(aumid, std::wstring{});
    return {};
  }
  PWSTR name = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) || name == nullptr) {
    cache.emplace(aumid, std::wstring{});
    return {};
  }
  std::wstring out = name;
  CoTaskMemFree(name);
  cache.emplace(aumid, out);
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

constexpr UINT kPinCmpLogMax = 5;
UINT g_pin_cmp_logs = 0;

std::wstring PinCmpDisplay(const std::wstring& text) {
  std::wstring out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    const wchar_t c = text[i];
    if (c == L'\t') {
      wchar_t mark[32]{};
      swprintf_s(mark, L"<TAB:%zu>", i);
      out += mark;
    } else if (c < 32) {
      wchar_t mark[16]{};
      swprintf_s(mark, L"<0x%02X>", static_cast<unsigned>(c) & 0xFFu);
      out += mark;
    } else {
      out.push_back(c);
    }
  }
  return out;
}

void LogPinCmpFalse(const wchar_t* branch, const std::wstring& a, const std::wstring& b, const std::wstring& ca,
                    const std::wstring& cb) {
  if (g_pin_cmp_logs >= kPinCmpLogMax) {
    return;
  }
  ++g_pin_cmp_logs;
  const std::wstring da = PinCmpDisplay(a);
  const std::wstring db = PinCmpDisplay(b);
  const std::wstring dca = PinCmpDisplay(ca);
  const std::wstring dcb = PinCmpDisplay(cb);
  Log(L"dock", L"pin cmp branch=%s a=[%s](%zu) b=[%s](%zu) ca=[%s](%zu) cb=[%s](%zu)", branch, da.c_str(), a.size(),
      db.c_str(), b.size(), dca.c_str(), ca.size(), dcb.c_str(), cb.size());
}

}  // namespace

uint64_t TaskWindowFingerprint() {
  struct Acc {
    uint64_t hash = 14695981039346656037ULL;
    uint64_t count = 0;
  } acc;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsTaskWindow(hwnd)) {
          return TRUE;
        }
        auto* a = reinterpret_cast<Acc*>(lp);
        a->hash ^= static_cast<uint64_t>(reinterpret_cast<uintptr_t>(hwnd));
        a->hash *= 1099511628211ULL;
        ++a->count;
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&acc));
  acc.hash ^= acc.count;
  acc.hash *= 1099511628211ULL;
  return acc.hash;
}

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
  wchar_t buf[513]{};
  using InternalGetWindowTextFn = int(WINAPI*)(HWND, LPWSTR, int);
  static const auto internal_text = reinterpret_cast<InternalGetWindowTextFn>(
      GetProcAddress(GetModuleHandleW(L"user32.dll"), "InternalGetWindowText"));
  if (internal_text != nullptr) {
    const int n = internal_text(hwnd, buf, 512);
    if (n > 0) {
      return {buf, static_cast<size_t>(n)};
    }
  }
  DWORD_PTR len = 0;
  if (SendMessageTimeoutW(hwnd, WM_GETTEXTLENGTH, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 10, &len) == 0 ||
      len == 0) {
    return {};
  }
  const int cap = static_cast<int>((std::min)(len, static_cast<DWORD_PTR>(512)));
  std::wstring text(static_cast<size_t>(cap) + 1, L'\0');
  DWORD_PTR written = 0;
  if (SendMessageTimeoutW(hwnd, WM_GETTEXT, static_cast<WPARAM>(cap + 1), reinterpret_cast<LPARAM>(text.data()),
                          SMTO_ABORTIFHUNG | SMTO_BLOCK, 10, &written) == 0 ||
      written == 0) {
    return {};
  }
  text.resize(wcsnlen(text.c_str(), text.size()));
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

std::wstring DockPinId(const DockApp& app) {
  if (!app.aumid.empty() &&
      (LooksLikeHostedWebApp(app.aumid) || app.exe_path.empty() || IsHostExe(app.exe_path))) {
    return std::wstring(kAumidPinPrefix) + app.aumid;
  }
  if (!app.exe_path.empty()) {
    return app.exe_path;
  }
  if (!app.aumid.empty()) {
    return std::wstring(kAumidPinPrefix) + app.aumid;
  }
  return {};
}

bool SameDockPin(const std::wstring& a, const std::wstring& b) {
  if (a.empty() || b.empty()) {
    LogPinCmpFalse(L"path", a, b, {}, {});
    return false;
  }
  const bool a_aumid = IsAumidPin(a);
  const bool b_aumid = IsAumidPin(b);
  if (a_aumid && b_aumid) {
    const bool same = EqualsIgnoreCase(AumidFromPin(a), AumidFromPin(b));
    if (!same) {
      LogPinCmpFalse(L"both-aumid", a, b, {}, {});
    }
    return same;
  }
  if (a_aumid || b_aumid) {
    const std::wstring a_id = Lower(a_aumid ? AumidFromPin(a) : PathAumid(a));
    const std::wstring b_id = Lower(b_aumid ? AumidFromPin(b) : PathAumid(b));
    const bool same = !a_id.empty() && a_id == b_id;
    if (!same) {
      LogPinCmpFalse(L"mixed-aumid", a, b, {}, {});
    }
    return same;
  }
  const std::wstring ca = CanonicalPath(a);
  const std::wstring cb = CanonicalPath(b);
  const bool same = ca == cb;
  if (!same) {
    LogPinCmpFalse(L"path", a, b, ca, cb);
  }
  return same;
}

void ResetPinCmpLog() {
  g_pin_cmp_logs = 0;
}

std::wstring DockPinCompareForm(const std::wstring& pin) {
  if (IsAumidPin(pin)) {
    return Lower(AumidFromPin(pin));
  }
  return CanonicalPath(pin);
}

std::vector<std::wstring> LoadDockPins() {
  std::vector<std::wstring> pins;
  const std::wstring path = DockPinsPath();
  if (path.empty()) {
    Log(L"pins", L"load skipped: empty path");
    return pins;
  }
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || file == nullptr) {
    Log(L"pins", L"load missing %s", path.c_str());
    return pins;
  }
  wchar_t line[4096]{};
  while (fgetws(line, 4096, file) != nullptr) {
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
  Log(L"pins", L"loaded %zu from %s", pins.size(), path.c_str());
  return pins;
}

bool SaveDockPins(const std::vector<std::wstring>& paths) {
  const std::wstring path = DockPinsPath();
  if (path.empty()) {
    Log(L"pins", L"save failed: empty path");
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
    Log(L"pins", L"save failed move %s", path.c_str());
    return false;
  }
  Log(L"pins", L"saved %zu to %s", paths.size(), path.c_str());
  return true;
}

std::vector<DockApp> CollectDockApps(const std::vector<std::wstring>& pinned_paths) {
  WatchdogStage(L"collect");
  IVirtualDesktopManager* vdm = DesktopManager();

  struct Raw {
    HWND hwnd;
    std::wstring path;
    std::wstring aumid;
    std::wstring title;
    std::wstring icon_resource;
    std::wstring relaunch_name;
    std::wstring relaunch_command;
    bool path_cached = false;
  };
  std::vector<Raw> windows;
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsTaskWindow(hwnd)) {
          return TRUE;
        }
        auto* out = reinterpret_cast<std::vector<Raw>*>(lp);
        bool path_cached = false;
        const WindowCacheEntry& cached = CachedWindow(hwnd, &path_cached);
        Raw raw{};
        raw.hwnd = hwnd;
        raw.path = cached.path;
        raw.aumid = cached.props.aumid;
        raw.icon_resource = cached.props.icon_resource;
        raw.relaunch_name = cached.props.relaunch_name;
        raw.relaunch_command = cached.props.relaunch_command;
        raw.path_cached = path_cached;
        raw.title = WindowTitle(hwnd);
        if (SkipGhostWindow(hwnd, raw.path, raw.aumid, raw.title)) {
          return TRUE;
        }
        out->push_back(std::move(raw));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&windows));
  PruneWindowCache();

  std::unordered_map<std::wstring, DockApp> groups;
  std::vector<std::wstring> order;
  std::unordered_map<std::wstring, int> path_cached_flag;

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
    path_cached_flag.clear();
    for (const auto& raw : windows) {
      if (filter_desktop && !OnCurrentDesktop(vdm, raw.hwnd)) {
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
      if (app.relaunch_command.empty()) {
        app.relaunch_command = raw.relaunch_command;
      }
      if (app.exe_path.empty() && !raw.path.empty() && !IsHostExe(raw.path)) {
        app.exe_path = raw.path;
        path_cached_flag[key] = raw.path_cached ? 1 : 0;
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

  auto already_used = [&](const std::wstring& key) {
    return std::find(used_keys.begin(), used_keys.end(), key) != used_keys.end();
  };

  std::unordered_set<std::wstring> aumid_pins;
  for (const auto& pin : pinned_paths) {
    if (IsAumidPin(pin)) {
      aumid_pins.insert(Lower(AumidFromPin(pin)));
    }
  }

  auto find_by_aumid = [&](const std::wstring& aumid) -> DockApp* {
    const std::wstring want = Lower(aumid);
    for (const auto& key : order) {
      DockApp& app = groups[key];
      if (already_used(app.key)) {
        continue;
      }
      if (!app.aumid.empty() && Lower(app.aumid) == want) {
        return &app;
      }
    }
    return nullptr;
  };

  auto find_by_path = [&](const std::wstring& path) -> DockApp* {
    for (const auto& key : order) {
      DockApp& app = groups[key];
      if (already_used(app.key) || app.exe_path.empty() || !SameDockPin(path, app.exe_path)) {
        continue;
      }
      if (!app.aumid.empty() &&
          (aumid_pins.count(Lower(app.aumid)) != 0 || LooksLikeHostedWebApp(app.aumid))) {
        continue;
      }
      return &app;
    }
    return nullptr;
  };

  for (const auto& pin : pinned_paths) {
    if (!IsAumidPin(pin) && IsSelfExecutable(pin)) {
      continue;
    }
    DockApp* found = IsAumidPin(pin) ? find_by_aumid(AumidFromPin(pin)) : find_by_path(pin);
    if (found != nullptr) {
      found->pinned = true;
      found->can_pin = true;
      if (!IsAumidPin(pin) && found->exe_path.empty()) {
        found->exe_path = pin;
      }
      if (found->relaunch_command.empty()) {
        found->relaunch_command = PinExtra(pin);
      }
      result.push_back(*found);
      used_keys.push_back(found->key);
    } else if (IsAumidPin(pin)) {
      DockApp app;
      app.aumid = AumidFromPin(pin);
      app.key = L"aumid:" + Lower(app.aumid);
      app.display_name = AppsFolderDisplayName(app.aumid);
      if (app.display_name.empty()) {
        app.display_name = AumidFallbackName(app.aumid);
      }
      app.relaunch_command = PinExtra(pin);
      app.exe_path = FilePathFromShellItem(ShellItemFromAumid(app.aumid).Get());
      app.pinned = true;
      app.can_pin = true;
      used_keys.push_back(app.key);
      result.push_back(std::move(app));
    } else {
      DockApp app;
      app.exe_path = pin;
      app.aumid = PathAumid(pin);
      if (!app.aumid.empty()) {
        app.key = L"aumid:" + Lower(app.aumid);
        app.display_name = AppsFolderDisplayName(app.aumid);
      } else {
        app.key = L"path:" + CanonicalPath(pin);
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

  int miss_logs = 0;
  for (const auto& key : order) {
    if (std::find(used_keys.begin(), used_keys.end(), key) != used_keys.end()) {
      continue;
    }
    DockApp& app = groups[key];
    app.can_pin = CanPinApp(app);
    if (miss_logs < 3) {
      const std::wstring pin_key = DockPinId(app);
      const std::wstring canon = CanonicalPath(app.exe_path);
      int cached = 0;
      if (const auto it = path_cached_flag.find(app.key); it != path_cached_flag.end()) {
        cached = it->second;
      }
      Log(L"dock", L"pin miss key=%s path=%s canon=%s cached=%d", pin_key.c_str(), app.exe_path.c_str(),
          canon.c_str(), cached);
      ++miss_logs;
    }
    result.push_back(app);
  }

  return result;
}

void ForgetCachedWindow(HWND hwnd) {
  if (hwnd != nullptr) {
    g_window_cache.erase(hwnd);
  }
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
  bool attached = false;
  if (fg_tid != 0 && fg_tid != self_tid) {
    DWORD_PTR dummy = 0;
    if (SendMessageTimeoutW(fg, WM_NULL, 0, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &dummy) != 0) {
      attached = AttachThreadInput(self_tid, fg_tid, TRUE) != FALSE;
    }
  }
  BringWindowToTop(hwnd);
  const BOOL ok = SetForegroundWindow(hwnd);
  if (attached) {
    AttachThreadInput(self_tid, fg_tid, FALSE);
  }
  if (ok == FALSE) {
    Log(L"task", L"activate failed hwnd=%p", hwnd);
  }
  return ok != FALSE;
}

bool LaunchCommandLine(std::wstring command) {
  if (command.empty()) {
    return false;
  }
  STARTUPINFOW si{};
  si.cb = sizeof(si);
  PROCESS_INFORMATION pi{};
  if (CreateProcessW(nullptr, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi) == FALSE) {
    return false;
  }
  CloseHandle(pi.hProcess);
  CloseHandle(pi.hThread);
  return true;
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

bool LaunchDockApp(const DockApp& app) {
  const bool hosted_web = LooksLikeHostedWebApp(app.aumid);
  if (hosted_web) {
    if (!app.aumid.empty() && LaunchAumid(app.aumid)) {
      return true;
    }
    return LaunchCommandLine(app.relaunch_command);
  }
  if (!app.aumid.empty() && (app.exe_path.empty() || IsHostExe(app.exe_path)) && LaunchAumid(app.aumid)) {
    return true;
  }
  if (LaunchCommandLine(app.relaunch_command)) {
    return true;
  }
  if (!app.exe_path.empty() && !IsHostExe(app.exe_path)) {
    return LaunchExe(app.exe_path);
  }
  if (!app.aumid.empty()) {
    return LaunchAumid(app.aumid);
  }
  return false;
}

void RestoreHwnds(const std::vector<HWND>& windows) {
  HWND focus = nullptr;
  for (auto it = windows.rbegin(); it != windows.rend(); ++it) {
    HWND hwnd = *it;
    if (hwnd == nullptr || !IsWindow(hwnd)) {
      continue;
    }
    if (IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
      ShowWindow(hwnd, SW_SHOW);
    }
    SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_SHOWWINDOW);
    focus = hwnd;
  }
  if (focus != nullptr) {
    ActivateHwnd(focus);
  }
}

void HideHwnds(const std::vector<HWND>& windows) {
  for (HWND hwnd : windows) {
    if (hwnd != nullptr && IsWindow(hwnd) && !IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_SHOWMINNOACTIVE);
    }
  }
}

void CloseHwnds(const std::vector<HWND>& windows) {
  int n = 0;
  for (HWND hwnd : windows) {
    if (hwnd != nullptr && IsWindow(hwnd)) {
      PostMessageW(hwnd, WM_CLOSE, 0, 0);
      ++n;
    }
  }
  Log(L"task", L"close posted n=%d", n);
}

}  // namespace bamti
