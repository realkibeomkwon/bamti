#include "dock.hpp"

#include "dwm.hpp"
#include "fullscreen.hpp"
#include "log.hpp"
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
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <numeric>
#include <utility>
#include <vector>

namespace bamti {
namespace {

constexpr int kIconDip = 36;
constexpr int kSlotDip = 52;
constexpr int kHeightDip = 64;
constexpr int kPadXDip = 10;
constexpr int kGroupGapDip = 12;
constexpr int kDragSlopDip = 6;
constexpr int kMarginBottomDip = 8;
constexpr int kHotDip = 8;
// macOS Dock is ~20pt at the default bar height (~64pt). DWM ROUND/ROUNDSMALL cannot
// express that, so the pill is drawn with Direct2D.
constexpr int kCornerRadiusDip = 20;
constexpr int kMenuPadDip = 6;
constexpr int kMenuRowDip = 28;
constexpr int kMenuSepDip = 8;
constexpr int kMenuMinWidthDip = 168;
constexpr int kMenuMaxWidthDip = 280;
constexpr int kMenuTextPadDip = 12;
constexpr UINT kHideDelayMs = 100;
constexpr UINT kRebuildDelayMs = 300;
constexpr UINT_PTR kHideTimerId = 1;
constexpr UINT_PTR kPollTimerId = 2;
constexpr UINT_PTR kRebuildTimerId = 3;
constexpr UINT_PTR kTrayWatchTimerId = 4;
constexpr UINT kTrayWatchMs = 5000;
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

COLORREF Channel(float x) {
  return static_cast<COLORREF>(std::clamp(static_cast<int>(x * 255.0f + 0.5f), 0, 255));
}

COLORREF OpaqueColor(const D2D1_COLOR_F& c) {
  return RGB(Channel(c.r), Channel(c.g), Channel(c.b));
}

COLORREF BlendOn(COLORREF under, const D2D1_COLOR_F& over) {
  const float a = std::clamp(over.a, 0.0f, 1.0f);
  auto mix = [a](int dst, float src) {
    return std::clamp(static_cast<int>(dst * (1.0f - a) + src * 255.0f * a + 0.5f), 0, 255);
  };
  return RGB(mix(GetRValue(under), over.r), mix(GetGValue(under), over.g), mix(GetBValue(under), over.b));
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

IWICImagingFactory* WicFactory() {
  static Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  if (!factory) {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  }
  return factory.Get();
}

ID2D1Factory* D2dFactory() {
  static Microsoft::WRL::ComPtr<ID2D1Factory> factory;
  if (!factory) {
    D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory.GetAddressOf());
  }
  return factory.Get();
}

struct BgraImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;
};

bool BitmapToBgra(HBITMAP bmp, BgraImage& out) {
  BITMAP bm{};
  if (bmp == nullptr || GetObjectW(bmp, sizeof(bm), &bm) == 0 || bm.bmWidth <= 0 || bm.bmHeight == 0) {
    return false;
  }
  out.width = bm.bmWidth;
  out.height = std::abs(bm.bmHeight);
  out.pixels.assign(static_cast<size_t>(out.width) * static_cast<size_t>(out.height) * 4, 0);

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = out.width;
  bmi.bmiHeader.biHeight = -out.height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;

  const HDC dc = CreateCompatibleDC(nullptr);
  const int rows = GetDIBits(dc, bmp, 0, out.height, out.pixels.data(), &bmi, DIB_RGB_COLORS);
  DeleteDC(dc);
  return rows > 0;
}

void CropPaddedJumbo(BgraImage& image) {
  if (image.width < 8 || image.height < 8) {
    return;
  }
  auto alpha_at = [&](int x, int y) {
    return image.pixels[(static_cast<size_t>(y) * image.width + x) * 4 + 3];
  };
  int min_x = image.width;
  int min_y = image.height;
  int max_x = -1;
  int max_y = -1;
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      if (alpha_at(x, y) > 12) {
        min_x = (std::min)(min_x, x);
        min_y = (std::min)(min_y, y);
        max_x = (std::max)(max_x, x);
        max_y = (std::max)(max_y, y);
      }
    }
  }
  if (max_x < min_x) {
    return;
  }
  const int content_w = max_x - min_x + 1;
  const int content_h = max_y - min_y + 1;
  if (content_w >= image.width * 7 / 10 && content_h >= image.height * 7 / 10) {
    return;
  }
  min_x = (std::max)(0, min_x - 1);
  min_y = (std::max)(0, min_y - 1);
  max_x = (std::min)(image.width - 1, max_x + 1);
  max_y = (std::min)(image.height - 1, max_y + 1);
  const int new_w = max_x - min_x + 1;
  const int new_h = max_y - min_y + 1;
  std::vector<std::uint8_t> cropped(static_cast<size_t>(new_w) * static_cast<size_t>(new_h) * 4);
  for (int y = 0; y < new_h; ++y) {
    const std::uint8_t* src = image.pixels.data() + (static_cast<size_t>(min_y + y) * image.width + min_x) * 4;
    std::uint8_t* dst = cropped.data() + static_cast<size_t>(y) * new_w * 4;
    memcpy(dst, src, static_cast<size_t>(new_w) * 4);
  }
  image.width = new_w;
  image.height = new_h;
  image.pixels.swap(cropped);
}

