#include "dock.hpp"

#include "dwm.hpp"
#include "fullscreen.hpp"
#include "icon_cache.hpp"
#include "log.hpp"
#include "menu_bar.hpp"
#include "taskbar_controller.hpp"
#include "theme.hpp"
#include "watchdog.hpp"

#include <commctrl.h>
#include <commoncontrols.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <knownfolders.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <timeapi.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <winreg.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <utility>
#include <vector>

namespace bamti {
namespace {

constexpr int kIconDip = 36;
constexpr int kSlotDip = 64;
constexpr int kHeightDip = 64;
constexpr int kHoverInsetDip = 4;
constexpr int kPadXDip = 10;
constexpr int kGroupGapDip = 12;
constexpr int kDragSlopDip = 6;
constexpr int kMarginBottomDip = 8;
constexpr int kHotDip = 8;
constexpr int kMenuPadDip = 6;
constexpr int kMenuRowDip = 28;
constexpr int kMenuSepDip = 8;
constexpr int kMenuMinWidthDip = 168;
constexpr int kMenuMaxWidthDip = 280;
constexpr int kMenuTextPadDip = 12;
constexpr int kMenuCheckDip = 16;
constexpr int kMenuArrowDip = 14;
constexpr UINT kHideDelayMs = 100;
constexpr UINT kRebuildDelayMs = 300;
constexpr UINT_PTR kHideTimerId = 1;
constexpr UINT_PTR kPollTimerId = 2;
constexpr UINT_PTR kRebuildTimerId = 3;
constexpr UINT_PTR kTrayWatchTimerId = 4;
constexpr UINT_PTR kAnimTimerId = 5;
constexpr UINT_PTR kWarmupTimerId = 7;
constexpr UINT kWarmupDelayMs = 3000;
constexpr UINT kTrayWatchMs = 5000;
constexpr UINT kAnimTimerMs = 8;
constexpr UINT kIdlePollMs = 500;
constexpr UINT kTasksChangedMsg = WM_APP + 20;
constexpr UINT kMenuCommandMsg = WM_APP + 21;
constexpr UINT kTrayChangedMsg = WM_APP + 22;
constexpr UINT kFullscreenMsg = WM_APP + 23;
constexpr UINT kPinCommand = 1;
constexpr UINT kUnpinCommand = 2;
constexpr UINT kQuitCommand = 3;
constexpr UINT kShowAllCommand = 4;
constexpr UINT kHideCommand = 5;
constexpr UINT kNewWindowCommand = 6;
constexpr UINT kOpenCommand = 7;
constexpr UINT kOptionsCommand = 8;
constexpr UINT kToggleLoginCommand = 9;
constexpr UINT kShowInFolderCommand = 10;
constexpr UINT kWindowCommandBase = 100;

HWND g_notify = nullptr;
ULONGLONG g_rbutton_down_at = 0;
std::atomic<bool> g_rebuild_posted{false};
std::atomic<bool> g_tray_posted{false};
UINT g_tray_msg_count = 0;
ULONGLONG g_tray_msg_window = 0;
UINT g_task_msg_count = 0;
ULONGLONG g_task_msg_window = 0;
UINT g_fullscreen_msg_count = 0;
ULONGLONG g_fullscreen_msg_window = 0;
UINT g_popup_closed_count = 0;
ULONGLONG g_popup_closed_window = 0;
UINT g_menu_cmd_count = 0;
ULONGLONG g_menu_cmd_window = 0;

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

int DisplayRefreshHz() {
  DWM_TIMING_INFO info{};
  info.cbSize = sizeof(info);
  if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &info)) && info.rateRefresh.uiDenominator != 0) {
    return static_cast<int>((info.rateRefresh.uiNumerator + info.rateRefresh.uiDenominator / 2) /
                            info.rateRefresh.uiDenominator);
  }
  const HDC hdc = GetDC(nullptr);
  if (hdc == nullptr) {
    return 0;
  }
  const int hz = GetDeviceCaps(hdc, VREFRESH);
  ReleaseDC(nullptr, hdc);
  return hz;
}

const wchar_t* MenuCmdName(UINT cmd) {
  if (cmd >= kWindowCommandBase) {
    return L"window";
  }
  switch (cmd) {
    case kPinCommand:
      return L"pin";
    case kUnpinCommand:
      return L"unpin";
    case kQuitCommand:
      return L"quit";
    case kShowAllCommand:
      return L"show-all";
    case kHideCommand:
      return L"hide";
    case kNewWindowCommand:
      return L"new-window";
    case kOpenCommand:
      return L"open";
    case kOptionsCommand:
      return L"options";
    case kToggleLoginCommand:
      return L"login";
    case kShowInFolderCommand:
      return L"show-in-folder";
    default:
      return L"none";
  }
}

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

std::wstring CollectSnap(const std::vector<DockApp>& apps) {
  std::wstring snap;
  snap.reserve(apps.size() * 64);
  for (const auto& app : apps) {
    snap += app.key;
    snap.push_back(L'|');
    snap += std::to_wstring(reinterpret_cast<std::uintptr_t>(app.hwnd));
    snap.push_back(L'|');
    snap.push_back(app.running ? L'1' : L'0');
    snap.push_back(app.pinned ? L'1' : L'0');
    snap.push_back(L'|');
    snap += std::to_wstring(app.windows.size());
    snap.push_back(L'\n');
  }
  return snap;
}

std::wstring JoinItemNames(const std::vector<DockApp>& apps) {
  std::wstring joined;
  const size_t n = (std::min)(apps.size(), static_cast<size_t>(8));
  for (size_t i = 0; i < n; ++i) {
    if (i != 0) {
      joined += L">";
    }
    joined += apps[i].display_name;
  }
  return joined;
}

template <typename T>
void RotatePinnedRange(std::vector<T>& vec, int from, int to) {
  if (from < 0 || to < 0 || from == to) {
    return;
  }
  if (from >= static_cast<int>(vec.size()) || to >= static_cast<int>(vec.size())) {
    return;
  }
  if (to < from) {
    std::rotate(vec.begin() + to, vec.begin() + from, vec.begin() + from + 1);
  } else {
    std::rotate(vec.begin() + from, vec.begin() + from + 1, vec.begin() + to + 1);
  }
}

HMONITOR PrimaryMonitor() {
  HMONITOR found = nullptr;
  EnumDisplayMonitors(
      nullptr, nullptr,
      [](HMONITOR monitor, HDC, LPRECT, LPARAM lp) -> BOOL {
        MONITORINFO info{};
        info.cbSize = sizeof(info);
        if (GetMonitorInfoW(monitor, &info) && (info.dwFlags & MONITORINFOF_PRIMARY) != 0) {
          *reinterpret_cast<HMONITOR*>(lp) = monitor;
          return FALSE;
        }
        return TRUE;
      },
      reinterpret_cast<LPARAM>(&found));
  if (found != nullptr) {
    return found;
  }
  const POINT origin{0, 0};
  return MonitorFromPoint(origin, MONITOR_DEFAULTTOPRIMARY);
}

MONITORINFO PrimaryMonitorInfo() {
  MONITORINFO info{};
  info.cbSize = sizeof(info);
  GetMonitorInfoW(PrimaryMonitor(), &info);
  return info;
}

ID2D1Factory* D2dFactory() {
  static Microsoft::WRL::ComPtr<ID2D1Factory> factory;
  if (!factory) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
  }
  return factory.Get();
}

// 셸 아이콘 캐시는 48/96/256 단계로 관리된다. 그리는 데 필요한 크기 이상인 가장 작은
// 단계를 요청하면, 원본 자산이 작은 앱에서 셸이 크게 확대한 비트맵을 돌려주는 일을 막는다.
int ShellIconRequestPx(int px) {
  if (px <= 48) {
    return 48;
  }
  if (px <= 96) {
    return 96;
  }
  return 256;
}

HBITMAP BitmapFromShellItem(const std::wstring& path, int request_px) {
  Microsoft::WRL::ComPtr<IShellItem> item;
  if (FAILED(SHCreateItemFromParsingName(path.c_str(), nullptr, IID_PPV_ARGS(&item))) || !item) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
  if (FAILED(item.As(&factory)) || !factory) {
    return nullptr;
  }
  HBITMAP bmp = nullptr;
  const SIZE size{request_px, request_px};
  if (FAILED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp))) {
    return nullptr;
  }
  return bmp;
}

HBITMAP BitmapFromShellItemObject(IShellItem* item, int request_px) {
  if (item == nullptr) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IShellItemImageFactory> factory;
  if (FAILED(item->QueryInterface(IID_PPV_ARGS(&factory))) || !factory) {
    return nullptr;
  }
  HBITMAP bmp = nullptr;
  const SIZE size{request_px, request_px};
  if (FAILED(factory->GetImage(size, SIIGBF_ICONONLY | SIIGBF_BIGGERSIZEOK, &bmp))) {
    return nullptr;
  }
  return bmp;
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
    if (SUCCEEDED(SHCreateItemInKnownFolder(FOLDERID_AppsFolder, 0, alt.c_str(), IID_PPV_ARGS(&item))) && item) {
      return item;
    }
    item.Reset();
  }
  const std::wstring parsing = L"shell:AppsFolder\\" + aumid;
  SHCreateItemFromParsingName(parsing.c_str(), nullptr, IID_PPV_ARGS(&item));
  return item;
}

HBITMAP BitmapFromAumid(const std::wstring& aumid, int request_px) {
  Microsoft::WRL::ComPtr<IShellItem> item = ShellItemFromAumid(aumid);
  if (!item) {
    return nullptr;
  }
  return BitmapFromShellItemObject(item.Get(), request_px);
}

bool PathImpliesGenericIcon(const std::wstring& path) {
  if (path.empty()) {
    return false;
  }
  std::wstring lower = path;
  CharLowerBuffW(lower.data(), static_cast<DWORD>(lower.size()));
  if (lower.find(L"\\windowsapps\\") != std::wstring::npos ||
      lower.find(L"\\systemapps\\") != std::wstring::npos) {
    return true;
  }
  const size_t slash = lower.find_last_of(L"\\/");
  std::wstring name = slash == std::wstring::npos ? lower : lower.substr(slash + 1);
  const size_t dot = name.find_last_of(L'.');
  if (dot != std::wstring::npos) {
    name.resize(dot);
  }
  return name == L"applicationframehost" || name == L"wwahost" || name == L"dllhost" || name == L"runtimebroker" ||
         name == L"openwith";
}

HBITMAP BitmapFromIconResource(const std::wstring& resource, int px) {
  if (resource.empty()) {
    return nullptr;
  }
  std::wstring spec(32768, L'\0');
  DWORD n = ExpandEnvironmentStringsW(resource.c_str(), spec.data(), static_cast<DWORD>(spec.size()));
  if (n == 0 || n > spec.size()) {
    spec = resource;
  } else {
    spec.resize(n - 1);
  }

  int index = 0;
  bool has_index = false;
  std::wstring path = spec;
  const size_t comma = spec.find_last_of(L',');
  if (comma != std::wstring::npos && comma > 0 && comma + 1 < spec.size()) {
    const wchar_t* suffix = spec.c_str() + comma + 1;
    wchar_t* end = nullptr;
    const long parsed = wcstol(suffix, &end, 10);
    if (end != suffix && (end == nullptr || *end == L'\0')) {
      index = static_cast<int>(parsed);
      has_index = true;
      path = spec.substr(0, comma);
    }
  }

  auto extract = [&]() -> HBITMAP {
    HICON icon = nullptr;
    const UINT got =
        PrivateExtractIconsW(path.c_str(), has_index ? index : 0, ShellIconRequestPx(px), ShellIconRequestPx(px), &icon,
                             nullptr, 1, LR_DEFAULTCOLOR);
    if (got == 0 || icon == nullptr) {
      return nullptr;
    }
    HBITMAP ready = BitmapFromIcon(icon, px);
    DestroyIcon(icon);
    return ready;
  };
  const wchar_t* ext = PathFindExtensionW(path.c_str());
  const bool pe = ext != nullptr &&
                  (_wcsicmp(ext, L".exe") == 0 || _wcsicmp(ext, L".dll") == 0 || _wcsicmp(ext, L".ico") == 0);
  if (pe) {
    if (HBITMAP ready = extract()) {
      return ready;
    }
  }
  if (HBITMAP shell = BitmapFromShellItem(path, ShellIconRequestPx(px))) {
    if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
      return ready;
    }
  }
  if (!pe) {
    return extract();
  }
  return nullptr;
}

