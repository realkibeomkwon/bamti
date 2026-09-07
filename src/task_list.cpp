#include "task_list.hpp"

#include "log.hpp"
#include "paths.hpp"
#include "watchdog.hpp"

#include <appmodel.h>
#include <dwmapi.h>
#include <ole2.h>
#include <exdisp.h>
#include <oleauto.h>
#include <knownfolders.h>
#include <propsys.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shlwapi.h>
#include <shobjidl.h>
#include <propkey.h>
#include <winver.h>
#include <wrl/client.h>

#include <tlhelp32.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <unordered_map>
#include <unordered_set>
#include <utility>

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

bool IsExplorerFolderClass(HWND hwnd) {
  wchar_t cls[256]{};
  GetClassNameW(hwnd, cls, 256);
  return lstrcmpiW(cls, L"CabinetWClass") == 0 || lstrcmpiW(cls, L"ExploreWClass") == 0;
}

bool IsExplorerFolderWindow(HWND hwnd) {
  return IsExplorerFolderClass(hwnd);
}

std::wstring ExplorerExePath() {
  wchar_t win[MAX_PATH]{};
  const UINT n = GetSystemWindowsDirectoryW(win, MAX_PATH);
  if (n == 0 || n >= MAX_PATH) {
    return L"C:\\Windows\\explorer.exe";
  }
  std::wstring path(win, n);
  if (path.back() != L'\\') {
    path.push_back(L'\\');
  }
  path += L"explorer.exe";
  return path;
}

bool IsFilesystemFolder(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  const DWORD attr = GetFileAttributesW(path.c_str());
  return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
}

std::unordered_set<HWND> ShellBrowserHwnds() {
  std::unordered_set<HWND> out;
  Microsoft::WRL::ComPtr<IShellWindows> windows;
  if (FAILED(CoCreateInstance(CLSID_ShellWindows, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&windows))) || !windows) {
    return out;
  }
  long count = 0;
  if (FAILED(windows->get_Count(&count)) || count <= 0) {
    return out;
  }
  for (long i = 0; i < count; ++i) {
    VARIANT index;
    VariantInit(&index);
    index.vt = VT_I4;
    index.lVal = i;
    Microsoft::WRL::ComPtr<IDispatch> disp;
    if (FAILED(windows->Item(index, disp.GetAddressOf())) || !disp) {
      continue;
    }
    Microsoft::WRL::ComPtr<IWebBrowserApp> browser;
    if (FAILED(disp.As(&browser)) || !browser) {
      continue;
    }
    SHANDLE_PTR found = 0;
    if (SUCCEEDED(browser->get_HWND(&found)) && found != 0) {
      out.insert(reinterpret_cast<HWND>(found));
    }
  }
  return out;
}