void ZeroTransparentRgb(BgraImage& image) {
  const size_t n = image.pixels.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    if (image.pixels[i * 4 + 3] == 0) {
      image.pixels[i * 4 + 0] = 0;
      image.pixels[i * 4 + 1] = 0;
      image.pixels[i * 4 + 2] = 0;
    }
  }
}

bool HasStraightAlpha(const BgraImage& image) {
  const size_t n = image.pixels.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    const std::uint8_t b = image.pixels[i * 4 + 0];
    const std::uint8_t g = image.pixels[i * 4 + 1];
    const std::uint8_t r = image.pixels[i * 4 + 2];
    const std::uint8_t a = image.pixels[i * 4 + 3];
    if (r > a || g > a || b > a) {
      return true;
    }
  }
  return false;
}

void StraightToPremul(BgraImage& image) {
  const size_t n = image.pixels.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    const std::uint8_t a = image.pixels[i * 4 + 3];
    image.pixels[i * 4 + 0] = static_cast<std::uint8_t>((image.pixels[i * 4 + 0] * a + 127) / 255);
    image.pixels[i * 4 + 1] = static_cast<std::uint8_t>((image.pixels[i * 4 + 1] * a + 127) / 255);
    image.pixels[i * 4 + 2] = static_cast<std::uint8_t>((image.pixels[i * 4 + 2] * a + 127) / 255);
  }
}

void DefringePremul(BgraImage& image) {
  const int w = image.width;
  const int h = image.height;
  if (w < 3 || h < 3) {
    return;
  }
  const std::vector<std::uint8_t> src = image.pixels;
  auto sample = [&](int x, int y) -> const std::uint8_t* {
    return src.data() + (static_cast<size_t>(y) * w + x) * 4;
  };
  for (int y = 0; y < h; ++y) {
    for (int x = 0; x < w; ++x) {
      std::uint8_t* px = image.pixels.data() + (static_cast<size_t>(y) * w + x) * 4;
      const int a = px[3];
      if (a == 0 || a == 255) {
        continue;
      }
      const int ru = (px[2] * 255 + a / 2) / a;
      const int gu = (px[1] * 255 + a / 2) / a;
      const int bu = (px[0] * 255 + a / 2) / a;
      if (ru + gu + bu > 48) {
        continue;
      }
      int sr = 0;
      int sg = 0;
      int sb = 0;
      int n = 0;
      for (int ny = y - 2; ny <= y + 2; ++ny) {
        if (ny < 0 || ny >= h) {
          continue;
        }
        for (int nx = x - 2; nx <= x + 2; ++nx) {
          if (nx < 0 || nx >= w || (nx == x && ny == y)) {
            continue;
          }
          const std::uint8_t* nb = sample(nx, ny);
          const int na = nb[3];
          if (na < 200) {
            continue;
          }
          sr += (nb[2] * 255 + na / 2) / na;
          sg += (nb[1] * 255 + na / 2) / na;
          sb += (nb[0] * 255 + na / 2) / na;
          ++n;
        }
      }
      if (n == 0) {
        continue;
      }
      px[2] = static_cast<std::uint8_t>(((sr / n) * a + 127) / 255);
      px[1] = static_cast<std::uint8_t>(((sg / n) * a + 127) / 255);
      px[0] = static_cast<std::uint8_t>(((sb / n) * a + 127) / 255);
    }
  }
}