HBITMAP BitmapFromJumboList(const std::wstring& path) {
  SHFILEINFOW info{};
  if (SHGetFileInfoW(path.c_str(), 0, &info, sizeof(info), SHGFI_SYSICONINDEX) == 0) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IImageList> list;
  if (FAILED(SHGetImageList(SHIL_JUMBO, IID_PPV_ARGS(&list))) || !list) {
    return nullptr;
  }
  HICON icon = nullptr;
  if (FAILED(list->GetIcon(info.iIcon, ILD_TRANSPARENT, &icon)) || icon == nullptr) {
    return nullptr;
  }
  ICONINFO ii{};
  int native = 256;
  if (GetIconInfo(icon, &ii) != FALSE) {
    BITMAP bm{};
    if (ii.hbmColor != nullptr && GetObjectW(ii.hbmColor, sizeof(bm), &bm) != 0) {
      native = (std::max)(bm.bmWidth, std::abs(bm.bmHeight));
    }
    if (ii.hbmColor != nullptr) {
      DeleteObject(ii.hbmColor);
    }
    if (ii.hbmMask != nullptr) {
      DeleteObject(ii.hbmMask);
    }
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = native;
  bmi.bmiHeader.biHeight = -native;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp != nullptr) {
    const HDC dc = CreateCompatibleDC(nullptr);
    const HGDIOBJ old = SelectObject(dc, bmp);
    DrawIconEx(dc, 0, 0, icon, native, native, 0, nullptr, DI_NORMAL);
    SelectObject(dc, old);
    DeleteDC(dc);
  }
  DestroyIcon(icon);
  return bmp;
}

HBITMAP BitmapFromFluentSearch(int px, bool dark) {
  if (px <= 0) {
    return nullptr;
  }
  ID2D1Factory* d2d = D2dFactory();
  if (d2d == nullptr) {
    return nullptr;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = px;
  bmi.bmiHeader.biHeight = -px;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HDC mem = CreateCompatibleDC(nullptr);
  if (mem == nullptr) {
    return nullptr;
  }
  HBITMAP bmp = CreateDIBSection(mem, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp == nullptr || bits == nullptr) {
    DeleteDC(mem);
    return nullptr;
  }
  const HGDIOBJ old_bmp = SelectObject(mem, bmp);
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(d2d->CreateDCRenderTarget(&props, rt.GetAddressOf()))) {
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    return nullptr;
  }
  RECT box{0, 0, px, px};
  if (FAILED(rt->BindDC(mem, &box))) {
    SelectObject(mem, old_bmp);
    DeleteObject(bmp);
    DeleteDC(mem);
    return nullptr;
  }
  D2D1_STROKE_STYLE_PROPERTIES stroke_props{};
  stroke_props.startCap = D2D1_CAP_STYLE_ROUND;
  stroke_props.endCap = D2D1_CAP_STYLE_ROUND;
  stroke_props.dashCap = D2D1_CAP_STYLE_ROUND;
  stroke_props.lineJoin = D2D1_LINE_JOIN_ROUND;
  stroke_props.miterLimit = 1.0f;
  stroke_props.dashStyle = D2D1_DASH_STYLE_SOLID;
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  const HRESULT stroke_hr = d2d->CreateStrokeStyle(stroke_props, nullptr, 0, stroke.GetAddressOf());
  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0, 0, 0, 0));
  if (SUCCEEDED(stroke_hr) && SUCCEEDED(rt->CreateSolidColorBrush(ClockTextColor(dark), brush.GetAddressOf()))) {
    const float s = static_cast<float>(px) / 16.0f;
    const float width = 1.50f * s;
    const D2D1_ELLIPSE ring = D2D1::Ellipse(D2D1::Point2F(6.75f * s, 6.75f * s), 4.10f * s, 4.10f * s);
    rt->DrawEllipse(ring, brush.Get(), width, stroke.Get());
    rt->DrawLine(D2D1::Point2F(9.65f * s, 9.65f * s), D2D1::Point2F(13.35f * s, 13.35f * s), brush.Get(), width,
                 stroke.Get());
  }
  rt->EndDraw();
  SelectObject(mem, old_bmp);
  DeleteDC(mem);
  return bmp;
}

HICON QueryWindowIcon(HWND hwnd) {
  if (hwnd == nullptr) {
    return nullptr;
  }
  auto send = [hwnd](WPARAM which) -> HICON {
    DWORD_PTR result = 0;
    if (SendMessageTimeoutW(hwnd, WM_GETICON, which, 0, SMTO_ABORTIFHUNG | SMTO_BLOCK, 50, &result) == 0) {
      return nullptr;
    }
    return reinterpret_cast<HICON>(result);
  };
  if (HICON icon = send(ICON_BIG)) {
    return icon;
  }
  if (HICON icon = send(ICON_SMALL2)) {
    return icon;
  }
  if (HICON icon = send(ICON_SMALL)) {
    return icon;
  }
  if (HICON icon = reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICON))) {
    return icon;
  }
  return reinterpret_cast<HICON>(GetClassLongPtrW(hwnd, GCLP_HICONSM));
}

constexpr wchar_t kRunSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
constexpr wchar_t kApprovedSubkey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kRunNamePrefix[] = L"bamti-dock-";
constexpr size_t kMaxRunValueName = 240;

void SanitizeRunTail(std::wstring& tail) {
  for (wchar_t& ch : tail) {
    if (ch == L'\\' || ch == L'/' || ch == L':') {
      ch = L'_';
    }
  }
}

uint32_t Fnv1a32(const std::wstring& text) {
  uint32_t hash = 2166136261u;
  for (const wchar_t ch : text) {
    hash ^= static_cast<uint32_t>(ch);
    hash *= 16777619u;
  }
  return hash;
}

std::wstring Hex8(uint32_t value) {
  std::wstring out(8, L'0');
  for (int i = 7; i >= 0; --i) {
    out[static_cast<size_t>(i)] = L"0123456789abcdef"[value & 0xfu];
    value >>= 4;
  }
  return out;
}

std::wstring MakeRunValueName(std::wstring tail) {
  SanitizeRunTail(tail);
  if (tail.empty()) {
    return {};
  }
  std::wstring name = std::wstring(kRunNamePrefix) + tail;
  if (name.size() <= kMaxRunValueName) {
    return name;
  }
  const std::wstring hash = Hex8(Fnv1a32(name));
  const size_t keep = kMaxRunValueName - 1 - hash.size();
  return name.substr(0, keep) + L"_" + hash;
}

std::wstring LoginRunValueNameLegacy(const DockApp& app) {
  std::wstring tail;
  if (!app.exe_path.empty()) {
    const wchar_t* file = PathFindFileNameW(app.exe_path.c_str());
    tail = file != nullptr ? file : app.exe_path;
  } else if (!app.aumid.empty()) {
    tail = app.aumid;
  }
  SanitizeRunTail(tail);
  if (tail.empty()) {
    return {};
  }
  return std::wstring(kRunNamePrefix) + tail;
}

std::wstring LoginRunValueName(const DockApp& app) {
  std::wstring tail;
  if (!app.aumid.empty()) {
    tail = app.aumid;
  } else if (!app.exe_path.empty()) {
    tail = app.exe_path;
  }
  return MakeRunValueName(std::move(tail));
}

std::wstring LoginRunCommand(const DockApp& app) {
  return DockLaunchCommandLine(app);
}

bool LoginValueExists(const std::wstring& name) {
  if (name.empty()) {
    return false;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD size = 0;
  const LONG st = RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size);
  RegCloseKey(key);
  return st == ERROR_SUCCESS && (type == REG_SZ || type == REG_EXPAND_SZ);
}

bool IsBamtiDockRunName(const std::wstring& name) {
  const size_t prefix_n = sizeof(kRunNamePrefix) / sizeof(kRunNamePrefix[0]) - 1;
  return name.size() >= prefix_n && name.compare(0, prefix_n, kRunNamePrefix) == 0;
}

bool StartupApprovedAllows(const std::wstring& name) {
  if (name.empty()) {
    return false;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedSubkey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return true;
  }
  DWORD type = 0;
  DWORD size = 0;
  LONG st = RegQueryValueExW(key, name.c_str(), nullptr, &type, nullptr, &size);
  if (st != ERROR_SUCCESS || type != REG_BINARY || size == 0) {
    RegCloseKey(key);
    return true;
  }
  std::vector<BYTE> data(size);
  st = RegQueryValueExW(key, name.c_str(), nullptr, &type, data.data(), &size);
  RegCloseKey(key);
  if (st != ERROR_SUCCESS || size == 0) {
    return true;
  }
  return (data[0] & 1u) == 0;
}

bool LoginValueEnabled(const std::wstring& name) {
  return LoginValueExists(name) && StartupApprovedAllows(name);
}

bool LoginItemPresent(const DockApp& app) {
  const std::wstring name = LoginRunValueName(app);
  const std::wstring legacy = LoginRunValueNameLegacy(app);
  if (LoginValueEnabled(name)) {
    return true;
  }
  return legacy != name && LoginValueEnabled(legacy);
}

void DeleteApprovedValue(const std::wstring& name) {
  if (!IsBamtiDockRunName(name)) {
    return;
  }
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kApprovedSubkey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return;
  }
  const LONG st = RegDeleteValueW(key, name.c_str());
  RegCloseKey(key);
  if (st != ERROR_SUCCESS && st != ERROR_FILE_NOT_FOUND) {
    Log(L"dock", L"login approved delete failed name=%s err=%lu", name.c_str(), static_cast<unsigned long>(st));
  }
}

LONG DeleteRunValue(HKEY key, const std::wstring& name) {
  if (name.empty()) {
    return ERROR_SUCCESS;
  }
  const LONG st = RegDeleteValueW(key, name.c_str());
  if (st == ERROR_FILE_NOT_FOUND) {
    return ERROR_SUCCESS;
  }
  return st;
}

void ToggleLoginItem(const DockApp& app) {
  const std::wstring name = LoginRunValueName(app);
  const std::wstring legacy = LoginRunValueNameLegacy(app);
  const std::wstring command = LoginRunCommand(app);
  if (name.empty() || command.empty()) {
    Log(L"dock", L"login toggle failed err=%lu", static_cast<unsigned long>(ERROR_INVALID_DATA));
    return;
  }
  HKEY key = nullptr;
  LONG st = RegCreateKeyExW(HKEY_CURRENT_USER, kRunSubkey, 0, nullptr, 0, KEY_SET_VALUE | KEY_QUERY_VALUE, nullptr,
                            &key, nullptr);
  if (st != ERROR_SUCCESS) {
    Log(L"dock", L"login toggle failed err=%lu", static_cast<unsigned long>(st));
    return;
  }
  const bool exists = LoginItemPresent(app);
  if (exists) {
    st = DeleteRunValue(key, name);
    if (st == ERROR_SUCCESS) {
      st = DeleteRunValue(key, legacy);
    }
    DeleteApprovedValue(name);
    DeleteApprovedValue(legacy);
  } else {
    const DWORD bytes = static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t));
    st = RegSetValueExW(key, name.c_str(), 0, REG_SZ, reinterpret_cast<const BYTE*>(command.c_str()), bytes);
    if (st == ERROR_SUCCESS) {
      DeleteApprovedValue(name);
    }
  }
  RegCloseKey(key);
  if (st != ERROR_SUCCESS) {
    Log(L"dock", L"login toggle failed err=%lu", static_cast<unsigned long>(st));
  } else if (exists) {
    Log(L"dock", L"login removed name=%s legacy=%s", name.c_str(), legacy.c_str());
  } else {
    Log(L"dock", L"login added name=%s cmd=%s", name.c_str(), command.c_str());
  }
}

void ShowInFolder(const DockApp& app) {
  if (app.exe_path.empty() || app.exe_path.find(L'"') != std::wstring::npos) {
    Log(L"dock", L"show in folder skipped name=%s", app.display_name.c_str());
    return;
  }
  if (GetFileAttributesW(app.exe_path.c_str()) == INVALID_FILE_ATTRIBUTES) {
    Log(L"dock", L"show in folder missing path=%s", app.exe_path.c_str());
    return;
  }
  const std::wstring args = L"/select,\"" + app.exe_path + L"\"";
  const HINSTANCE ret = ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
  if (reinterpret_cast<INT_PTR>(ret) <= 32) {
    Log(L"dock", L"show in folder failed err=%lu name=%s", static_cast<unsigned long>(reinterpret_cast<UINT_PTR>(ret)),
        app.display_name.c_str());
  }
}

}  // namespace

struct DockMenuRow {
  UINT id = 0;
  std::wstring text;
  bool separator = false;
  bool checked = false;
  bool submenu = false;
};