std::wstring QueryWindowExePath(HWND hwnd) {
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

// MSIX 패키지 앱의 실행 파일은 업데이트마다 버전 폴더 이름이 바뀌므로 경로를 핀으로 쓸 수 없다.
bool IsPackagedPath(const std::wstring& path) {
  const std::wstring lower = Lower(path);
  return lower.find(L"\\windowsapps\\") != std::wstring::npos ||
         lower.find(L"\\systemapps\\") != std::wstring::npos;
}

// "Name_Version_Arch[_ResourceId]__PublisherId" 폴더 이름에서 버전과 아키텍처를 지우고
// "name__publisherid"만 남긴다. 패키지 폴더 형식이 아니면 빈 값을 돌려준다.
std::wstring StripPackageVersion(const std::wstring& segment) {
  const size_t pub = segment.rfind(L"__");
  if (pub == std::wstring::npos || pub == 0 || pub + 2 >= segment.size()) {
    return {};
  }
  const size_t name_end = segment.find(L'_');
  if (name_end == std::wstring::npos || name_end >= pub) {
    return {};
  }
  return segment.substr(0, name_end) + L"__" + segment.substr(pub + 2);
}

// 패키지 폴더 이름 "Name_Version_Arch[_ResourceId]__PublisherId"에서
// 패키지 패밀리 이름 "name_publisherid"를 만든다. 형식이 아니면 빈 값이다.
std::wstring PackageFamilyFromFolder(const std::wstring& segment) {
  const size_t pub = segment.rfind(L"__");
  if (pub == std::wstring::npos || pub == 0 || pub + 2 >= segment.size()) {
    return {};
  }
  const size_t name_end = segment.find(L'_');
  if (name_end == std::wstring::npos || name_end >= pub) {
    return {};
  }
  return Lower(segment.substr(0, name_end) + L"_" + segment.substr(pub + 2));
}

// WindowsApps 경로에서 패키지 패밀리 이름을 뽑는다. 패키지 경로가 아니면 빈 값이다.
std::wstring PackageFamilyFromPath(const std::wstring& path) {
  const std::wstring canon = CanonicalPath(path);
  const size_t root = canon.find(L"\\windowsapps\\");
  if (root == std::wstring::npos) {
    return {};
  }
  const size_t begin = root + wcslen(L"\\windowsapps\\");
  size_t end = canon.find(L'\\', begin);
  if (end == std::wstring::npos) {
    end = canon.size();
  }
  return PackageFamilyFromFolder(canon.substr(begin, end - begin));
}

// AUMID "PackageFamilyName!AppId"에서 앞부분을 뽑는다. '!'가 없으면 전체를 쓴다.
std::wstring PackageFamilyFromAumid(const std::wstring& aumid) {
  if (aumid.empty()) {
    return {};
  }
  const size_t bang = aumid.find(L'!');
  return Lower(bang == std::wstring::npos ? aumid : aumid.substr(0, bang));
}

// 핀을 비교할 때에만 쓰는 형태. 패키지 경로면 버전을 지우고, 아니면 CanonicalPath 그대로다.
std::wstring PathMatchForm(const std::wstring& path) {
  std::wstring canon = CanonicalPath(path);
  const size_t root = canon.find(L"\\windowsapps\\");
  if (root == std::wstring::npos) {
    return canon;
  }
  const size_t begin = root + wcslen(L"\\windowsapps\\");
  size_t end = canon.find(L'\\', begin);
  if (end == std::wstring::npos) {
    end = canon.size();
  }
  const std::wstring stripped = StripPackageVersion(canon.substr(begin, end - begin));
  if (stripped.empty()) {
    return canon;
  }
  return canon.substr(0, begin) + stripped + canon.substr(end);
}

bool SkipGhostWindow(HWND hwnd, const std::wstring& path, const std::wstring& aumid, const std::wstring& title,
                     bool shell_browser) {
  const bool folder_window = shell_browser || IsExplorerFolderClass(hwnd);
  if (folder_window) {
    (void)title;
    return false;
  }
  if (!path.empty() && Lower(FileStem(path)) == L"explorer") {
    return true;
  }
  if (IsHostExe(path) && aumid.empty()) {
    return true;
  }
  (void)title;
  return path.empty() && aumid.empty();
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
  if (IsSelfExecutable(app.exe_path) || IsFilesystemFolder(app.exe_path)) {
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

bool CollectAppsFolderEntries(std::vector<std::pair<std::wstring, std::wstring>>& out) {
  Microsoft::WRL::ComPtr<IShellItem> apps;
  HRESULT hr = SHGetKnownFolderItem(FOLDERID_AppsFolder, KF_FLAG_DONT_VERIFY, nullptr, IID_PPV_ARGS(&apps));
  if (FAILED(hr) || !apps) {
    return false;
  }
  Microsoft::WRL::ComPtr<IEnumShellItems> e;
  hr = apps->BindToHandler(nullptr, BHID_EnumItems, IID_PPV_ARGS(&e));
  if (FAILED(hr) || !e) {
    return false;
  }
  Microsoft::WRL::ComPtr<IShellItem> item;
  ULONG fetched = 0;
  while (e->Next(1, item.ReleaseAndGetAddressOf(), &fetched) == S_OK && fetched == 1) {
    std::wstring path = FilePathFromShellItem(item.Get());
    std::wstring aumid;
    PWSTR id = nullptr;
    if (SUCCEEDED(item->GetDisplayName(SIGDN_PARENTRELATIVEFORADDRESSBAR, &id)) && id != nullptr) {
      aumid = id;
      CoTaskMemFree(id);
    }
    if (path.empty() && aumid.empty()) {
      continue;
    }
    out.push_back({std::move(aumid), std::move(path)});
  }
  return true;
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
  entry.path = QueryWindowExePath(hwnd);
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

void TrimSpaces(std::wstring& text) {
  while (!text.empty() && iswspace(text.front())) {
    text.erase(text.begin());
  }
  while (!text.empty() && iswspace(text.back())) {
    text.pop_back();
  }
}

bool HasBinaryExtension(const std::wstring& name) {
  const size_t dot = name.find_last_of(L'.');
  if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) {
    return false;
  }
  const std::wstring ext = Lower(name.substr(dot));
  return ext == L".exe" || ext == L".com" || ext == L".bat" || ext == L".cmd" || ext == L".msc" ||
         ext == L".lnk";
}

bool LooksLikeRawFileName(const std::wstring& name, const std::wstring& path) {
  if (name.empty()) {
    return true;
  }
  if (HasBinaryExtension(name)) {
    return true;
  }
  if (path.empty()) {
    return false;
  }
  const size_t slash = path.find_last_of(L"\\/");
  const std::wstring file = slash == std::wstring::npos ? path : path.substr(slash + 1);
  return EqualsIgnoreCase(name, file);
}

std::wstring ShellPropertyString(const std::wstring& path, REFPROPERTYKEY key) {
  Microsoft::WRL::ComPtr<IShellItem2> item;
  if (path.empty() || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) {
    return {};
  }
  PWSTR value = nullptr;
  if (FAILED(item->GetString(key, &value)) || value == nullptr) {
    return {};
  }
  std::wstring out = value;
  CoTaskMemFree(value);
  TrimSpaces(out);
  return out;
}

std::wstring ShellNormalName(const std::wstring& path) {
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (path.empty() || FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) {
    return {};
  }
  PWSTR name = nullptr;
  if (FAILED(item->GetDisplayName(SIGDN_NORMALDISPLAY, &name)) || name == nullptr) {
    return {};
  }
  std::wstring out = name;
  CoTaskMemFree(name);
  TrimSpaces(out);
  return out;
}

std::wstring VersionResourceString(const std::wstring& path, const wchar_t* field) {
  if (path.empty() || field == nullptr) {
    return {};
  }
  DWORD dummy = 0;
  const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &dummy);
  if (size == 0) {
    return {};
  }
  std::vector<std::uint8_t> buf(size);
  if (GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) == FALSE) {
    return {};
  }
  struct Translation {
    WORD language;
    WORD codepage;
  };
  Translation* trans = nullptr;
  UINT trans_bytes = 0;
  std::vector<Translation> ids;
  if (VerQueryValueW(buf.data(), L"\\VarFileInfo\\Translation", reinterpret_cast<void**>(&trans), &trans_bytes) !=
          FALSE &&
      trans != nullptr && trans_bytes >= sizeof(Translation)) {
    const size_t n = trans_bytes / sizeof(Translation);
    ids.assign(trans, trans + n);
  }
  const Translation extras[] = {{0x0409, 0x04B0}, {0x0412, 0x04B0}, {0x0409, 0x04E4}};
  ids.insert(ids.end(), std::begin(extras), std::end(extras));
  for (const Translation id : ids) {
    wchar_t key[80]{};
    swprintf_s(key, L"\\StringFileInfo\\%04x%04x\\%s", id.language, id.codepage, field);
    wchar_t* value = nullptr;
    UINT value_bytes = 0;
    if (VerQueryValueW(buf.data(), key, reinterpret_cast<void**>(&value), &value_bytes) == FALSE || value == nullptr ||
        value[0] == L'\0') {
      continue;
    }
    std::wstring out = value;
    TrimSpaces(out);
    if (!out.empty()) {
      return out;
    }
  }
  return {};
}

std::wstring DisplayNameFor(const std::wstring& path, const std::wstring& title) {
  static std::unordered_map<std::wstring, std::wstring> cache;
  const std::wstring cache_key = Lower(path);
  if (!cache_key.empty()) {
    if (const auto it = cache.find(cache_key); it != cache.end()) {
      return it->second;
    }
  }

  auto accept = [&](std::wstring name) -> std::wstring {
    TrimSpaces(name);
    if (LooksLikeRawFileName(name, path)) {
      return {};
    }
    return name;
  };

  std::wstring chosen;
  if (!path.empty()) {
    chosen = accept(ShellPropertyString(path, PKEY_FileDescription));
    if (chosen.empty()) {
      chosen = accept(VersionResourceString(path, L"FileDescription"));
    }
    if (chosen.empty()) {
      chosen = accept(VersionResourceString(path, L"ProductName"));
    }
    if (chosen.empty()) {
      chosen = accept(ShellNormalName(path));
    }
    if (chosen.empty()) {
      SHFILEINFOW info{};
      if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_DISPLAYNAME) != 0) {
        chosen = accept(info.szDisplayName);
      }
    }
    if (chosen.empty()) {
      const std::wstring stem = FileStem(path);
      if (!stem.empty() && !IsHostExe(path) && !HasBinaryExtension(stem)) {
        chosen = stem;
      }
    }
  }
  if (chosen.empty()) {
    chosen = accept(title);
  }
  if (chosen.empty() && !path.empty()) {
    chosen = FileStem(path);
  }
  if (chosen.empty()) {
    chosen = title;
  }
  if (!cache_key.empty() && !chosen.empty()) {
    cache.emplace(cache_key, chosen);
  }
  return chosen;
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