HBITMAP BgraToBitmap(const BgraImage& image, int px) {
  IWICImagingFactory* wic = WicFactory();
  if (wic == nullptr || image.width <= 0 || image.height <= 0 || px <= 0) {
    return nullptr;
  }

  Microsoft::WRL::ComPtr<IWICBitmap> source;
  if (FAILED(wic->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppPBGRA, image.width * 4,
                                         static_cast<UINT>(image.pixels.size()),
                                         const_cast<BYTE*>(image.pixels.data()), source.GetAddressOf()))) {
    return nullptr;
  }

  int fit_w = px;
  int fit_h = px;
  if (image.width != image.height) {
    if (image.width > image.height) {
      fit_h = (std::max)(1, image.height * px / image.width);
    } else {
      fit_w = (std::max)(1, image.width * px / image.height);
    }
  }

  Microsoft::WRL::ComPtr<IWICBitmapSource> scaled = source;
  if (fit_w != image.width || fit_h != image.height) {
    Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
    const bool down = fit_w < image.width || fit_h < image.height;
    const WICBitmapInterpolationMode mode =
        down ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeLinear;
    if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(source.Get(), fit_w, fit_h, mode))) {
      return nullptr;
    }
    scaled = scaler;
  }

  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(scaled.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
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
  HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr || bits == nullptr) {
    return nullptr;
  }

  std::vector<std::uint8_t> fit(static_cast<size_t>(fit_w) * static_cast<size_t>(fit_h) * 4);
  if (FAILED(converter->CopyPixels(nullptr, fit_w * 4, static_cast<UINT>(fit.size()), fit.data()))) {
    DeleteObject(dib);
    return nullptr;
  }

  auto* dest = static_cast<std::uint8_t*>(bits);
  const int ox = (px - fit_w) / 2;
  const int oy = (px - fit_h) / 2;
  for (int y = 0; y < fit_h; ++y) {
    memcpy(dest + (static_cast<size_t>(oy + y) * px + ox) * 4, fit.data() + static_cast<size_t>(y) * fit_w * 4,
           static_cast<size_t>(fit_w) * 4);
  }
  return dib;
}

HBITMAP FinalizeIconBitmap(HBITMAP source, int px, bool straight_alpha) {
  if (source == nullptr) {
    return nullptr;
  }
  BgraImage image;
  const bool ok = BitmapToBgra(source, image);
  DeleteObject(source);
  if (!ok) {
    return nullptr;
  }
  CropPaddedJumbo(image);
  ZeroTransparentRgb(image);
  if (straight_alpha || HasStraightAlpha(image)) {
    StraightToPremul(image);
  }
  DefringePremul(image);
  return BgraToBitmap(image, px);
}