class DockMenuContent : public PopupContent {
 public:
  void Reset(Dock* owner, const DockApp& app) {
    owner_ = owner;
    app_ = app;
    rows_.clear();
    window_targets_.clear();
    if (owner_ == nullptr) {
      return;
    }

    window_targets_.reserve(app.windows.size());
    std::vector<DockMenuRow> window_rows;
    window_rows.reserve(app.windows.size());
    const HWND foreground = GetForegroundWindow();
    for (HWND hwnd : app.windows) {
      if (hwnd == nullptr || !IsWindow(hwnd)) {
        continue;
      }
      std::wstring title = WindowTitle(hwnd);
      if (title.empty()) {
        title = app.display_name.empty() ? std::wstring(L"(제목 없음)") : app.display_name;
      }
      if (title.size() > 48) {
        title.resize(47);
        title.push_back(L'\u2026');
      }
      const UINT id = kWindowCommandBase + static_cast<UINT>(window_targets_.size());
      window_rows.push_back({id, std::move(title), false, hwnd == foreground});
      window_targets_.push_back(hwnd);
    }

    const bool has_windows = !window_targets_.empty();
    const bool can_launch = (!app.aumid.empty() || !app.relaunch_command.empty() || !app.exe_path.empty()) &&
                            !IsSelfExecutable(app.exe_path);
    auto add_sep = [&]() {
      if (!rows_.empty() && !rows_.back().separator) {
        rows_.push_back({0, L"", true});
      }
    };
    auto add_row = [&](UINT id, std::wstring text, bool checked = false, bool submenu = false) {
      rows_.push_back({id, std::move(text), false, checked, submenu});
    };

    if (has_windows) {
      rows_.insert(rows_.end(), window_rows.begin(), window_rows.end());
      if (can_launch) {
        add_sep();
        add_row(kNewWindowCommand, L"새 창");
      }
    }
    add_sep();
    add_row(kOptionsCommand, L"옵션", false, true);
    if (has_windows) {
      add_sep();
      add_row(kShowAllCommand, L"모두 보기");
      add_row(kHideCommand, L"가리기");
      add_row(kQuitCommand, L"종료");
    } else if (can_launch) {
      add_sep();
      add_row(kOpenCommand, L"열기");
    }
    if (!rows_.empty() && rows_.back().separator) {
      rows_.pop_back();
    }
  }

  bool empty() const { return rows_.empty(); }
  size_t size() const { return rows_.size(); }
  int RowCount() const override { return static_cast<int>(rows_.size()); }
  const DockApp& App() const { return app_; }

  bool StickyRow(int index) const override {
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
      return false;
    }
    return rows_[static_cast<size_t>(index)].id == kOptionsCommand;
  }

  int OptionsIndex() const {
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      if (rows_[static_cast<size_t>(i)].id == kOptionsCommand) {
        return i;
      }
    }
    return -1;
  }

  bool RowScreenRect(int index, RECT* out) const {
    if (out == nullptr || owner_ == nullptr || owner_->popup_.hwnd() == nullptr) {
      return false;
    }
    HWND hwnd = owner_->popup_.hwnd();
    RECT client{};
    GetClientRect(hwnd, &client);
    const RECT row = RowRect(index, owner_->Dpi(), client.right);
    POINT top_left{row.left, row.top};
    POINT bottom_right{row.right, row.bottom};
    ClientToScreen(hwnd, &top_left);
    ClientToScreen(hwnd, &bottom_right);
    *out = RECT{top_left.x, top_left.y, bottom_right.x, bottom_right.y};
    return true;
  }

  SIZE Measure(UINT dpi) override {
    const int pad = DipToPx(kMenuPadDip, dpi);
    const int row_h = DipToPx(kMenuRowDip, dpi);
    const int sep_h = DipToPx(kMenuSepDip, dpi);
    const int text_pad = DipToPx(kMenuTextPadDip, dpi);
    const int check_w = DipToPx(kMenuCheckDip, dpi);
    const int arrow_w = DipToPx(kMenuArrowDip, dpi);
    int text_w = 0;
    for (const DockMenuRow& row : rows_) {
      if (row.separator || row.text.empty()) {
        continue;
      }
      text_w = (std::max)(text_w, static_cast<int>(PopupTextWidth(dpi, row.text) + 0.5f));
    }
    int width = text_w + pad * 2 + text_pad * 2 + check_w + arrow_w;
    width = (std::max)(width, DipToPx(kMenuMinWidthDip, dpi));
    width = (std::min)(width, DipToPx(kMenuMaxWidthDip, dpi));
    int height = pad * 2;
    for (const DockMenuRow& row : rows_) {
      height += row.separator ? sep_h : row_h;
    }
    return SIZE{width, height};
  }

  void Render(ID2D1RenderTarget* target, UINT dpi, int hot) override {
    if (target == nullptr) {
      return;
    }
    const D2D1_SIZE_F sz = target->GetSize();
    const RECT client{0, 0, static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)};
    const bool dark = owner_ != nullptr ? owner_->dark_ : true;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
    const D2D1_COLOR_F text_c = ClockTextColor(dark);
    const D2D1_COLOR_F hover_c = MenuItemHoverFill(dark, false);
    const D2D1_COLOR_F line_c = DockStrokeColor(dark);
    target->CreateSolidColorBrush(D2D1::ColorF(text_c.r, text_c.g, text_c.b, 1.0f), text.GetAddressOf());
    target->CreateSolidColorBrush(hover_c, hover.GetAddressOf());
    target->CreateSolidColorBrush(D2D1::ColorF(line_c.r, line_c.g, line_c.b, line_c.a), line.GetAddressOf());
    const int text_pad = DipToPx(kMenuTextPadDip, dpi);
    const int check_w = DipToPx(kMenuCheckDip, dpi);
    const int arrow_w = DipToPx(kMenuArrowDip, dpi);
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const DockMenuRow& row = rows_[static_cast<size_t>(i)];
      const RECT rc = RowRect(i, dpi, client.right);
      if (row.separator) {
        if (!line) {
          continue;
        }
        const float y = static_cast<float>(rc.top + (rc.bottom - rc.top) / 2) + 0.5f;
        target->DrawLine(D2D1::Point2F(static_cast<float>(rc.left), y),
                         D2D1::Point2F(static_cast<float>(rc.right), y), line.Get(), 1.0f);
        continue;
      }
      if (i == hot && hover) {
        target->FillRectangle(
            D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.right),
                        static_cast<float>(rc.bottom)),
            hover.Get());
      }
      if (text) {
        if (row.checked) {
          DrawPopupText(target, dpi, L"\u2713",
                        D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                    static_cast<float>(rc.left + check_w), static_cast<float>(rc.bottom)),
                        text.Get());
        }
        DrawPopupText(target, dpi, row.text,
                      D2D1::RectF(static_cast<float>(rc.left + check_w), static_cast<float>(rc.top),
                                  static_cast<float>(rc.right - arrow_w), static_cast<float>(rc.bottom)),
                      text.Get());
        if (row.submenu) {
          DrawPopupText(target, dpi, L"\u203A",
                        D2D1::RectF(static_cast<float>(rc.right - arrow_w), static_cast<float>(rc.top),
                                    static_cast<float>(rc.right - text_pad / 4), static_cast<float>(rc.bottom)),
                        text.Get());
        }
      }
    }
  }

  int HitTest(POINT client, UINT dpi) const override {
    int width = 0;
    if (owner_ != nullptr && owner_->popup_.hwnd() != nullptr) {
      RECT rc{};
      GetClientRect(owner_->popup_.hwnd(), &rc);
      width = rc.right;
    }
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      if (rows_[static_cast<size_t>(i)].separator) {
        continue;
      }
      const RECT rc = RowRect(i, dpi, width);
      if (PtInRect(&rc, client)) {
        return i;
      }
    }
    return -1;
  }

  void Invoke(int index) override {
    if (owner_ == nullptr || owner_->hwnd_ == nullptr || index < 0 || index >= static_cast<int>(rows_.size())) {
      return;
    }
    const UINT cmd = rows_[static_cast<size_t>(index)].id;
    if (cmd == 0) {
      return;
    }
    owner_->pending_menu_cmd_ = cmd;
    owner_->pending_menu_app_ = app_;
    owner_->pending_menu_windows_ = window_targets_;
    PostMessageW(owner_->hwnd_, kMenuCommandMsg, 0, 0);
  }

 private:
  RECT RowRect(int index, UINT dpi, int width) const {
    RECT result{};
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
      return result;
    }
    const int pad = DipToPx(kMenuPadDip, dpi);
    const int row_h = DipToPx(kMenuRowDip, dpi);
    const int sep_h = DipToPx(kMenuSepDip, dpi);
    int y = pad;
    for (int i = 0; i < index; ++i) {
      y += rows_[static_cast<size_t>(i)].separator ? sep_h : row_h;
    }
    const int h = rows_[static_cast<size_t>(index)].separator ? sep_h : row_h;
    result = {pad, y, width - pad, y + h};
    return result;
  }

  Dock* owner_ = nullptr;
  DockApp app_{};
  std::vector<DockMenuRow> rows_;
  std::vector<HWND> window_targets_;
};

class DockSubmenuContent : public PopupContent {
 public:
  void Reset(Dock* owner, const DockApp& app) {
    owner_ = owner;
    app_ = app;
    rows_.clear();
    if (owner_ == nullptr) {
      return;
    }
    const bool can_pin = app.pinned || (app.can_pin && !IsSelfExecutable(app.exe_path));
    if (can_pin) {
      rows_.push_back({app.pinned ? kUnpinCommand : kPinCommand, std::wstring(L"독에 유지"), false, app.pinned});
    }
    const std::wstring login_name = LoginRunValueName(app);
    const std::wstring login_cmd = LoginRunCommand(app);
    if (!login_name.empty() && !login_cmd.empty()) {
      rows_.push_back({kToggleLoginCommand, std::wstring(L"로그인 시 열기"), false, LoginItemPresent(app)});
    }
    if (!app.exe_path.empty() && app.exe_path.find(L'"') == std::wstring::npos) {
      rows_.push_back({kShowInFolderCommand, std::wstring(L"파일 위치 열기")});
    }
  }

  bool empty() const { return rows_.empty(); }
  int RowCount() const override { return static_cast<int>(rows_.size()); }

  SIZE Measure(UINT dpi) override {
    const int pad = DipToPx(kMenuPadDip, dpi);
    const int row_h = DipToPx(kMenuRowDip, dpi);
    const int sep_h = DipToPx(kMenuSepDip, dpi);
    const int text_pad = DipToPx(kMenuTextPadDip, dpi);
    const int check_w = DipToPx(kMenuCheckDip, dpi);
    const int arrow_w = DipToPx(kMenuArrowDip, dpi);
    int text_w = 0;
    for (const DockMenuRow& row : rows_) {
      if (row.separator || row.text.empty()) {
        continue;
      }
      text_w = (std::max)(text_w, static_cast<int>(PopupTextWidth(dpi, row.text) + 0.5f));
    }
    int width = text_w + pad * 2 + text_pad * 2 + check_w + arrow_w;
    width = (std::max)(width, DipToPx(kMenuMinWidthDip, dpi));
    width = (std::min)(width, DipToPx(kMenuMaxWidthDip, dpi));
    int height = pad * 2;
    for (const DockMenuRow& row : rows_) {
      height += row.separator ? sep_h : row_h;
    }
    return SIZE{width, height};
  }

  void Render(ID2D1RenderTarget* target, UINT dpi, int hot) override {
    if (target == nullptr) {
      return;
    }
    const D2D1_SIZE_F sz = target->GetSize();
    const RECT client{0, 0, static_cast<LONG>(sz.width), static_cast<LONG>(sz.height)};
    const bool dark = owner_ != nullptr ? owner_->dark_ : true;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
    Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
    const D2D1_COLOR_F text_c = ClockTextColor(dark);
    const D2D1_COLOR_F hover_c = MenuItemHoverFill(dark, false);
    target->CreateSolidColorBrush(D2D1::ColorF(text_c.r, text_c.g, text_c.b, 1.0f), text.GetAddressOf());
    target->CreateSolidColorBrush(hover_c, hover.GetAddressOf());
    const int check_w = DipToPx(kMenuCheckDip, dpi);
    const int arrow_w = DipToPx(kMenuArrowDip, dpi);
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      const DockMenuRow& row = rows_[static_cast<size_t>(i)];
      const RECT rc = RowRect(i, dpi, client.right);
      if (i == hot && hover) {
        target->FillRectangle(
            D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.right),
                        static_cast<float>(rc.bottom)),
            hover.Get());
      }
      if (text) {
        if (row.checked) {
          DrawPopupText(target, dpi, L"\u2713",
                        D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                    static_cast<float>(rc.left + check_w), static_cast<float>(rc.bottom)),
                        text.Get());
        }
        DrawPopupText(target, dpi, row.text,
                      D2D1::RectF(static_cast<float>(rc.left + check_w), static_cast<float>(rc.top),
                                  static_cast<float>(rc.right - arrow_w), static_cast<float>(rc.bottom)),
                      text.Get());
      }
    }
  }

  int HitTest(POINT client, UINT dpi) const override {
    int width = 0;
    if (owner_ != nullptr && owner_->submenu_.hwnd() != nullptr) {
      RECT rc{};
      GetClientRect(owner_->submenu_.hwnd(), &rc);
      width = rc.right;
    }
    for (int i = 0; i < static_cast<int>(rows_.size()); ++i) {
      if (rows_[static_cast<size_t>(i)].separator) {
        continue;
      }
      const RECT rc = RowRect(i, dpi, width);
      if (PtInRect(&rc, client)) {
        return i;
      }
    }
    return -1;
  }

  void Invoke(int index) override {
    if (owner_ == nullptr || owner_->hwnd_ == nullptr || index < 0 || index >= static_cast<int>(rows_.size())) {
      return;
    }
    const UINT cmd = rows_[static_cast<size_t>(index)].id;
    if (cmd == 0) {
      return;
    }
    owner_->pending_menu_cmd_ = cmd;
    owner_->pending_menu_app_ = app_;
    owner_->pending_menu_windows_ = app_.windows;
    PostMessageW(owner_->hwnd_, kMenuCommandMsg, 0, 0);
  }

 private:
  RECT RowRect(int index, UINT dpi, int width) const {
    RECT result{};
    if (index < 0 || index >= static_cast<int>(rows_.size())) {
      return result;
    }
    const int pad = DipToPx(kMenuPadDip, dpi);
    const int row_h = DipToPx(kMenuRowDip, dpi);
    const int sep_h = DipToPx(kMenuSepDip, dpi);
    int y = pad;
    for (int i = 0; i < index; ++i) {
      y += rows_[static_cast<size_t>(i)].separator ? sep_h : row_h;
    }
    const int h = rows_[static_cast<size_t>(index)].separator ? sep_h : row_h;
    result = {pad, y, width - pad, y + h};
    return result;
  }

  Dock* owner_ = nullptr;
  DockApp app_{};
  std::vector<DockMenuRow> rows_;
};