std::wstring AppsFolderLaunchLine(const std::wstring& aumid) {
  if (aumid.empty() || aumid.find(L'"') != std::wstring::npos) {
    return {};
  }
  return L"explorer.exe shell:AppsFolder\\" + aumid;
}

std::wstring QuotedExeCommand(const std::wstring& path) {
  if (path.empty() || path.find(L'"') != std::wstring::npos) {
    return {};
  }
  return L"\"" + path + L"\"";
}

template <typename TryAumid, typename TryRelaunch, typename TryExe>
bool TryDockLaunch(const DockApp& app, TryAumid&& try_aumid, TryRelaunch&& try_relaunch, TryExe&& try_exe) {
  if (LooksLikeHostedWebApp(app.aumid)) {
    if (!app.aumid.empty() && try_aumid(app.aumid)) {
      return true;
    }
    return try_relaunch(app.relaunch_command);
  }
  if (!app.aumid.empty() && (app.exe_path.empty() || IsHostExe(app.exe_path)) && try_aumid(app.aumid)) {
    return true;
  }
  if (try_relaunch(app.relaunch_command)) {
    return true;
  }
  if (!app.exe_path.empty() && !IsHostExe(app.exe_path) && try_exe(app.exe_path)) {
    return true;
  }
  if (!app.aumid.empty()) {
    return try_aumid(app.aumid);
  }
  return false;
}