HBITMAP BitmapFromIcon(HICON icon, int px) {
  if (icon == nullptr || px <= 0) {
    return nullptr;
  }
  ICONINFO info{};
  int native = px;
  if (GetIconInfo(icon, &info) != FALSE) {
    BITMAP bm{};
    if (info.hbmColor != nullptr && GetObjectW(info.hbmColor, sizeof(bm), &bm) != 0) {
      native = (std::max)(bm.bmWidth, std::abs(bm.bmHeight));
    }
    if (info.hbmColor != nullptr) {
      DeleteObject(info.hbmColor);
    }
    if (info.hbmMask != nullptr) {
      DeleteObject(info.hbmMask);
    }
  }
  if (native <= 0) {
    native = px;
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
  if (bmp == nullptr) {
    return nullptr;
  }
  const HDC dc = CreateCompatibleDC(nullptr);
  const HGDIOBJ old = SelectObject(dc, bmp);
  DrawIconEx(dc, 0, 0, icon, native, native, 0, nullptr, DI_NORMAL);
  SelectObject(dc, old);
  DeleteDC(dc);
  return FinalizeIconBitmap(bmp, px, true);
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

  if (HBITMAP shell = BitmapFromShellItem(path, 256)) {
    if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
      return ready;
    }
  }
  HICON icon = nullptr;
  const UINT got =
      PrivateExtractIconsW(path.c_str(), has_index ? index : 0, 256, 256, &icon, nullptr, 1, LR_DEFAULTCOLOR);
  if (got != 0 && icon != nullptr) {
    HBITMAP ready = BitmapFromIcon(icon, px);
    DestroyIcon(icon);
    return ready;
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

}  // namespace

struct DockMenuRow {
  UINT id = 0;
  std::wstring text;
  bool separator = false;
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
      rows_.push_back({id, std::move(title), false});
      window_targets_.push_back(hwnd);
    }

    const bool has_windows = !window_targets_.empty();
    const bool can_pin = app.pinned || (app.can_pin && !IsSelfExecutable(app.exe_path));
    auto add_sep = [&]() {
      if (!rows_.empty() && !rows_.back().separator) {
        rows_.push_back({0, L"", true});
      }
    };
    if (has_windows && can_pin) {
      add_sep();
    }
    if (app.pinned) {
      rows_.push_back({kUnpinCommand, L"고정 해제", false});
    } else if (app.can_pin && !IsSelfExecutable(app.exe_path)) {
      rows_.push_back({kPinCommand, L"독에 고정", false});
    }
    if (has_windows) {
      if (can_pin) {
        add_sep();
      }
      rows_.push_back({kShowAllCommand, L"모두 보기", false});
      rows_.push_back({kHideCommand, L"가리기", false});
      rows_.push_back({kQuitCommand, L"종료", false});
    }
    if (!rows_.empty() && rows_.back().separator) {
      rows_.pop_back();
    }
  }

  bool empty() const { return rows_.empty(); }
  size_t size() const { return rows_.size(); }
  int RowCount() const override { return static_cast<int>(rows_.size()); }

  SIZE Measure(UINT dpi) override {
    const int pad = DipToPx(kMenuPadDip, dpi);
    const int row_h = DipToPx(kMenuRowDip, dpi);
    const int sep_h = DipToPx(kMenuSepDip, dpi);
    const int text_pad = DipToPx(kMenuTextPadDip, dpi);
    int text_w = 0;
    for (const DockMenuRow& row : rows_) {
      if (row.separator || row.text.empty()) {
        continue;
      }
      text_w = (std::max)(text_w, static_cast<int>(PopupTextWidth(dpi, row.text) + 0.5f));
    }
    int width = text_w + pad * 2 + text_pad * 2;
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
        DrawPopupText(target, dpi, row.text,
                      D2D1::RectF(static_cast<float>(rc.left + text_pad), static_cast<float>(rc.top),
                                  static_cast<float>(rc.right - text_pad), static_cast<float>(rc.bottom)),
                      text.Get());
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
  TaskbarController::UnwatchTray();
  StopFullscreenWatch(hwnd_);
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

bool Dock::Create(HINSTANCE instance) {
  if (!RegisterClasses(instance)) {
    Log(L"dock", L"register classes failed");
    return false;
  }

  dark_ = ShellUsesDarkMode();
  pins_ = LoadDockPins();
  SanitizePins();
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
  StartFullscreenWatch(hwnd_, kFullscreenMsg);

  menu_content_ = std::make_unique<DockMenuContent>();
  if (!popup_.Create(instance, hwnd_)) {
    Log(L"dock", L"popup create failed err=%lu", GetLastError());
  }
  popup_.SetDark(dark_);
  RefreshFullscreen();
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
      return 0;
    }
    case WM_MOUSELEAVE:
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
        if (app.running && app.hwnd != nullptr) {
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
      TaskbarController::UnwatchTray();
      StopFullscreenWatch(hwnd_);
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
  if (!shown_) {
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
    Log(L"perf", L"rebuild skip fingerprint items=%zu %ums", items_.size(), fp_ms);
    return;
  }
  force_collect_ = false;
  last_window_fp_ = fp;
  std::vector<DockApp> next = CollectDockApps(pins_);
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
    Log(L"perf", L"rebuild skip items=%zu %ums", items_.size(),
        static_cast<unsigned>(GetTickCount64() - started));
    return;
  }
  last_collect_snap_ = snap;
  items_ = std::move(next);
  EnsureIcons();
  size_t kept = 0;
  for (size_t i = 0; i < items_.size(); ++i) {
    if (items_[i].pinned || (i < icons_.size() && icons_[i] != nullptr)) {
      if (kept != i) {
        items_[kept] = std::move(items_[i]);
      }
      ++kept;
    }
  }
  if (kept != items_.size()) {
    items_.resize(kept);
    EnsureIcons();
  }
  Log(L"perf", L"rebuild items=%zu pins=%zu shown=%d %ums", items_.size(), pins_.size(), shown_ ? 1 : 0,
      static_cast<unsigned>(GetTickCount64() - started));
  if (shown_) {
    if (items_.empty()) {
      HidePill();
    } else {
      Layout();
    }
  }
  InvalidateRect(hwnd_, nullptr, FALSE);
}