Dock::Dock() = default;

Dock::~Dock() {
  for (HWINEVENTHOOK hook : hooks_) {
    if (hook != nullptr) {
      UnhookWinEvent(hook);
    }
  }
  hooks_.clear();
  if (g_notify == hwnd_) {
    g_notify = nullptr;
  }
  ResetIconCache();
  ReleaseLayeredTarget();
  ReleaseAnimTimerPeriod();
  TaskbarController::UnwatchTray();
  StopFullscreenWatch(hwnd_);
  submenu_.Destroy();
  popup_.Destroy();
  if (hwnd_ != nullptr) {
    DestroyWindow(hwnd_);
    hwnd_ = nullptr;
  }
  if (hot_hwnd_ != nullptr) {
    DestroyWindow(hot_hwnd_);
    hot_hwnd_ = nullptr;
  }
}

bool Dock::RegisterClasses(HINSTANCE instance) {
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = instance;
  wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
  wc.lpszClassName = kDockClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  wc.lpfnWndProc = HotProc;
  wc.lpszClassName = kDockHotClass;
  wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(NULL_BRUSH));
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }
  return true;
}

bool Dock::Create(HINSTANCE instance, HWND bar_hwnd) {
  bar_hwnd_ = bar_hwnd;
  if (!RegisterClasses(instance)) {
    Log(L"dock", L"register classes failed");
    return false;
  }

  dark_ = ShellUsesDarkMode();
  pins_ = LoadDockPins();
  RepairDockPins(pins_);
  SanitizePins();
  EnsureSpotlightPin();
  Log(L"dock", L"create pins=%zu", pins_.size());

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_NOACTIVATE, kDockClass,
                          L"bamti dock", WS_POPUP, 0, 0, 0, 0, nullptr, nullptr, instance, this);
  if (hwnd_ == nullptr) {
    return false;
  }

  hot_hwnd_ = CreateWindowExW(
      WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_NOACTIVATE | WS_EX_LAYERED, kDockHotClass, L"", WS_POPUP, 0, 0, 0, 0,
      nullptr, nullptr, instance, this);
  if (hot_hwnd_ == nullptr) {
    return false;
  }
  SetLayeredWindowAttributes(hot_hwnd_, 0, 1, LWA_ALPHA);

  g_notify = hwnd_;
  ApplyBackdrop();
  CreateTooltip();
  Rebuild();
  LayoutHot();
  ShowWindow(hot_hwnd_, SW_SHOWNA);

  const DWORD hook_flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS | WINEVENT_SKIPOWNTHREAD;
  static const DWORD kEvents[] = {
      EVENT_OBJECT_CREATE,   EVENT_OBJECT_DESTROY, EVENT_OBJECT_SHOW,
      EVENT_OBJECT_HIDE,     EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED,
  };
  for (DWORD e : kEvents) {
    if (HWINEVENTHOOK hook = SetWinEventHook(e, e, nullptr, WinEventProc, 0, 0, hook_flags)) {
      hooks_.push_back(hook);
    }
  }
  TaskbarController::WatchTray(TrayWinEventProc);
  SetTimer(hwnd_, kTrayWatchTimerId, kTrayWatchMs, nullptr);
  SetTimer(hwnd_, kWarmupTimerId, kWarmupDelayMs, nullptr);
  StartFullscreenWatch(hwnd_, kFullscreenMsg);

  menu_content_ = std::make_unique<DockMenuContent>();
  submenu_content_ = std::make_unique<DockSubmenuContent>();
  if (!popup_.Create(instance, hwnd_)) {
    Log(L"dock", L"popup create failed err=%lu", GetLastError());
  }
  if (!submenu_.Create(instance, hwnd_)) {
    Log(L"dock", L"submenu create failed err=%lu", GetLastError());
  }
  popup_.SetDark(dark_);
  submenu_.SetDark(dark_);
  popup_.SetAfterTick(&Dock::AfterPopupTick, this);
  RefreshFullscreen();
  Log(L"perf", L"display refresh=%dHz", DisplayRefreshHz());
  Log(L"dock", L"ready hwnd=%p items=%zu", hwnd_, items_.size());
  return true;
}

bool Dock::CreateTooltip() {
  tooltip_ = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_NOPREFIX | TTS_ALWAYSTIP, 0, 0, 0,
                             0, hwnd_, nullptr, GetModuleHandleW(nullptr), nullptr);
  if (tooltip_ == nullptr) {
    return false;
  }
  SendMessageW(tooltip_, TTM_SETMAXTIPWIDTH, 0, 320);
  TOOLINFOW info{};
  info.cbSize = sizeof(info);
  info.uFlags = TTF_SUBCLASS | TTF_TRANSPARENT;
  info.hwnd = hwnd_;
  info.uId = 1;
  GetClientRect(hwnd_, &info.rect);
  info.lpszText = LPSTR_TEXTCALLBACKW;
  SendMessageW(tooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&info));
  return true;
}

void Dock::ApplyBackdrop() {
  if (hwnd_ == nullptr) {
    return;
  }
  const int corner = dwm::kCornerDoNotRound;
  DwmSetWindowAttribute(hwnd_, dwm::kWindowCornerPreference, &corner, sizeof(corner));
}

LRESULT CALLBACK Dock::WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Dock* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<Dock*>(create->lpCreateParams);
    self->hwnd_ = hwnd;
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->HandleMessage(msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

LRESULT CALLBACK Dock::HotProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  Dock* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* create = reinterpret_cast<CREATESTRUCTW*>(lparam);
    self = static_cast<Dock*>(create->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  } else {
    self = reinterpret_cast<Dock*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self != nullptr) {
    return self->HandleHot(hwnd, msg, wparam, lparam);
  }
  return DefWindowProcW(hwnd, msg, wparam, lparam);
}

void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD event, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
  if (object != OBJID_WINDOW || child != CHILDID_SELF || hwnd == nullptr) {
    return;
  }
  if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
    return;
  }
  if (!IsWindowVisible(hwnd) && event != EVENT_OBJECT_DESTROY && event != EVENT_OBJECT_HIDE) {
    return;
  }
  if ((GetWindowLongW(hwnd, GWL_EXSTYLE) & WS_EX_TOOLWINDOW) != 0) {
    return;
  }
  if (event == EVENT_OBJECT_DESTROY) {
    ForgetCachedWindow(hwnd);
    if (hwnd == TaskbarController::WatchedTray()) {
      TaskbarController::UnwatchTray();
      TaskbarController::RewatchTray();
    }
  }
  if (g_notify == nullptr) {
    return;
  }
  if (!g_rebuild_posted.exchange(true)) {
    PostMessageW(g_notify, kTasksChangedMsg, 0, 0);
  }
}

void CALLBACK Dock::TrayWinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD) {
  if (TaskbarController::SuppressingTrayEvents()) {
    return;
  }
  if (g_notify == nullptr) {
    TaskbarController::Rehide();
    return;
  }
  if (!g_tray_posted.exchange(true)) {
    PostMessageW(g_notify, kTrayChangedMsg, 0, 0);
  }
}