std::wstring BuildDockLaunchCommandLine(const DockApp& app) {
  std::wstring line;
  TryDockLaunch(
      app,
      [&](const std::wstring& aumid) {
        line = AppsFolderLaunchLine(aumid);
        return !line.empty();
      },
      [&](const std::wstring& command) {
        line = command;
        return !command.empty();
      },
      [&](const std::wstring& path) {
        line = QuotedExeCommand(path);
        return !line.empty();
      });
  return line;
}

}  // namespace

std::wstring WindowExePath(HWND hwnd) {
  return QueryWindowExePath(hwnd);
}

std::wstring DockLaunchCommandLine(const DockApp& app) {
  return BuildDockLaunchCommandLine(app);
}

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

DockApp MakeSpotlightDockApp() {
  DockApp app;
  app.kind = DockItemKind::kSpotlight;
  app.key = kSpotlightPin;
  app.display_name = L"검색";
  app.pinned = true;
  app.can_pin = false;
  return app;
}

bool IsSpotlightPin(const std::wstring& pin) {
  return pin == kSpotlightPin || pin == L"\x01spotlight";
}

std::wstring DockPinId(const DockApp& app) {
  if (!app.aumid.empty() &&
      (LooksLikeHostedWebApp(app.aumid) || app.exe_path.empty() || IsHostExe(app.exe_path) ||
       IsPackagedPath(app.exe_path))) {
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
  if (IsSpotlightPin(a) || IsSpotlightPin(b)) {
    return IsSpotlightPin(a) && IsSpotlightPin(b);
  }
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
  const std::wstring ca = PathMatchForm(a);
  const std::wstring cb = PathMatchForm(b);
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
  if (IsSpotlightPin(pin)) {
    return kSpotlightPin;
  }
  if (IsAumidPin(pin)) {
    return Lower(AumidFromPin(pin));
  }
  return PathMatchForm(pin);
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

size_t RepairDockPins(std::vector<std::wstring>& pins) {
  std::vector<size_t> stale;
  for (size_t i = 0; i < pins.size(); ++i) {
    const std::wstring& pin = pins[i];
    if (IsSpotlightPin(pin) || IsAumidPin(pin)) {
      continue;
    }
    const std::wstring primary = PinPrimary(pin);
    if (GetFileAttributesW(primary.c_str()) != INVALID_FILE_ATTRIBUTES) {
      continue;
    }
    stale.push_back(i);
  }
  if (stale.empty()) {
    return 0;
  }

  std::vector<std::pair<std::wstring, std::wstring>> candidates;
  CollectAppsFolderEntries(candidates);

  size_t repaired = 0;
  for (const size_t i : stale) {
    const std::wstring primary = PinPrimary(pins[i]);
    const std::wstring family = PackageFamilyFromPath(primary);
    const std::pair<std::wstring, std::wstring>* match = nullptr;
    const wchar_t* by = nullptr;

    if (!family.empty()) {
      for (const auto& candidate : candidates) {
        if (PackageFamilyFromAumid(candidate.first) == family) {
          match = &candidate;
          by = L"family";
          break;
        }
      }
    }
    if (match == nullptr) {
      const std::wstring form = PathMatchForm(primary);
      if (!form.empty()) {
        for (const auto& candidate : candidates) {
          if (candidate.second.empty() || PathMatchForm(candidate.second) != form) {
            continue;
          }
          match = &candidate;
          by = L"path";
          break;
        }
      }
    }

    if (match == nullptr) {
      Log(L"pins", L"repair miss pin=%s family=%s candidates=%zu", pins[i].c_str(), family.c_str(),
          candidates.size());
      continue;
    }

    const std::wstring extra = PinExtra(pins[i]);
    std::wstring replacement = match->first.empty() ? match->second : std::wstring(kAumidPinPrefix) + match->first;
    if (!extra.empty()) {
      replacement += L'\t';
      replacement += extra;
    }

    bool duplicate = false;
    for (size_t j = 0; j < pins.size(); ++j) {
      if (j == i) {
        continue;
      }
      if (SameDockPin(pins[j], replacement)) {
        duplicate = true;
        break;
      }
    }
    if (duplicate) {
      Log(L"pins", L"repair miss pin=%s family=%s candidates=%zu", pins[i].c_str(), family.c_str(),
          candidates.size());
      continue;
    }

    const std::wstring stale_pin = pins[i];
    pins[i] = std::move(replacement);
    ++repaired;
    Log(L"pins", L"repaired by=%s stale=%s new=%s", by, stale_pin.c_str(), pins[i].c_str());
  }
  if (repaired > 0) {
    SaveDockPins(pins);
  }
  return repaired;
}

namespace {

double QpcMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  if (freq.QuadPart == 0) {
    return 0.0;
  }
  return (end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

constexpr ULONGLONG kLiveProcessCacheMs = 1000;

struct LiveProcessCache {
  ULONGLONG tick = 0;
  std::unordered_map<std::wstring, std::vector<DWORD>> pids_by_name;
};

LiveProcessCache g_live_process_cache;

std::wstring ExeFileName(const std::wstring& path) {
  const size_t slash = path.find_last_of(L"\\/");
  std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
  return Lower(std::move(name));
}

void RefreshLiveProcessCache() {
  const ULONGLONG now = GetTickCount64();
  if (g_live_process_cache.tick != 0 && now - g_live_process_cache.tick < kLiveProcessCacheMs) {
    return;
  }
  std::unordered_map<std::wstring, std::vector<DWORD>> pids_by_name;
  const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
  if (snap != INVALID_HANDLE_VALUE) {
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (Process32FirstW(snap, &entry)) {
      do {
        if (entry.th32ProcessID != 0) {
          pids_by_name[Lower(entry.szExeFile)].push_back(entry.th32ProcessID);
        }
      } while (Process32NextW(snap, &entry));
    }
    CloseHandle(snap);
  }
  g_live_process_cache.pids_by_name = std::move(pids_by_name);
  g_live_process_cache.tick = now;
}

// 주어진 실행 파일 경로로 도는 프로세스가 있는지 본다. 창이 없어도 참을 돌려준다.
bool ExeHasLiveProcess(const std::wstring& exe_path) {
  if (exe_path.empty()) {
    return false;
  }
  const std::wstring name = ExeFileName(exe_path);
  if (name.empty()) {
    return false;
  }
  RefreshLiveProcessCache();
  const auto it = g_live_process_cache.pids_by_name.find(name);
  if (it == g_live_process_cache.pids_by_name.end() || it->second.empty()) {
    return false;
  }
  if (it->second.size() == 1) {
    return true;
  }
  bool any_image = false;
  for (const DWORD pid : it->second) {
    const HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr) {
      continue;
    }
    wchar_t image[32768]{};
    DWORD n = static_cast<DWORD>(sizeof(image) / sizeof(image[0]));
    const BOOL ok = QueryFullProcessImageNameW(process, 0, image, &n);
    CloseHandle(process);
    if (ok == FALSE || n == 0) {
      continue;
    }
    any_image = true;
    if (SameDockPin(exe_path, std::wstring(image, n))) {
      return true;
    }
  }
  return !any_image;
}

}  // namespace

void InvalidateLiveProcessCache() {
  g_live_process_cache.tick = 0;
}

std::vector<DockApp> CollectDockApps(const std::vector<std::wstring>& pinned_paths) {
  WatchdogStage(L"collect");
  IVirtualDesktopManager* vdm = DesktopManager();
  const ULONGLONG total_started = GetTickCount64();

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
  struct EnumCtx {
    std::vector<Raw>* windows = nullptr;
    const std::unordered_set<HWND>* shell_hwnds = nullptr;
    double cached_ms = 0;
    double title_ms = 0;
    unsigned window_count = 0;
    unsigned cache_hits = 0;
  } enum_ctx;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  const std::unordered_set<HWND> shell_hwnds = ShellBrowserHwnds();
  QueryPerformanceCounter(&t1);
  const unsigned shell_ms = static_cast<unsigned>(QpcMs(t0, t1) + 0.5);
  enum_ctx.windows = &windows;
  enum_ctx.shell_hwnds = &shell_hwnds;
  QueryPerformanceCounter(&t0);
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        if (!IsTaskWindow(hwnd)) {
          return TRUE;
        }
        auto* ctx = reinterpret_cast<EnumCtx*>(lp);
        ++ctx->window_count;
        bool path_cached = false;
        LARGE_INTEGER c0{};
        LARGE_INTEGER c1{};
        QueryPerformanceCounter(&c0);
        const WindowCacheEntry& cached = CachedWindow(hwnd, &path_cached);
        QueryPerformanceCounter(&c1);
        ctx->cached_ms += QpcMs(c0, c1);
        if (path_cached) {
          ++ctx->cache_hits;
        }
        Raw raw{};
        raw.hwnd = hwnd;
        raw.path = cached.path;
        raw.aumid = cached.props.aumid;
        raw.icon_resource = cached.props.icon_resource;
        raw.relaunch_name = cached.props.relaunch_name;
        raw.relaunch_command = cached.props.relaunch_command;
        raw.path_cached = path_cached;
        QueryPerformanceCounter(&c0);
        raw.title = WindowTitle(hwnd);
        QueryPerformanceCounter(&c1);
        ctx->title_ms += QpcMs(c0, c1);
        const bool shell_browser = ctx->shell_hwnds != nullptr && ctx->shell_hwnds->count(hwnd) != 0;
        if (SkipGhostWindow(hwnd, raw.path, raw.aumid, raw.title, shell_browser)) {
          return TRUE;
        }
        ctx->windows->push_back(std::move(raw));
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&enum_ctx));
  PruneWindowCache();
  QueryPerformanceCounter(&t1);
  const unsigned enum_ms = static_cast<unsigned>(QpcMs(t0, t1) + 0.5);
  const unsigned cached_ms = static_cast<unsigned>(enum_ctx.cached_ms + 0.5);
  const unsigned title_ms = static_cast<unsigned>(enum_ctx.title_ms + 0.5);

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
      const bool folder_window = IsExplorerFolderClass(raw.hwnd) || shell_hwnds.count(raw.hwnd) != 0;
      std::wstring aumid_fs;
      if (!folder_window && !raw.aumid.empty() && (raw.path.empty() || Lower(FileStem(raw.path)) == L"explorer")) {
        aumid_fs = FilePathFromShellItem(ShellItemFromAumid(raw.aumid).Get());
      }
      const bool folder_identity = folder_window || IsFilesystemFolder(aumid_fs);
      std::wstring key;
      if (folder_identity) {
        const std::wstring explorer =
            !raw.path.empty() && Lower(FileStem(raw.path)) == L"explorer" ? raw.path : ExplorerExePath();
        key = L"path:" + CanonicalPath(explorer);
      } else if (!raw.aumid.empty()) {
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
      if (app.aumid.empty() && !folder_identity) {
        app.aumid = raw.aumid;
      }
      if (app.icon_resource.empty() && !folder_identity) {
        app.icon_resource = raw.icon_resource;
      }
      if (app.relaunch_command.empty() && !folder_identity) {
        app.relaunch_command = raw.relaunch_command;
      }
      if (folder_identity) {
        if (app.exe_path.empty()) {
          app.exe_path = !raw.path.empty() && Lower(FileStem(raw.path)) == L"explorer" ? raw.path : ExplorerExePath();
          path_cached_flag[key] = raw.path_cached ? 1 : 0;
        }
      } else if (app.exe_path.empty() && !raw.path.empty() && !IsHostExe(raw.path) && !IsFilesystemFolder(raw.path)) {
        app.exe_path = raw.path;
        path_cached_flag[key] = raw.path_cached ? 1 : 0;
      }
      if (app.display_name.empty()) {
        if (folder_identity) {
          app.display_name = DisplayNameFor(app.exe_path, {});
        } else if (!raw.aumid.empty()) {
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

  QueryPerformanceCounter(&t0);
  ingest(true);
  if (groups.empty() && !windows.empty()) {
    ingest(false);
  }
  QueryPerformanceCounter(&t1);
  const unsigned ingest_ms = static_cast<unsigned>(QpcMs(t0, t1) + 0.5);

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

  QueryPerformanceCounter(&t0);
  for (const auto& pin : pinned_paths) {
    if (IsSpotlightPin(pin)) {
      result.push_back(MakeSpotlightDockApp());
      continue;
    }
    if (!IsAumidPin(pin) && (IsSelfExecutable(pin) || IsFilesystemFolder(PinPrimary(pin)))) {
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
      if (IsFilesystemFolder(app.exe_path)) {
        continue;
      }
      app.pinned = true;
      app.can_pin = true;
      if (!app.exe_path.empty() && ExeHasLiveProcess(app.exe_path)) {
        app.running = true;
      }
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
      if (!app.exe_path.empty() && ExeHasLiveProcess(app.exe_path)) {
        app.running = true;
      }
      used_keys.push_back(app.key);
      result.push_back(std::move(app));
    }
  }
  QueryPerformanceCounter(&t1);
  const unsigned pins_ms = static_cast<unsigned>(QpcMs(t0, t1) + 0.5);

  int miss_logs = 0;
  for (const auto& key : order) {
    if (std::find(used_keys.begin(), used_keys.end(), key) != used_keys.end()) {
      continue;
    }
    DockApp& app = groups[key];
    if ((app.exe_path.empty() && app.aumid.empty()) || IsFilesystemFolder(app.exe_path)) {
      Log(L"dock", L"drop ghost key=%s path=%s aumid=%s name=%s", app.key.c_str(), app.exe_path.c_str(),
          app.aumid.c_str(), app.display_name.c_str());
      continue;
    }
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

  const unsigned total_ms = static_cast<unsigned>(GetTickCount64() - total_started);
  Log(L"perf", L"collect total=%u shell=%u enum=%u ingest=%u pins=%u (ms) windows=%u cache_hit=%u",
      total_ms, shell_ms, enum_ms, ingest_ms, pins_ms, enum_ctx.window_count, enum_ctx.cache_hits);
  Log(L"perf", L"collect enum cached=%u title=%u (ms)", cached_ms, title_ms);
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
  return TryDockLaunch(app, LaunchAumid, LaunchCommandLine, LaunchExe);
}

std::vector<HWND> CollectDesktopClearWindows() {
  struct EnumCtx {
    std::vector<HWND>* windows = nullptr;
    IVirtualDesktopManager* vdm = nullptr;
  } ctx;
  std::vector<HWND> windows;
  ctx.windows = &windows;
  ctx.vdm = DesktopManager();
  EnumWindows(
      [](HWND hwnd, LPARAM lp) -> BOOL {
        auto* ctx = reinterpret_cast<EnumCtx*>(lp);
        if (!IsTaskWindow(hwnd) || IsIconic(hwnd) || !IsWindowVisible(hwnd) || !OnCurrentDesktop(ctx->vdm, hwnd)) {
          return TRUE;
        }
        ctx->windows->push_back(hwnd);
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&ctx));
  return windows;
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