void Dock::ResetIconCache() {
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
  for (size_t i = 0; i < items_.size(); ++i) {
    live[items_[i].key] = true;
    const std::wstring key = items_[i].key + L"|" + std::to_wstring(px);
    auto it = icon_cache_.find(key);
    if (it == icon_cache_.end() || it->second == nullptr) {
      icon_cache_[key] = LoadIconBitmap(items_[i], px);
      it = icon_cache_.find(key);
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
      it = icon_cache_.erase(it);
    } else {
      ++it;
    }
  }
}

HBITMAP Dock::LoadIconBitmap(const DockApp& app, int px) {
  const bool identity = !app.aumid.empty() || PathImpliesGenericIcon(app.exe_path);

  if (!app.aumid.empty()) {
    if (HBITMAP shell = BitmapFromAumid(app.aumid, 256)) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        return ready;
      }
    }
  }
  if (!app.icon_resource.empty()) {
    if (HBITMAP ready = BitmapFromIconResource(app.icon_resource, px)) {
      return ready;
    }
  }
  if (identity && app.hwnd != nullptr) {
    if (HBITMAP ready = BitmapFromIcon(QueryWindowIcon(app.hwnd), px)) {
      return ready;
    }
  }
  if (!app.exe_path.empty() && !PathImpliesGenericIcon(app.exe_path)) {
    if (HBITMAP shell = BitmapFromShellItem(app.exe_path, 256)) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        return ready;
      }
    }
    HICON extracted = nullptr;
    const UINT got =
        PrivateExtractIconsW(app.exe_path.c_str(), 0, 256, 256, &extracted, nullptr, 1, LR_DEFAULTCOLOR);
    if (got != 0 && extracted != nullptr) {
      HBITMAP ready = BitmapFromIcon(extracted, px);
      DestroyIcon(extracted);
      if (ready != nullptr) {
        return ready;
      }
    }
    if (HBITMAP jumbo = BitmapFromJumboList(app.exe_path)) {
      if (HBITMAP ready = FinalizeIconBitmap(jumbo, px, true)) {
        return ready;
      }
    }
  }
  if (app.hwnd != nullptr) {
    if (HBITMAP ready = BitmapFromIcon(QueryWindowIcon(app.hwnd), px)) {
      return ready;
    }
  }
  if (!app.exe_path.empty()) {
    if (HBITMAP shell = BitmapFromShellItem(app.exe_path, 256)) {
      if (HBITMAP ready = FinalizeIconBitmap(shell, px, false)) {
        return ready;
      }
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

  ID2D1Factory* d2d = D2dFactory();
  IWICImagingFactory* wic = WicFactory();
  if (d2d == nullptr) {
    return;
  }

  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = width;
  bmi.bmiHeader.biHeight = -height;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  const HBITMAP dib = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (dib == nullptr) {
    return;
  }
  const HDC mem = CreateCompatibleDC(nullptr);
  const HGDIOBJ old = SelectObject(mem, dib);

  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> rt;
  const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
      D2D1_RENDER_TARGET_TYPE_DEFAULT,
      D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96.0f, 96.0f);
  if (FAILED(d2d->CreateDCRenderTarget(&props, rt.GetAddressOf())) ||
      FAILED(rt->BindDC(mem, &client))) {
    SelectObject(mem, old);
    DeleteDC(mem);
    DeleteObject(dib);
    return;
  }

  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_PER_PRIMITIVE);
  rt->BeginDraw();
  rt->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

  const float radius = static_cast<float>(Dip(kCornerRadiusDip));
  const D2D1_ROUNDED_RECT rounded{
      D2D1::RectF(0.5f, 0.5f, static_cast<float>(width) - 0.5f, static_cast<float>(height) - 0.5f), radius, radius};

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> indicator;
  rt->CreateSolidColorBrush(DockFillColor(dark_), fill.GetAddressOf());
  rt->CreateSolidColorBrush(DockStrokeColor(dark_), stroke.GetAddressOf());
  rt->CreateSolidColorBrush(DockIndicatorColor(dark_), indicator.GetAddressOf());
  if (fill) {
    rt->FillRoundedRectangle(rounded, fill.Get());
  }
  if (stroke) {
    rt->DrawRoundedRectangle(rounded, stroke.Get(), 1.0f);
  }

  const int icon_px = Dip(kIconDip);
  const auto order = DisplayOrder();
  const int pinned = PinnedCount();
  auto draw_icon = [&](size_t i, float x, float y, float alpha) {
    if (i >= icons_.size() || icons_[i] == nullptr || wic == nullptr) {
      return;
    }
    Microsoft::WRL::ComPtr<IWICBitmap> wic_bmp;
    if (FAILED(wic->CreateBitmapFromHBITMAP(icons_[i], nullptr, WICBitmapUsePremultipliedAlpha,
                                            wic_bmp.GetAddressOf()))) {
      return;
    }
    Microsoft::WRL::ComPtr<ID2D1Bitmap> d2d_bmp;
    if (FAILED(rt->CreateBitmapFromWicBitmap(wic_bmp.Get(), d2d_bmp.GetAddressOf()))) {
      return;
    }
    rt->DrawBitmap(d2d_bmp.Get(), D2D1::RectF(x, y, x + static_cast<float>(icon_px), y + static_cast<float>(icon_px)),
                   alpha, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
  };
  for (size_t slot_i = 0; slot_i < order.size() && slot_i < slots_.size(); ++slot_i) {
    const size_t i = order[slot_i];
    const RECT& slot = slots_[slot_i];
    const float x = static_cast<float>(slot.left + (slot.right - slot.left - icon_px) / 2);
    float y = static_cast<float>((height - icon_px) / 2 - Dip(4));
    if (!dragging_ && static_cast<int>(i) == pressed_) {
      y += static_cast<float>(Dip(1));
    }
    const bool is_dragged = dragging_ && static_cast<int>(i) == drag_index_;
    draw_icon(i, x, y, is_dragged ? 0.25f : 1.0f);
    if (items_[i].running && indicator) {
      const float dot_w = static_cast<float>(Dip(10));
      const float dot_h = static_cast<float>(Dip(3));
      const float dx = static_cast<float>(slot.left) + (static_cast<float>(slot.right - slot.left) - dot_w) * 0.5f;
      const float dy = static_cast<float>(height - Dip(10));
      const D2D1_ROUNDED_RECT dot{D2D1::RectF(dx, dy, dx + dot_w, dy + dot_h), dot_h * 0.5f, dot_h * 0.5f};
      rt->FillRoundedRectangle(dot, indicator.Get());
    }
  }

  if (dragging_ && drag_index_ >= 0 && drag_index_ < static_cast<int>(items_.size())) {
    const float half = static_cast<float>(icon_px) * 0.5f;
    float gx = static_cast<float>(drag_cursor_.x) - half;
    const float max_x = static_cast<float>((std::max)(0, width - icon_px));
    if (gx < 0.0f) {
      gx = 0.0f;
    } else if (gx > max_x) {
      gx = max_x;
    }
    const float gy = static_cast<float>((height - icon_px) / 2 - Dip(4) - Dip(3));
    draw_icon(static_cast<size_t>(drag_index_), gx, gy, 0.7f);
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
  rt.Reset();

  BLENDFUNCTION blend{};
  blend.BlendOp = AC_SRC_OVER;
  blend.SourceConstantAlpha = 255;
  blend.AlphaFormat = AC_SRC_ALPHA;
  POINT src{0, 0};
  SIZE size{width, height};
  UpdateLayeredWindow(hwnd_, nullptr, nullptr, &size, mem, &src, 0, &blend, ULW_ALPHA);

  SelectObject(mem, old);
  DeleteDC(mem);
  DeleteObject(dib);
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
  CancelHideTimer();
  if (popup_.IsOpen()) {
    popup_.Close();
  }
  if (dragging_) {
    EndDrag(false);
  }
  pressed_ = -1;
  if (!shown_) {
    UpdateIdleTimer();
    return;
  }
  shown_ = false;
  ShowWindow(hwnd_, SW_HIDE);
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
  if (hwnd_ == nullptr || !shown_) {
    return;
  }
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd_;
  TrackMouseEvent(&track);
}

void Dock::ArmHotMouseLeave() {
  if (hot_hwnd_ == nullptr) {
    return;
  }
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hot_hwnd_;
  TrackMouseEvent(&track);
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
  popup_.Close();
  CancelHideTimer();
  if (tooltip_ != nullptr) {
    SendMessageW(tooltip_, TTM_POP, 0, 0);
  }
  if (!menu_content_) {
    menu_content_ = std::make_unique<DockMenuContent>();
  }
  const DockApp& app = items_[static_cast<size_t>(index)];
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
  }

  if (pending_rebuild_) {
    ScheduleRebuild();
  }
  if (!PointerOverUi()) {
    StartHideTimer();
  }
}