LRESULT Dock::HandleHot(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
      if (!fullscreen_occluded_) {
        ShowPill();
        ArmHotMouseLeave();
      }
      return 0;
    case WM_MOUSELEAVE:
      hot_leave_armed_ = false;
      if (!PointerOverUi()) {
        StartHideTimer();
      }
      return 0;
    case WM_LBUTTONDOWN:
    case WM_RBUTTONDOWN:
      if (popup_.IsOpen()) {
        popup_.Close();
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

LRESULT Dock::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
  WatchdogStage(L"dock.msg");
  if (popup_.IsOpen() && GetTickCount64() - last_popup_tick_ >= 100) {
    last_popup_tick_ = GetTickCount64();
    popup_.Tick();
  }
  bool anim_pumped = false;
  if (anim_timer_on_ && AnimFrameDue()) {
    TickDragAnim();
    anim_pumped = true;
  }
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT:
      Paint();
      return 0;
    case WM_TIMER:
      if (wparam == kPollTimerId) {
        PollPointer();
      } else if (wparam == kHideTimerId) {
        hide_armed_ = false;
        KillTimer(hwnd_, kHideTimerId);
        if (!PointerOverUi() && !Busy()) {
          HidePill();
        }
      } else if (wparam == kRebuildTimerId) {
        KillTimer(hwnd_, kRebuildTimerId);
        if (Busy()) {
          pending_rebuild_ = true;
        } else {
          Rebuild();
        }
      } else if (wparam == kTrayWatchTimerId) {
        if (TaskbarController::WatchedTray() == nullptr || !IsWindow(TaskbarController::WatchedTray())) {
          TaskbarController::RewatchTray();
        }
        if (TaskbarController::Rehide()) {
          RaiseOverlays();
        }
      } else if (wparam == kAnimTimerId) {
        if (!anim_pumped) {
          TickDragAnim();
        }
      } else if (wparam == kWarmupTimerId) {
        KillTimer(hwnd_, kWarmupTimerId);
        if (pending_rebuild_) {
          warming_up_ = true;
          const ULONGLONG started = GetTickCount64();
          Rebuild();
          warming_up_ = false;
          pending_rebuild_ = true;
          Log(L"dock", L"warmup items=%zu %ums collect=%u icons=%u (ms)", items_.size(),
              static_cast<unsigned>(GetTickCount64() - started), warmup_collect_ms_, warmup_icons_ms_);
        }
      }
      return 0;
    case kTasksChangedMsg:
      NotePostedStorm(L"task", g_task_msg_count, g_task_msg_window);
      g_rebuild_posted = false;
      ScheduleRebuild();
      return 0;
    case kMenuCommandMsg: {
      NotePostedStorm(L"menu", g_menu_cmd_count, g_menu_cmd_window);
      const UINT cmd = pending_menu_cmd_;
      DockApp app = std::move(pending_menu_app_);
      std::vector<HWND> windows = std::move(pending_menu_windows_);
      pending_menu_cmd_ = 0;
      ApplyMenuCommand(cmd, app, windows);
      return 0;
    }
    case kTrayChangedMsg:
      NotePostedStorm(L"tray", g_tray_msg_count, g_tray_msg_window);
      if (TaskbarController::Rehide()) {
        RaiseOverlays();
      }
      g_tray_posted = false;
      return 0;
    case kFullscreenMsg:
      NotePostedStorm(L"fullscreen", g_fullscreen_msg_count, g_fullscreen_msg_window);
      RefreshFullscreen();
      return 0;
    case kPopupClosedMsg:
      NotePostedStorm(L"popup-closed", g_popup_closed_count, g_popup_closed_window);
      UpdateIdleTimer();
      if (pending_rebuild_) {
        ScheduleRebuild();
      }
      if (!PointerOverUi()) {
        StartHideTimer();
      }
      return 0;
    case WM_DPICHANGED:
    case WM_DISPLAYCHANGE:
      ResetIconCache();
      EnsureIcons();
      Layout();
      LayoutHot();
      return 0;
    case WM_SETTINGCHANGE: {
      const wchar_t* area = reinterpret_cast<const wchar_t*>(lparam);
      if (area != nullptr && lstrcmpiW(area, L"ImmersiveColorSet") == 0) {
        dark_ = ShellUsesDarkMode();
        ApplyBackdrop();
        popup_.SetDark(dark_);
        submenu_.SetDark(dark_);
        ResetIconCache();
        EnsureIcons();
        RenderLayered();
      }
      return 0;
    }
    case WM_MOUSEMOVE: {
      CancelHideTimer();
      ArmMouseLeave();
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      if (pressed_ >= 0 && drag_move_logs_ < 3 && NoteDragLog()) {
        ++drag_move_logs_;
        Log(L"dock", L"drag move dx=%d dy=%d slop=%d pressed=%d dragging=%d", pt.x - drag_origin_.x,
            pt.y - drag_origin_.y, Dip(kDragSlopDip), pressed_, dragging_ ? 1 : 0);
      }
      if (pressed_ >= 0) {
        BeginDragIfNeeded(pt);
        if (dragging_) {
          UpdateDrag(pt);
        }
      }
      const int hit = dragging_ ? -1 : HitTest(pt);
      if (hit != hover_) {
        hover_ = hit;
        if (!dragging_) {
          RenderLayered();
        }
      }
      return 0;
    }
    case WM_MOUSELEAVE:
      leave_armed_ = false;
      if (hover_ != -1) {
        hover_ = -1;
        if (shown_ && !dragging_) {
          RenderLayered();
        }
      }
      if (!PointerOverUi()) {
        StartHideTimer();
      }
      return 0;
    case WM_LBUTTONDOWN: {
      if (popup_.IsOpen()) {
        popup_.Close();
      }
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      drag_logs_ = 0;
      drag_move_logs_ = 0;
      pressed_ = HitTest(pt);
      dragging_ = false;
      drag_index_ = -1;
      drop_index_ = -1;
      drag_origin_ = pt;
      if (NoteDragLog()) {
        const int pinned = (pressed_ >= 0 && pressed_ < static_cast<int>(items_.size()) &&
                            items_[static_cast<size_t>(pressed_)].pinned)
                               ? 1
                               : 0;
        Log(L"dock", L"drag down index=%d pinned=%d pins=%zu", pressed_, pinned, pins_.size());
      }
      if (pressed_ >= 0) {
        SetCapture(hwnd_);
        RenderLayered();
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const bool was_dragging = dragging_;
      if (was_dragging) {
        EndDrag(true);
        return 0;
      }
      const int index = HitTest(pt);
      const int pressed = pressed_;
      pressed_ = -1;
      if (GetCapture() == hwnd_) {
        ReleaseCapture();
      }
      RenderLayered();
      if (index >= 0 && index == pressed && index < static_cast<int>(items_.size())) {
        const DockApp app = items_[static_cast<size_t>(index)];
        Log(L"dock", L"click index=%d running=%d hwnd=%p name=%s", index, app.running ? 1 : 0, app.hwnd,
            app.display_name.c_str());
        if (app.kind == DockItemKind::kSpotlight) {
          if (bar_hwnd_ != nullptr) {
            PostMessageW(bar_hwnd_, kToggleSpotlightMsg, 0, 0);
          }
        } else if (app.running && app.hwnd != nullptr) {
          ActivateHwnd(app.hwnd);
        } else {
          LaunchDockApp(app);
        }
      }
      if (pending_rebuild_) {
        ScheduleRebuild();
      }
      return 0;
    }
    case WM_CAPTURECHANGED:
      if (NoteDragLog()) {
        Log(L"dock", L"drag capture lost to=%p dragging=%d", reinterpret_cast<HWND>(lparam), dragging_ ? 1 : 0);
      }
      if (dragging_ && reinterpret_cast<HWND>(lparam) != hwnd_) {
        EndDrag(false);
      } else if (!dragging_) {
        pressed_ = -1;
      }
      return 0;
    case WM_RBUTTONDOWN:
      g_rbutton_down_at = GetTickCount64();
      Log(L"dock", L"rbutton down");
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
    case WM_RBUTTONUP: {
      const unsigned since_down =
          g_rbutton_down_at == 0 ? 0 : static_cast<unsigned>(GetTickCount64() - g_rbutton_down_at);
      Log(L"dock", L"rbutton up %ums since down", since_down);
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int index = HitTest(pt);
      if (index >= 0) {
        POINT screen = pt;
        ClientToScreen(hwnd_, &screen);
        OpenDockMenu(screen, index);
      } else if (popup_.IsOpen()) {
        popup_.Close();
      }
      return 0;
    }
    case WM_COMMAND:
      return 0;
    case WM_NOTIFY: {
      auto* header = reinterpret_cast<NMHDR*>(lparam);
      if (header->code == TTN_GETDISPINFOW) {
        auto* info = reinterpret_cast<NMTTDISPINFOW*>(lparam);
        POINT pt{};
        GetCursorPos(&pt);
        ScreenToClient(hwnd_, &pt);
        const int index = HitTest(pt);
        if (index >= 0 && index < static_cast<int>(items_.size())) {
          tooltip_text_ = items_[static_cast<size_t>(index)].display_name;
          info->lpszText = tooltip_text_.data();
        } else {
          info->lpszText = const_cast<wchar_t*>(L"");
        }
      }
      return 0;
    }
    case WM_DESTROY:
      KillTimer(hwnd_, kHideTimerId);
      KillTimer(hwnd_, kPollTimerId);
      KillTimer(hwnd_, kRebuildTimerId);
      KillTimer(hwnd_, kTrayWatchTimerId);
      KillTimer(hwnd_, kAnimTimerId);
      KillTimer(hwnd_, kWarmupTimerId);
      ReleaseAnimTimerPeriod();
      TaskbarController::UnwatchTray();
      StopFullscreenWatch(hwnd_);
      submenu_.Destroy();
      popup_.Destroy();
      if (g_notify == hwnd_) {
        g_notify = nullptr;
      }
      hwnd_ = nullptr;
      return 0;
    default:
      return DefWindowProcW(hwnd_, msg, wparam, lparam);
  }
}

void Dock::Rebuild() {
  WatchdogStage(L"dock.rebuild");
  ResetPinCmpLog();
  if (!shown_ && !warming_up_) {
    pending_rebuild_ = true;
    return;
  }
  if (Busy()) {
    pending_rebuild_ = true;
    return;
  }
  pending_rebuild_ = false;
  const ULONGLONG started = GetTickCount64();
  const ULONGLONG fp_started = GetTickCount64();
  const uint64_t fp = TaskWindowFingerprint();
  const unsigned fp_ms = static_cast<unsigned>(GetTickCount64() - fp_started);
  if (fp_ms > 2) {
    Log(L"perf", L"rebuild fingerprint %ums", fp_ms);
  }
  if (!force_collect_ && fp == last_window_fp_ && !items_.empty()) {
    if (warming_up_) {
      warmup_collect_ms_ = 0;
      warmup_icons_ms_ = 0;
    }
    Log(L"perf", L"rebuild skip fingerprint items=%zu %ums", items_.size(), fp_ms);
    return;
  }
  force_collect_ = false;
  last_window_fp_ = fp;
  EnsureSpotlightPin();
  const ULONGLONG collect_started = warming_up_ ? GetTickCount64() : 0;
  std::vector<DockApp> next = CollectDockApps(pins_);
  const unsigned collect_ms =
      warming_up_ ? static_cast<unsigned>(GetTickCount64() - collect_started) : 0;
  bool pin_miss = false;
  for (const auto& app : next) {
    if (app.running && !app.pinned) {
      pin_miss = true;
      break;
    }
  }
  if (pin_miss) {
    std::wstring list;
    for (size_t i = 0; i < pins_.size(); ++i) {
      if (i != 0) {
        list += L" ";
      }
      list += DockPinCompareForm(pins_[i]);
    }
    Log(L"dock", L"pins %s", list.c_str());
  }
  const std::wstring snap = CollectSnap(next);
  if (snap == last_collect_snap_ && !items_.empty()) {
    if (warming_up_) {
      warmup_collect_ms_ = collect_ms;
      warmup_icons_ms_ = 0;
    }
    Log(L"perf", L"rebuild skip items=%zu %ums", items_.size(),
        static_cast<unsigned>(GetTickCount64() - started));
    Log(L"dock", L"order after-rebuild %s", JoinItemNames(next).c_str());
    return;
  }
  last_collect_snap_ = snap;
  items_ = std::move(next);
  unsigned icons_ms = 0;
  {
    const ULONGLONG icons_started = warming_up_ ? GetTickCount64() : 0;
    EnsureIcons();
    if (warming_up_) {
      icons_ms += static_cast<unsigned>(GetTickCount64() - icons_started);
    }
  }
  size_t kept = 0;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].kind == DockItemKind::kSpotlight || items_[i].pinned ||
        (i < icons_.size() && icons_[i] != nullptr)) {
      if (kept != i) {
        items_[kept] = std::move(items_[i]);
      }
      ++kept;
    }
  }
  if (kept != items_.size()) {
    items_.resize(kept);
    const ULONGLONG icons_started = warming_up_ ? GetTickCount64() : 0;
    EnsureIcons();
    if (warming_up_) {
      icons_ms += static_cast<unsigned>(GetTickCount64() - icons_started);
    }
  }
  if (warming_up_) {
    warmup_collect_ms_ = collect_ms;
    warmup_icons_ms_ = icons_ms;
  }
  Log(L"perf", L"rebuild items=%zu pins=%zu shown=%d %ums", items_.size(), pins_.size(), shown_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
  Log(L"dock", L"order after-rebuild %s", JoinItemNames(items_).c_str());
  if (shown_) {
    if (items_.empty()) {
      HidePill();
    } else {
      Layout();
    }
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Dock::ResetD2dIcons() {
  d2d_icons_.clear();
}

void Dock::ReleaseLayeredTarget() {
  ResetD2dIcons();
  layered_rt_.Reset();
  squircle_.Reset();
  if (layered_mem_ != nullptr && layered_old_ != nullptr) {
    SelectObject(layered_mem_, layered_old_);
    layered_old_ = nullptr;
  }
  if (layered_dib_ != nullptr) {
    DeleteObject(layered_dib_);
    layered_dib_ = nullptr;
  }
  if (layered_mem_ != nullptr) {
    DeleteDC(layered_mem_);
    layered_mem_ = nullptr;
  }
  layered_w_ = 0;
  layered_h_ = 0;
}

bool Dock::EnsureLayeredTarget(int width, int height) {
  if (width <= 0 || height <= 0) {
    return false;
  }
  if (layered_rt_ && layered_dib_ != nullptr && layered_mem_ != nullptr && layered_w_ == width &&
      layered_h_ == height) {
    return true;
  }
  ReleaseLayeredTarget();
  ID2D1Factory* d2d = D2dFactory();
  if (d2d == nullptr) {
    return false;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  layered_mem_ = CreateCompatibleDC(nullptr);
  if (layered_mem_ == nullptr) {
    return false;
  }
  layered_dib_ = CreateDIBSection(layered_mem_, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (layered_dib_ == nullptr) {
    ReleaseLayeredTarget();
    return false;
  }
  layered_old_ = SelectObject(layered_mem_, layered_dib_);
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(d2d->CreateDCRenderTarget(&props, layered_rt_.ReleaseAndGetAddressOf()))) {
    ReleaseLayeredTarget();
    return false;
  }
  layered_w_ = width;
  layered_h_ = height;
  return true;
}

void Dock::ResetIconCache() {
  ResetD2dIcons();
  for (auto& [key, bmp] : icon_cache_) {
    if (bmp != nullptr) {
      DeleteObject(bmp);
    }
  }
  icon_cache_.clear();
}

void Dock::EnsureIcons() {
  const int px = Dip(kIconDip);
  icons_.assign(items_.size(), nullptr);
  std::map<std::wstring, bool> live;
  const ULONGLONG icons_started = warming_up_ ? GetTickCount64() : 0;
  unsigned cache_hits = 0;
  unsigned loaded = 0;
  unsigned aumid_ms = 0;
  unsigned aumid_n = 0;
  unsigned exe_shell_ms = 0;
  unsigned exe_shell_n = 0;
  unsigned exe_extract_ms = 0;
  unsigned exe_extract_n = 0;
  unsigned spotlight_ms = 0;
  unsigned spotlight_n = 0;
  unsigned other_ms = 0;
  unsigned other_n = 0;
  for (size_t i = 0; i < items_.size(); ++i) {
    live[items_[i].key] = true;
    const std::wstring key = items_[i].key + L"|" + std::to_wstring(px);
    auto it = icon_cache_.find(key);
    if (it == icon_cache_.end() || it->second == nullptr) {
      d2d_icons_.erase(key);
      const wchar_t* source = nullptr;
      LARGE_INTEGER t0{};
      LARGE_INTEGER t1{};
      if (warming_up_) {
        QueryPerformanceCounter(&t0);
      }
      icon_cache_[key] = LoadIconBitmap(items_[i], px, warming_up_ ? &source : nullptr);
      if (warming_up_) {
        QueryPerformanceCounter(&t1);
        const unsigned ms = static_cast<unsigned>(QpcMs(t0, t1) + 0.5);
        ++loaded;
        if (source != nullptr && _wcsicmp(source, L"aumid") == 0) {
          aumid_ms += ms;
          ++aumid_n;
        } else if (source != nullptr && _wcsicmp(source, L"exe_shell") == 0) {
          exe_shell_ms += ms;
          ++exe_shell_n;
        } else if (source != nullptr && _wcsicmp(source, L"exe_extract") == 0) {
          exe_extract_ms += ms;
          ++exe_extract_n;
        } else if (source != nullptr && _wcsicmp(source, L"spotlight") == 0) {
          spotlight_ms += ms;
          ++spotlight_n;
        } else if (source != nullptr) {
          other_ms += ms;
          ++other_n;
        }
      }
      it = icon_cache_.find(key);
    } else if (warming_up_) {
      ++cache_hits;
    }
    icons_[i] = it->second;
  }
  for (auto it = icon_cache_.begin(); it != icon_cache_.end();) {
    const size_t bar = it->first.rfind(L'|');
    const std::wstring app_key = bar == std::wstring::npos ? it->first : it->first.substr(0, bar);
    if (live.find(app_key) == live.end()) {
      if (it->second != nullptr) {
        DeleteObject(it->second);
      }
      d2d_icons_.erase(it->first);
      it = icon_cache_.erase(it);
    } else {
      ++it;
    }
  }
  if (warming_up_) {
    const unsigned total_ms = static_cast<unsigned>(GetTickCount64() - icons_started);
    Log(L"perf",
        L"icons total=%u loaded=%u cache_hit=%u aumid=%u/%u exe_shell=%u/%u exe_extract=%u/%u "
        L"spotlight=%u/%u other=%u/%u (ms/n)",
        total_ms, loaded, cache_hits, aumid_ms, aumid_n, exe_shell_ms, exe_shell_n, exe_extract_ms,
        exe_extract_n, spotlight_ms, spotlight_n, other_ms, other_n);
  }
}

HBITMAP Dock::LoadIconBitmap(const DockApp& app, int px, const wchar_t** source) {
  auto note = [&](const wchar_t* name) {
    if (source != nullptr) {
      *source = name;
    }
  };
  if (app.kind == DockItemKind::kSpotlight) {
    note(L"spotlight");
    return BitmapFromFluentSearch(px, dark_);
  }
  const bool identity = !app.aumid.empty() || PathImpliesGenericIcon(app.exe_path);

  const wchar_t* exe_name = PathFindFileNameW(app.exe_path.c_str());
  const bool explorer = exe_name != nullptr && _wcsicmp(exe_name, L"explorer.exe") == 0;
  if (explorer && !app.exe_path.empty()) {
    if (HBITMAP shell = BitmapFromShellItem(app.exe_path, ShellIconRequestPx(px))) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        Log(L"dock", L"icon source=%s name=%s px=%d", L"exe_shell", app.display_name.c_str(), px);
        note(L"exe_shell");
        return ready;
      }
    }
  }
  if (!app.aumid.empty()) {
    if (HBITMAP shell = BitmapFromAumid(app.aumid, ShellIconRequestPx(px))) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        Log(L"dock", L"icon source=%s name=%s px=%d", L"aumid", app.display_name.c_str(), px);
        note(L"aumid");
        return ready;
      }
    }
  }
  if (!app.icon_resource.empty()) {
    if (HBITMAP ready = BitmapFromIconResource(app.icon_resource, px)) {
      Log(L"dock", L"icon source=%s name=%s px=%d", L"icon_resource", app.display_name.c_str(), px);
      note(L"icon_resource");
      return ready;
    }
  }
  if (identity && app.hwnd != nullptr) {
    if (HBITMAP ready = BitmapFromIcon(QueryWindowIcon(app.hwnd), px)) {
      Log(L"dock", L"icon source=%s name=%s px=%d", L"window_icon", app.display_name.c_str(), px);
      note(L"window_icon");
      return ready;
    }
  }
  if (!app.exe_path.empty() && !PathImpliesGenericIcon(app.exe_path)) {
    HICON extracted = nullptr;
    const UINT got =
        PrivateExtractIconsW(app.exe_path.c_str(), 0, ShellIconRequestPx(px), ShellIconRequestPx(px), &extracted,
                             nullptr, 1, LR_DEFAULTCOLOR);
    if (got != 0 && extracted != nullptr) {
      HBITMAP ready = BitmapFromIcon(extracted, px);
      DestroyIcon(extracted);
      if (ready != nullptr) {
        Log(L"dock", L"icon source=%s name=%s px=%d", L"exe_extract", app.display_name.c_str(), px);
        note(L"exe_extract");
        return ready;
      }
    }
    if (HBITMAP shell = BitmapFromShellItem(app.exe_path, ShellIconRequestPx(px))) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        Log(L"dock", L"icon source=%s name=%s px=%d", L"exe_shell", app.display_name.c_str(), px);
        note(L"exe_shell");
        return ready;
      }
    }
    if (HBITMAP jumbo = BitmapFromJumboList(app.exe_path)) {
      if (HBITMAP ready = FinalizeIconBitmap(jumbo, px, true)) {
        Log(L"dock", L"icon source=%s name=%s px=%d", L"jumbo", app.display_name.c_str(), px);
        note(L"jumbo");
        return ready;
      }
    }
  }
  if (app.hwnd != nullptr) {
    if (HBITMAP ready = BitmapFromIcon(QueryWindowIcon(app.hwnd), px)) {
      Log(L"dock", L"icon source=%s name=%s px=%d", L"window_icon", app.display_name.c_str(), px);
      note(L"window_icon");
      return ready;
    }
  }
  if (!app.exe_path.empty()) {
    if (HBITMAP shell = BitmapFromShellItem(app.exe_path, ShellIconRequestPx(px))) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        Log(L"dock", L"icon source=%s name=%s px=%d", L"exe_shell", app.display_name.c_str(), px);
        note(L"exe_shell");
        return ready;
      }
    }
  }
  // 어떤 방법으로도 아이콘을 얻지 못한 고정 항목은 빈 칸으로 남으므로 일반 앱 아이콘을 대신 그린다.
  SHSTOCKICONINFO stock{};
  stock.cbSize = sizeof(stock);
  if (SUCCEEDED(SHGetStockIconInfo(SIID_APPLICATION, SHGSI_ICON | SHGSI_LARGEICON, &stock)) &&
      stock.hIcon != nullptr) {
    HBITMAP ready = BitmapFromIcon(stock.hIcon, px);
    DestroyIcon(stock.hIcon);
    if (ready != nullptr) {
      Log(L"dock", L"icon source=%s name=%s px=%d", L"stock", app.display_name.c_str(), px);
      note(L"stock");
      return ready;
    }
  }
  return nullptr;
}

void Dock::Layout() {
  if (hwnd_ == nullptr) {
    return;
  }
  const MONITORINFO info = PrimaryMonitorInfo();
  const int slot = Dip(kSlotDip);
  const int pad = Dip(kPadXDip);
  const int height = Dip(kHeightDip);
  const int count = static_cast<int>(items_.size());
  const int pinned = PinnedCount();
  const int gap = (pinned > 0 && pinned < count) ? Dip(kGroupGapDip) : 0;
  const int width = pad * 2 + (count > 0 ? count * slot : slot) + gap;
  const int x = info.rcMonitor.left + (info.rcMonitor.right - info.rcMonitor.left - width) / 2;
  const int y = info.rcMonitor.bottom - Dip(kMarginBottomDip) - height;
  RECT current{};
  GetWindowRect(hwnd_, &current);
  if (current.left != x || current.top != y || current.right != x + width || current.bottom != y + height) {
    SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);
  }

  slots_.assign(static_cast<size_t>(count), RECT{});
  for (int i = 0; i < count; ++i) {
    RECT slot_rect{};
    slot_rect.left = pad + i * slot + (i >= pinned ? gap : 0);
    slot_rect.top = 0;
    slot_rect.right = slot_rect.left + slot;
    slot_rect.bottom = height;
    slots_[static_cast<size_t>(i)] = slot_rect;
  }

  if (!anim_timer_on_) {
    SnapAnimX();
  }

  if (tooltip_ != nullptr) {
    TOOLINFOW ti{};
    ti.cbSize = sizeof(ti);
    ti.hwnd = hwnd_;
    ti.uId = 1;
    GetClientRect(hwnd_, &ti.rect);
    SendMessageW(tooltip_, TTM_NEWTOOLRECTW, 0, reinterpret_cast<LPARAM>(&ti));
  }
  if (shown_) {
    RenderLayered();
  }
}

void Dock::LayoutHot() {
  if (hot_hwnd_ == nullptr) {
    return;
  }
  const MONITORINFO info = PrimaryMonitorInfo();
  const int hot = DipToPx(kHotDip, Dpi());
  SetWindowPos(hot_hwnd_, HWND_TOPMOST, info.rcMonitor.left, info.rcMonitor.bottom - hot,
               info.rcMonitor.right - info.rcMonitor.left, hot, SWP_NOACTIVATE);
}

void Dock::Paint() {
  PAINTSTRUCT ps{};
  BeginPaint(hwnd_, &ps);
  RenderLayered();
  EndPaint(hwnd_, &ps);
}

void Dock::RenderLayered() {
  WatchdogStage(L"dock.render");
  if (hwnd_ == nullptr || !shown_) {
    return;
  }
  RECT client{};
  GetClientRect(hwnd_, &client);
  const int width = client.right - client.left;
  const int height = client.bottom - client.top;
  if (width <= 0 || height <= 0) {
    return;
  }

  IWICImagingFactory* wic = WicFactory();

  if (!EnsureLayeredTarget(width, height)) {
    return;
  }
  if (FAILED(layered_rt_->BindDC(layered_mem_, &client))) {
    ReleaseLayeredTarget();
    if (!EnsureLayeredTarget(width, height) || FAILED(layered_rt_->BindDC(layered_mem_, &client))) {
      return;
    }
  }
  ID2D1DCRenderTarget* rt = layered_rt_.Get();

  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

  const UINT dpi = Dpi();
  const float radius =
      corner::ClampPx(corner::ToPx(corner::kHeroDip, dpi), static_cast<float>(width), static_cast<float>(height));
  const D2D1_RECT_F pill =
      D2D1::RectF(0.5f, 0.5f, static_cast<float>(width) - 0.5f, static_cast<float>(height) - 0.5f);
  const D2D1_ROUNDED_RECT rounded{pill, radius, radius};
  ID2D1PathGeometry* squircle = squircle_.Get(D2dFactory(), pill, radius);

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> indicator;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover_fill;
  rt->CreateSolidColorBrush(DockFillColor(dark_), fill.GetAddressOf());
  rt->CreateSolidColorBrush(DockStrokeColor(dark_), stroke.GetAddressOf());
  rt->CreateSolidColorBrush(DockIndicatorColor(dark_), indicator.GetAddressOf());
  rt->CreateSolidColorBrush(MenuItemHoverFill(dark_, false), hover_fill.GetAddressOf());
  if (fill) {
    if (squircle != nullptr) {
      rt->FillGeometry(squircle, fill.Get());
    } else {
      rt->FillRoundedRectangle(rounded, fill.Get());
    }
  }
  if (stroke) {
    if (squircle != nullptr) {
      rt->DrawGeometry(squircle, stroke.Get(), 1.0f);
    } else {
      rt->DrawRoundedRectangle(rounded, stroke.Get(), 1.0f);
    }
  }

  const int icon_px = Dip(kIconDip);
  const auto order = DisplayOrder();
  const int pinned = PinnedCount();
  if (anim_x_.size() != items_.size()) {
    SnapAnimX();
  }
  auto draw_icon = [&](size_t i, float x, float y, float alpha) {
    if (i >= icons_.size() || icons_[i] == nullptr || i >= items_.size()) {
      return;
    }
    const std::wstring key = items_[i].key + L"|" + std::to_wstring(icon_px);
    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2d_bmp;
    if (const auto it = d2d_icons_.find(key); it != d2d_icons_.end() && it->second) {
      d2d_bmp = it->second;
    } else if (wic != nullptr) {
      Microsoft::WRL::ComPtr<IWICBitmap> wic_bmp;
      if (FAILED(wic->CreateBitmapFromHBITMAP(icons_[i], nullptr, WICBitmapUsePremultipliedAlpha,
                                              wic_bmp.GetAddressOf()))) {
        return;
      }
      if (FAILED(rt->CreateBitmapFromWicBitmap(wic_bmp.Get(), d2d_bmp.GetAddressOf())) || !d2d_bmp) {
        return;
      }
      d2d_icons_[key] = d2d_bmp;
    } else {
      return;
    }
    rt->DrawBitmap(d2d_bmp.Get(), D2D1::RectF(x, y, x + static_cast<float>(icon_px), y + static_cast<float>(icon_px)),
                   alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  };
  for (size_t slot_i = 0; slot_i < order.size() && slot_i < slots_.size(); ++slot_i) {
    const size_t i = order[slot_i];
    const RECT& slot = slots_[slot_i];
    const float slot_x = static_cast<float>(slot.left + (slot.right - slot.left - icon_px) / 2);
    float x = slot_x;
    if (!(dragging_ && static_cast<int>(i) == drag_index_) && i < anim_x_.size()) {
      x = anim_x_[i];
    }
    float y = static_cast<float>((height - icon_px) / 2);
    if (!dragging_ && static_cast<int>(i) == pressed_) {
      y += static_cast<float>(Dip(1));
    }
    if (!dragging_ && hover_ == static_cast<int>(slot_i) && hover_fill) {
      const float inset = static_cast<float>(Dip(kHoverInsetDip));
      const float hover_h = static_cast<float>(slot.bottom - slot.top) - inset * 2.0f;
      const float rr = corner::HoverPx(hover_h, dpi);
      const D2D1_ROUNDED_RECT bg{
          D2D1::RectF(static_cast<float>(slot.left) + inset, static_cast<float>(slot.top) + inset,
                      static_cast<float>(slot.right) - inset, static_cast<float>(slot.bottom) - inset),
          rr, rr};
      rt->FillRoundedRectangle(bg, hover_fill.Get());
    }
    draw_icon(i, x, y, 1.0f);
    if (items_[i].kind != DockItemKind::kSpotlight && items_[i].running && indicator) {
      const float dot_w = static_cast<float>(Dip(10));
      const float dot_h = static_cast<float>(Dip(3));
      const float dx = x + (static_cast<float>(icon_px) - dot_w) * 0.5f;
      const float dy = static_cast<float>(height - Dip(10));
      const D2D1_ROUNDED_RECT dot{D2D1::RectF(dx, dy, dx + dot_w, dy + dot_h), corner::PillPx(dot_h),
                                 corner::PillPx(dot_h)};
      rt->FillRoundedRectangle(dot, indicator.Get());
    }
  }

  if (stroke && pinned > 0 && pinned < static_cast<int>(slots_.size())) {
    const RECT& left = slots_[static_cast<size_t>(pinned - 1)];
    const RECT& right = slots_[static_cast<size_t>(pinned)];
    const float mid = (static_cast<float>(left.right) + static_cast<float>(right.left)) * 0.5f;
    const float top = static_cast<float>(Dip(18));
    const float bottom = static_cast<float>(height - Dip(18));
    rt->DrawLine(D2D1::Point2F(mid, top), D2D1::Point2F(mid, bottom), stroke.Get(), 1.0f);
  }

  rt->EndDraw();

  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT src{0, 0};
  SIZE size{width, height};
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, layered_mem_, &src, 0, &blend, ULW_ALPHA);
}