void Dock::SanitizePins() {
  const auto before = pins_.size();
  pins_.erase(std::remove_if(pins_.begin(), pins_.end(),
                             [](const std::wstring& path) { return IsSelfExecutable(path); }),
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
  if (drag_index_ >= pinned || drop_index_ >= pinned) {
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
  drag_cursor_ = client;
  last_drag_render_ = 0;
  drag_render_n_ = 0;
  drag_render_total_us_ = 0;
  drag_render_max_us_ = 0;
  if (tooltip_ != nullptr) {
    SendMessageW(tooltip_, TTM_POP, 0, 0);
  }
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
  drag_cursor_ = client;
  const int next = DropIndexAt(client);
  const bool slot_changed = next >= 0 && next != drop_index_;
  if (slot_changed) {
    drop_index_ = next;
  }
  const ULONGLONG now = GetTickCount64();
  if (!slot_changed && last_drag_render_ != 0 && now - last_drag_render_ < 16) {
    return;
  }
  LARGE_INTEGER freq{};
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  RenderLayered();
  QueryPerformanceCounter(&t1);
  last_drag_render_ = GetTickCount64();
  const ULONGLONG us =
      freq.QuadPart == 0 ? 0 : static_cast<ULONGLONG>(((t1.QuadPart - t0.QuadPart) * 1000000) / freq.QuadPart);
  ++drag_render_n_;
  drag_render_total_us_ += us;
  if (us > drag_render_max_us_) {
    drag_render_max_us_ = static_cast<UINT>(us);
  }
}

void Dock::EndDrag(bool commit) {
  const bool was_dragging = dragging_;
  const int from = drag_index_;
  const int to = drop_index_;
  if (was_dragging) {
    const double avg_ms =
        drag_render_n_ == 0 ? 0.0 : static_cast<double>(drag_render_total_us_) / static_cast<double>(drag_render_n_) / 1000.0;
    const unsigned max_ms = (drag_render_max_us_ + 500) / 1000;
    Log(L"perf", L"drag render n=%u avg=%.1fms max=%ums", drag_render_n_, avg_ms, max_ms);
  }
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
      SaveDockPins(pins_);
      pending_rebuild_ = true;
      force_collect_ = true;
    }
  }
  if (pending_rebuild_) {
    ScheduleRebuild();
  } else if (shown_) {
    RenderLayered();
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