void Dock::ShowPill() {
  if (fullscreen_occluded_) {
    return;
  }
  TaskbarController::Rehide();
  CancelHideTimer();
  if (!shown_) {
    shown_ = true;
    if (pending_rebuild_) {
      Rebuild();
    }
    if (!shown_ || items_.empty()) {
      shown_ = false;
      UpdateIdleTimer();
      return;
    }
    Layout();
    ShowWindow(hwnd_, SW_SHOWNA);
    RaiseOverlays();
    UpdateIdleTimer();
  }
  ArmMouseLeave();
}

void Dock::HidePill() {
  leave_armed_ = false;
  CancelHideTimer();
  if (popup_.IsOpen()) {
    popup_.Close();
  }
  if (dragging_) {
    EndDrag(false);
  }
  pressed_ = -1;
  hover_ = -1;
  if (!shown_) {
    UpdateIdleTimer();
    return;
  }
  shown_ = false;
  ShowWindow(hwnd_, SW_HIDE);
  if (anim_timer_on_) {
    StopDragAnimTimer(true);
  }
  UpdateIdleTimer();
}

void Dock::StartHideTimer() {
  if (!shown_ || Busy() || hide_armed_) {
    return;
  }
  hide_armed_ = true;
  SetTimer(hwnd_, kHideTimerId, kHideDelayMs, nullptr);
}

void Dock::CancelHideTimer() {
  if (!hide_armed_) {
    return;
  }
  hide_armed_ = false;
  KillTimer(hwnd_, kHideTimerId);
}

void Dock::ArmMouseLeave() {
  if (hwnd_ == nullptr || !shown_ || leave_armed_) {
    return;
  }
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  if (TrackMouseEvent(&track) != FALSE) {
    leave_armed_ = true;
  }
}

void Dock::ArmHotMouseLeave() {
  if (hot_hwnd_ == nullptr || hot_leave_armed_) {
    return;
  }
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hot_hwnd_;
  if (TrackMouseEvent(&track) != FALSE) {
    hot_leave_armed_ = true;
  }
}

void Dock::UpdateIdleTimer() {
  if (hwnd_ == nullptr) {
    return;
  }
  if (shown_ || popup_.IsOpen()) {
    SetTimer(hwnd_, kPollTimerId, kIdlePollMs, nullptr);
  } else {
    KillTimer(hwnd_, kPollTimerId);
  }
}

void Dock::PollPointer() {
  RefreshFullscreen();
  if (popup_.IsOpen()) {
    popup_.Tick();
    CancelHideTimer();
    return;
  }
  if (fullscreen_occluded_) {
    return;
  }
  if (PointerOverUi()) {
    CancelHideTimer();
    if (PointerOverHotEdge() || shown_) {
      ShowPill();
    }
  } else if (shown_ && !Busy()) {
    StartHideTimer();
  }
}

void Dock::RefreshFullscreen() {
  SetFullscreenOccluded(IsTrueFullscreen(hwnd_) || IsTrueFullscreen(hot_hwnd_));
}

void Dock::SetFullscreenOccluded(bool occluded) {
  if (fullscreen_occluded_ == occluded) {
    return;
  }
  fullscreen_occluded_ = occluded;
  if (occluded) {
    HidePill();
    ShowWindow(hot_hwnd_, SW_HIDE);
  } else {
    LayoutHot();
    ShowWindow(hot_hwnd_, SW_SHOWNA);
    RaiseOverlays();
  }
}

void Dock::RaiseOverlays() {
  SetOverlaysTopmost(true);
  if (popup_.IsOpen() && popup_.hwnd() != nullptr) {
    SetWindowPos(popup_.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  if (submenu_.IsOpen() && submenu_.hwnd() != nullptr) {
    SetWindowPos(submenu_.hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
}

void Dock::SetOverlaysTopmost(bool topmost) {
  if (fullscreen_occluded_ && topmost) {
    return;
  }
  const HWND z = topmost ? HWND_TOPMOST : HWND_NOTOPMOST;
  const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE;
  if (hot_hwnd_ != nullptr) {
    SetWindowPos(hot_hwnd_, z, 0, 0, 0, 0, flags);
  }
  if (shown_ && hwnd_ != nullptr) {
    SetWindowPos(hwnd_, z, 0, 0, 0, 0, flags);
  }
}

void Dock::OpenDockMenu(POINT screen, int index) {
  WatchdogStage(L"dock.menu");
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    return;
  }
  const ULONGLONG started = GetTickCount64();
  if (last_menu_open_ != 0 && started - last_menu_open_ < 100) {
    Log(L"dock", L"menu reopen storm %ums since last", static_cast<unsigned>(started - last_menu_open_));
  }
  last_menu_open_ = started;
  CloseOptionsSubmenu(L"reopen");
  popup_.Close();
  CancelHideTimer();
  if (tooltip_ != nullptr) {
    SendMessageW(tooltip_, TTM_POP, 0, 0);
  }
  if (!menu_content_) {
    menu_content_ = std::make_unique<DockMenuContent>();
  }
  const DockApp& app = items_[static_cast<size_t>(index)];
  if (app.kind == DockItemKind::kSpotlight) {
    return;
  }
  Log(L"dock", L"menu open index=%d name=%s running=%d windows=%zu pinned=%d", index, app.display_name.c_str(),
      app.running ? 1 : 0, app.windows.size(), app.pinned ? 1 : 0);
  menu_content_->Reset(this, app);
  if (menu_content_->empty()) {
    Log(L"dock", L"menu empty index=%d", index);
    return;
  }
  popup_.SetDark(dark_);
  if (!popup_.Open(menu_content_.get(), screen, PopupSurface::Anchor::AboveAt)) {
    Log(L"dock", L"menu open failed err=%lu", GetLastError());
    return;
  }
  Log(L"dock", L"menu hwnd=%p rows=%zu %ums", popup_.hwnd(), menu_content_->size(),
      static_cast<unsigned>(GetTickCount64() - started));
  UpdateIdleTimer();
}

void Dock::AfterPopupTick(void* ctx) {
  if (ctx != nullptr) {
    static_cast<Dock*>(ctx)->SyncOptionsSubmenu();
  }
}

void Dock::SyncOptionsSubmenu() {
  if (!popup_.IsOpen() || menu_content_ == nullptr) {
    CloseOptionsSubmenu(L"parent");
    return;
  }
  POINT cursor{};
  const bool got_cursor = GetCursorPos(&cursor) != FALSE;
  RECT sub{};
  const bool over_sub =
      got_cursor && submenu_.IsOpen() && submenu_.hwnd() != nullptr && GetWindowRect(submenu_.hwnd(), &sub) != FALSE &&
      PtInRect(&sub, cursor);
  const int opt = menu_content_->OptionsIndex();
  const int hot = popup_.Hot();
  if (opt >= 0 && (hot == opt || over_sub)) {
    OpenOptionsSubmenu();
  } else {
    CloseOptionsSubmenu(L"hover-leave");
  }
}

void Dock::OpenOptionsSubmenu() {
  if (!popup_.IsOpen() || menu_content_ == nullptr || submenu_.IsOpen()) {
    return;
  }
  const int opt = menu_content_->OptionsIndex();
  if (opt < 0) {
    return;
  }
  if (!submenu_content_) {
    submenu_content_ = std::make_unique<DockSubmenuContent>();
  }
  submenu_content_->Reset(this, menu_content_->App());
  if (submenu_content_->empty()) {
    return;
  }
  RECT row{};
  if (!menu_content_->RowScreenRect(opt, &row)) {
    return;
  }
  const POINT anchor{row.right, row.top};
  popup_.SetAllied(&submenu_);
  submenu_.SetDark(dark_);
  if (!submenu_.Open(submenu_content_.get(), anchor, PopupSurface::Anchor::RightOf, false)) {
    popup_.SetAllied(nullptr);
    Log(L"popup", L"submenu open failed err=%lu", GetLastError());
    return;
  }
  RaiseOverlays();
}

void Dock::CloseOptionsSubmenu(const wchar_t* reason) {
  if (!submenu_.IsOpen()) {
    popup_.SetAllied(nullptr);
    return;
  }
  Log(L"popup", L"submenu close reason=%s", reason != nullptr ? reason : L"explicit");
  popup_.SetAllied(nullptr);
  submenu_.Close();
}

void Dock::ApplyMenuCommand(UINT cmd, const DockApp& app, const std::vector<HWND>& window_cmds) {
  Log(L"dock", L"menu cmd=%u %s name=%s windows=%zu", cmd, MenuCmdName(cmd), app.display_name.c_str(),
      app.windows.size());
  if (cmd >= kWindowCommandBase) {
    const size_t window_index = static_cast<size_t>(cmd - kWindowCommandBase);
    if (window_index < window_cmds.size()) {
      ActivateHwnd(window_cmds[window_index]);
    }
  } else if (cmd == kShowAllCommand) {
    RestoreHwnds(app.windows);
  } else if (cmd == kHideCommand) {
    HideHwnds(app.windows);
  } else if (cmd == kPinCommand) {
    const std::wstring id = DockPinId(app);
    if (app.can_pin && !id.empty()) {
      std::wstring stored = id;
      if (!app.relaunch_command.empty()) {
        stored.push_back(L'\t');
        stored += app.relaunch_command;
      }
      const bool exists =
          std::any_of(pins_.begin(), pins_.end(), [&](const std::wstring& pin) { return SameDockPin(pin, id); });
      if (!exists) {
        pins_.push_back(std::move(stored));
        SaveDockPins(pins_);
      }
      pending_rebuild_ = true;
      force_collect_ = true;
    }
  } else if (cmd == kUnpinCommand) {
    const std::wstring id = DockPinId(app);
    pins_.erase(std::remove_if(pins_.begin(), pins_.end(),
                               [&](const std::wstring& pin) { return SameDockPin(pin, id); }),
                pins_.end());
    SaveDockPins(pins_);
    pending_rebuild_ = true;
    force_collect_ = true;
  } else if (cmd == kQuitCommand) {
    CloseHwnds(app.windows);
  } else if (cmd == kNewWindowCommand || cmd == kOpenCommand) {
    if (!LaunchDockApp(app)) {
      Log(L"dock", L"%s failed name=%s", MenuCmdName(cmd), app.display_name.c_str());
    }
  } else if (cmd == kToggleLoginCommand) {
    ToggleLoginItem(app);
  } else if (cmd == kShowInFolderCommand) {
    ShowInFolder(app);
  }

  if (pending_rebuild_) {
    ScheduleRebuild();
  }
  if (!PointerOverUi()) {
    StartHideTimer();
  }
}

void Dock::EnsureSpotlightPin() {
  std::vector<std::wstring> next;
  next.reserve(pins_.size() + 1);
  bool found = false;
  for (std::wstring& pin : pins_) {
    if (IsSpotlightPin(pin)) {
      if (found) {
        continue;
      }
      pin = kSpotlightPin;
      found = true;
    }
    next.push_back(std::move(pin));
  }
  pins_ = std::move(next);
  if (!found) {
    pins_.insert(pins_.begin(), kSpotlightPin);
  }
}

void Dock::SanitizePins() {
  const auto before = pins_.size();
  pins_.erase(std::remove_if(pins_.begin(), pins_.end(),
                             [](const std::wstring& path) {
                               if (IsSpotlightPin(path)) {
                                 return false;
                               }
                               const std::wstring primary = path.substr(0, path.find(L'\t'));
                               if (IsSelfExecutable(primary)) {
                                 return true;
                               }
                               const DWORD attr = GetFileAttributesW(primary.c_str());
                               return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY) != 0;
                             }),
              pins_.end());
  if (pins_.size() != before) {
    SaveDockPins(pins_);
  }
}

void Dock::ScheduleRebuild() {
  pending_rebuild_ = true;
  if (hwnd_ == nullptr || !shown_ || Busy()) {
    return;
  }
  SetTimer(hwnd_, kRebuildTimerId, kRebuildDelayMs, nullptr);
}

bool Dock::Busy() const {
  return popup_.IsOpen() || dragging_ || pressed_ >= 0;
}

int Dock::PinnedCount() const {
  int n = 0;
  for (const auto& app : items_) {
    if (!app.pinned) {
      break;
    }
    ++n;
  }
  return n;
}

int Dock::DropIndexAt(POINT client) const {
  const int pinned = PinnedCount();
  if (pinned <= 0) {
    return -1;
  }
  int best = 0;
  int best_dist = INT_MAX;
  for (int i = 0; i < pinned && i < static_cast<int>(slots_.size()); ++i) {
    const RECT& slot = slots_[static_cast<size_t>(i)];
    const int cx = slot.left + (slot.right - slot.left) / 2;
    const int dist = client.x > cx ? client.x - cx : cx - client.x;
    if (dist < best_dist) {
      best_dist = dist;
      best = i;
    }
  }
  return best;
}

std::vector<size_t> Dock::DisplayOrder() const {
  std::vector<size_t> order(items_.size());
  std::iota(order.begin(), order.end(), 0);
  if (!dragging_ || drag_index_ < 0 || drop_index_ < 0) {
    return order;
  }
  const int pinned = PinnedCount();
  if (drag_index_ < 0 || drop_index_ < 0 || drag_index_ >= pinned || drop_index_ >= pinned) {
    return order;
  }
  const int from = drag_index_;
  const int to = drop_index_;
  if (from == to) {
    return order;
  }
  if (to < from) {
    std::rotate(order.begin() + to, order.begin() + from, order.begin() + from + 1);
  } else {
    std::rotate(order.begin() + from, order.begin() + from + 1, order.begin() + to + 1);
  }
  return order;
}

float Dock::SlotIconX(size_t slot) const {
  const int icon_px = Dip(kIconDip);
  if (slot >= slots_.size()) {
    return 0.0f;
  }
  const RECT& rect = slots_[slot];
  return static_cast<float>(rect.left + (rect.right - rect.left - icon_px) / 2);
}

void Dock::SnapAnimX() {
  anim_x_.assign(items_.size(), 0.0f);
  const auto order = DisplayOrder();
  for (size_t slot = 0; slot < order.size() && slot < slots_.size(); ++slot) {
    const size_t i = order[slot];
    if (i < anim_x_.size()) {
      anim_x_[i] = SlotIconX(slot);
    }
  }
}

void Dock::HoldAnimTimerPeriod() {
  if (anim_period_held_) {
    return;
  }
  timeBeginPeriod(1);
  anim_period_held_ = true;
  ++anim_period_begin_;
}

void Dock::ReleaseAnimTimerPeriod() {
  if (!anim_period_held_) {
    return;
  }
  timeEndPeriod(1);
  anim_period_held_ = false;
  ++anim_period_end_;
  Log(L"perf", L"timer period begin=%u end=%u", anim_period_begin_, anim_period_end_);
}

bool Dock::AnimFrameDue() const {
  if (!anim_timer_on_) {
    return false;
  }
  if (last_anim_qpc_.QuadPart == 0) {
    return true;
  }
  LARGE_INTEGER now{};
  QueryPerformanceCounter(&now);
  return QpcMs(last_anim_qpc_, now) >= static_cast<double>(kAnimTimerMs);
}

void Dock::StartDragAnimTimer() {
  if (hwnd_ == nullptr) {
    return;
  }
  if (!anim_timer_on_) {
    HoldAnimTimerPeriod();
    SetTimer(hwnd_, kAnimTimerId, kAnimTimerMs, nullptr);
    anim_timer_on_ = true;
  }
  anim_frames_ = 0;
  anim_ms_sum_ = 0.0;
  anim_interval_sum_ = 0.0;
  anim_interval_min_ = 0.0;
  anim_interval_max_ = 0.0;
  QueryPerformanceCounter(&last_anim_qpc_);
  if (anim_x_.size() != items_.size()) {
    SnapAnimX();
  }
}

void Dock::StopDragAnimTimer(bool log) {
  if (hwnd_ != nullptr) {
    KillTimer(hwnd_, kAnimTimerId);
  }
  anim_timer_on_ = false;
  last_anim_qpc_ = {};
  ReleaseAnimTimerPeriod();
  if (log && anim_frames_ > 0) {
    const UINT intervals = anim_frames_ > 1 ? anim_frames_ - 1 : 0;
    const double interval_avg = intervals > 0 ? anim_interval_sum_ / static_cast<double>(intervals) : 0.0;
    const double fps = interval_avg > 0.0 ? 1000.0 / interval_avg : 0.0;
    const double render_avg = anim_ms_sum_ / static_cast<double>(anim_frames_);
    Log(L"perf", L"drag anim frames=%u interval avg=%.1f min=%.1f max=%.1f fps=%.1f render avg=%.1fms", anim_frames_,
        interval_avg, anim_interval_min_, anim_interval_max_, fps, render_avg);
  }
  anim_frames_ = 0;
  anim_ms_sum_ = 0.0;
  anim_interval_sum_ = 0.0;
  anim_interval_min_ = 0.0;
  anim_interval_max_ = 0.0;
  SnapAnimX();
}

void Dock::TickDragAnim() {
  if (hwnd_ == nullptr || !shown_ || items_.empty()) {
    StopDragAnimTimer(true);
    return;
  }
  if (anim_x_.size() != items_.size()) {
    SnapAnimX();
  }
  LARGE_INTEGER qpc_now{};
  QueryPerformanceCounter(&qpc_now);
  double dt = 16.0;
  if (last_anim_qpc_.QuadPart != 0) {
    const double interval = QpcMs(last_anim_qpc_, qpc_now);
    if (anim_interval_sum_ == 0.0 && anim_interval_max_ == 0.0) {
      anim_interval_min_ = interval;
      anim_interval_max_ = interval;
    } else {
      if (interval < anim_interval_min_) {
        anim_interval_min_ = interval;
      }
      if (interval > anim_interval_max_) {
        anim_interval_max_ = interval;
      }
    }
    anim_interval_sum_ += interval;
    dt = interval;
  }
  last_anim_qpc_ = qpc_now;
  if (dt > 100.0) {
    dt = 100.0;
  }
  const float k = static_cast<float>(1.0 - std::pow(0.8, dt / 16.0));
  const auto order = DisplayOrder();
  bool moving = false;
  for (size_t slot = 0; slot < order.size() && slot < slots_.size(); ++slot) {
    const size_t i = order[slot];
    if (i >= anim_x_.size()) {
      continue;
    }
    const float target = SlotIconX(slot);
    if (dragging_ && static_cast<int>(i) == drag_index_) {
      anim_x_[i] = target;
      continue;
    }
    const float x = anim_x_[i];
    const float delta = target - x;
    if (delta > -0.5f && delta < 0.5f) {
      anim_x_[i] = target;
    } else {
      anim_x_[i] = x + delta * k;
      moving = true;
    }
  }
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  RenderLayered();
  QueryPerformanceCounter(&t1);
  ++anim_frames_;
  anim_ms_sum_ += QpcMs(t0, t1);
  if (!dragging_ && !moving) {
    StopDragAnimTimer(true);
  }
}

void Dock::BeginDragIfNeeded(POINT client) {
  const wchar_t* reason = nullptr;
  if (dragging_) {
    reason = L"already-dragging";
  } else if (pressed_ < 0) {
    reason = L"no-press";
  } else if (pressed_ >= static_cast<int>(items_.size())) {
    reason = L"index-out-of-range";
  } else if (!items_[static_cast<size_t>(pressed_)].pinned) {
    reason = L"not-pinned";
  }
  if (reason != nullptr) {
    if (NoteDragLog()) {
      Log(L"dock", L"drag skip reason=%s", reason);
    }
    return;
  }
  const int slop = Dip(kDragSlopDip);
  const int dx = client.x - drag_origin_.x;
  const int dy = client.y - drag_origin_.y;
  if (dx * dx + dy * dy < slop * slop) {
    if (NoteDragLog()) {
      Log(L"dock", L"drag skip reason=%s", L"below-slop");
    }
    return;
  }
  dragging_ = true;
  drag_index_ = pressed_;
  drop_index_ = pressed_;
  hover_ = -1;
  if (tooltip_ != nullptr) {
    SendMessageW(tooltip_, TTM_POP, 0, 0);
  }
  StartDragAnimTimer();
}

bool Dock::NoteDragLog() {
  if (drag_logs_ >= 8) {
    return false;
  }
  ++drag_logs_;
  return true;
}

void Dock::UpdateDrag(POINT client) {
  if (!dragging_) {
    return;
  }
  const int next = DropIndexAt(client);
  if (next < 0 || next == drop_index_) {
    return;
  }
  drop_index_ = next;
  TickDragAnim();
}

void Dock::EndDrag(bool commit) {
  const bool was_dragging = dragging_;
  const int from = drag_index_;
  const int to = drop_index_;
  dragging_ = false;
  drag_index_ = -1;
  drop_index_ = -1;
  pressed_ = -1;
  if (GetCapture() == hwnd_) {
    ReleaseCapture();
  }
  if (was_dragging && commit && from >= 0 && to >= 0 && from != to) {
    const int pinned = static_cast<int>(pins_.size());
    if (from < pinned && to < pinned) {
      const std::wstring moved = pins_[static_cast<size_t>(from)];
      pins_.erase(pins_.begin() + from);
      pins_.insert(pins_.begin() + to, moved);
      RotatePinnedRange(items_, from, to);
      RotatePinnedRange(icons_, from, to);
      RotatePinnedRange(anim_x_, from, to);
      SaveDockPins(pins_);
      pending_rebuild_ = true;
      force_collect_ = true;
      Log(L"dock", L"order after-drop %s", JoinItemNames(items_).c_str());
    }
  }
  if (pending_rebuild_) {
    ScheduleRebuild();
  }
  if (shown_) {
    if (was_dragging && anim_timer_on_) {
      TickDragAnim();
    } else {
      RenderLayered();
    }
  } else if (was_dragging) {
    StopDragAnimTimer(true);
  }
}

int Dock::HitTest(POINT client) const {
  for (size_t i = 0; i < slots_.size(); ++i) {
    if (PtInRect(&slots_[i], client)) {
      return static_cast<int>(i);
    }
  }
  return -1;
}

bool Dock::PointerOverUi() const {
  if (Busy()) {
    return true;
  }
  POINT pt{};
  GetCursorPos(&pt);
  if (shown_ && hwnd_ != nullptr) {
    RECT rc{};
    GetWindowRect(hwnd_, &rc);
    if (PtInRect(&rc, pt)) {
      return true;
    }
  }
  return PointerOverHotEdge();
}

bool Dock::PointerOverHotEdge() const {
  POINT pt{};
  GetCursorPos(&pt);
  if (hot_hwnd_ != nullptr) {
    RECT hot_rc{};
    if (GetWindowRect(hot_hwnd_, &hot_rc) && PtInRect(&hot_rc, pt)) {
      return true;
    }
  }
  const MONITORINFO info = PrimaryMonitorInfo();
  const int hot = DipToPx(kHotDip, Dpi());
  RECT edge{info.rcMonitor.left, info.rcMonitor.bottom - hot, info.rcMonitor.right, info.rcMonitor.bottom};
  return PtInRect(&edge, pt) != FALSE;
}

UINT Dock::Dpi() const {
  HWND source = hwnd_ != nullptr ? hwnd_ : hot_hwnd_;
  if (source == nullptr) {
    return 96;
  }
  const UINT dpi = GetDpiForWindow(source);
  return dpi == 0 ? 96 : dpi;
}

int Dock::Dip(int value) const {
  return DipToPx(value, Dpi());
}

}  // namespace bamti
