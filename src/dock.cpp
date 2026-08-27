#include "dock.hpp"

#include "dwm.hpp"
#include "fullscreen.hpp"
#include "theme.hpp"

#include <commctrl.h>
#include <commoncontrols.h>
#include <d2d1.h>
#include <d2d1helper.h>
#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <wincodec.h>
#include <windowsx.h>
#include <wrl/client.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <vector>

namespace bamti {
namespace {

constexpr int kIconDip = 36;
constexpr int kSlotDip = 52;
constexpr int kHeightDip = 64;
constexpr int kPadXDip = 18;
constexpr int kMarginBottomDip = 8;
constexpr int kHotDip = 8;
// macOS Dock is ~20pt at the default bar height (~64pt). DWM ROUND/ROUNDSMALL cannot
// express that, so the pill is drawn with Direct2D.
constexpr int kCornerRadiusDip = 20;
constexpr UINT kHideDelayMs = 100;
constexpr UINT_PTR kHideTimerId = 1;
constexpr UINT_PTR kPollTimerId = 2;
constexpr UINT kTasksChangedMsg = WM_APP + 20;
constexpr UINT kPinCommand = 1;
constexpr UINT kUnpinCommand = 2;
constexpr UINT kCloseCommand = 3;

HWND g_notify = nullptr;
std::atomic<bool> g_rebuild_posted{false};

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
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
  auto as_icon = [](LRESULT value) { return reinterpret_cast<HICON>(value); };
  HICON icon = as_icon(SendMessageW(hwnd, WM_GETICON, ICON_BIG, 0));
  if (icon == nullptr) {
    icon = as_icon(SendMessageW(hwnd, WM_GETICON, ICON_SMALL2, 0));
  }
  if (icon == nullptr) {
    icon = as_icon(GetClassLongPtrW(hwnd, GCLP_HICON));
  }
  if (icon == nullptr) {
    icon = as_icon(GetClassLongPtrW(hwnd, GCLP_HICONSM));
  }
  return icon;
}

}  // namespace

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
    return false;
  }

  dark_ = ShellUsesDarkMode();
  pins_ = LoadDockPins();

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST | WS_EX_LAYERED, kDockClass, L"bamti dock", WS_POPUP, 0, 0, 0,
                          0, nullptr, nullptr, instance, this);
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

  const DWORD hook_flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS;
  const DWORD ranges[][2] = {
      {EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND},
      {EVENT_SYSTEM_MINIMIZESTART, EVENT_SYSTEM_MINIMIZEEND},
      {EVENT_OBJECT_CREATE, EVENT_OBJECT_HIDE},
      {EVENT_OBJECT_NAMECHANGE, EVENT_OBJECT_NAMECHANGE},
      {EVENT_OBJECT_CLOAKED, EVENT_OBJECT_UNCLOAKED},
  };
  for (const auto& range : ranges) {
    HWINEVENTHOOK hook = SetWinEventHook(range[0], range[1], nullptr, WinEventProc, 0, 0, hook_flags);
    if (hook != nullptr) {
      hooks_.push_back(hook);
    }
  }

  SetTimer(hwnd_, kPollTimerId, 50, nullptr);
  RefreshFullscreen();
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

void CALLBACK Dock::WinEventProc(HWINEVENTHOOK, DWORD, HWND, LONG object, LONG, DWORD, DWORD) {
  if (object != OBJID_WINDOW) {
    return;
  }
  if (g_notify == nullptr) {
    return;
  }
  if (!g_rebuild_posted.exchange(true)) {
    PostMessageW(g_notify, kTasksChangedMsg, 0, 0);
  }
}

LRESULT Dock::HandleHot(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) {
  switch (msg) {
    case WM_MOUSEMOVE:
    case WM_NCMOUSEMOVE:
      if (!fullscreen_occluded_) {
        ShowPill();
      }
      return 0;
    default:
      return DefWindowProcW(hwnd, msg, wparam, lparam);
  }
}

LRESULT Dock::HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam) {
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
        if (!PointerOverUi() && !menu_open_) {
          HidePill();
        }
      }
      return 0;
    case kTasksChangedMsg:
      g_rebuild_posted = false;
      Rebuild();
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
        RenderLayered();
      }
      return 0;
    }
    case WM_MOUSEMOVE:
      CancelHideTimer();
      ArmMouseLeave();
      return 0;
    case WM_MOUSELEAVE:
      StartHideTimer();
      return 0;
    case WM_LBUTTONDOWN: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      pressed_ = HitTest(pt);
      if (pressed_ >= 0) {
        SetCapture(hwnd_);
        RenderLayered();
      }
      return 0;
    }
    case WM_LBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int index = HitTest(pt);
      const int pressed = pressed_;
      pressed_ = -1;
      ReleaseCapture();
      RenderLayered();
      if (index >= 0 && index == pressed && index < static_cast<int>(items_.size())) {
        const DockApp& app = items_[static_cast<size_t>(index)];
        if (app.running && app.hwnd != nullptr) {
          ActivateHwnd(app.hwnd);
        } else {
          LaunchExe(app.exe_path);
        }
      }
      return 0;
    }
    case WM_RBUTTONUP: {
      const POINT pt{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
      const int index = HitTest(pt);
      if (index >= 0) {
        POINT screen = pt;
        ClientToScreen(hwnd_, &screen);
        ShowContextMenu(screen, index);
      }
      return 0;
    }
    case WM_COMMAND: {
      if (context_index_ < 0 || context_index_ >= static_cast<int>(items_.size())) {
        return 0;
      }
      DockApp& app = items_[static_cast<size_t>(context_index_)];
      switch (LOWORD(wparam)) {
        case kPinCommand:
          if (app.can_pin && !app.exe_path.empty()) {
            const std::wstring canon = CanonicalPath(app.exe_path);
            const bool exists =
                std::any_of(pins_.begin(), pins_.end(),
                            [&](const std::wstring& path) { return CanonicalPath(path) == canon; });
            if (!exists) {
              pins_.push_back(app.exe_path);
              SaveDockPins(pins_);
            }
            Rebuild();
          }
          break;
        case kUnpinCommand: {
          const std::wstring canon = CanonicalPath(app.exe_path);
          pins_.erase(std::remove_if(pins_.begin(), pins_.end(),
                                     [&](const std::wstring& path) { return CanonicalPath(path) == canon; }),
                      pins_.end());
          SaveDockPins(pins_);
          Rebuild();
          break;
        }
        case kCloseCommand:
          CloseHwnds(app.windows);
          break;
        default:
          break;
      }
      context_index_ = -1;
      return 0;
    }
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
  items_ = CollectDockApps(pins_);
  EnsureIcons();
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
  std::map<std::wstring, HBITMAP> keep;
  icons_.assign(items_.size(), nullptr);
  for (size_t i = 0; i < items_.size(); ++i) {
    const std::wstring key = items_[i].key + L"#" + std::to_wstring(px);
    auto it = icon_cache_.find(key);
    if (it == icon_cache_.end() || it->second == nullptr) {
      icon_cache_[key] = LoadIconBitmap(items_[i], px);
      it = icon_cache_.find(key);
    }
    icons_[i] = it->second;
    keep[key] = it->second;
  }
  for (auto& [key, bmp] : icon_cache_) {
    if (keep.find(key) == keep.end() && bmp != nullptr) {
      DeleteObject(bmp);
    }
  }
  icon_cache_.swap(keep);
}

HBITMAP Dock::LoadIconBitmap(const DockApp& app, int px) {
  if (!app.exe_path.empty()) {
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
  return BitmapFromIcon(QueryWindowIcon(app.hwnd), px);
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
  const int width = pad * 2 + (count > 0 ? count * slot : slot);
  const int x = info.rcMonitor.left + (info.rcMonitor.right - info.rcMonitor.left - width) / 2;
  const int y = info.rcMonitor.bottom - Dip(kMarginBottomDip) - height;
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE);

  slots_.assign(static_cast<size_t>(count), RECT{});
  for (int i = 0; i < count; ++i) {
    RECT slot_rect{};
    slot_rect.left = pad + i * slot;
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
  for (size_t i = 0; i < items_.size() && i < slots_.size(); ++i) {
    const RECT& slot = slots_[i];
    const float x = static_cast<float>(slot.left + (slot.right - slot.left - icon_px) / 2);
    float y = static_cast<float>((height - icon_px) / 2 - Dip(4));
    if (static_cast<int>(i) == pressed_) {
      y += static_cast<float>(Dip(1));
    }
    if (i < icons_.size() && icons_[i] != nullptr && wic != nullptr) {
      Microsoft::WRL::ComPtr<IWICBitmap> wic_bmp;
      if (SUCCEEDED(wic->CreateBitmapFromHBITMAP(icons_[i], nullptr, WICBitmapUsePremultipliedAlpha,
                                                 wic_bmp.GetAddressOf()))) {
        Microsoft::WRL::ComPtr<ID2D1Bitmap> d2d_bmp;
        if (SUCCEEDED(rt->CreateBitmapFromWicBitmap(wic_bmp.Get(), d2d_bmp.GetAddressOf()))) {
          const float opacity = items_[i].running ? 1.0f : 0.65f;
          rt->DrawBitmap(d2d_bmp.Get(), D2D1::RectF(x, y, x + static_cast<float>(icon_px), y + static_cast<float>(icon_px)),
                         opacity, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        }
      }
    }
    if (items_[i].running && indicator) {
      const float dot_w = static_cast<float>(Dip(10));
      const float dot_h = static_cast<float>(Dip(3));
      const float dx = static_cast<float>(slot.left) + (static_cast<float>(slot.right - slot.left) - dot_w) * 0.5f;
      const float dy = static_cast<float>(height - Dip(10));
      const D2D1_ROUNDED_RECT dot{D2D1::RectF(dx, dy, dx + dot_w, dy + dot_h), dot_h * 0.5f, dot_h * 0.5f};
      rt->FillRoundedRectangle(dot, indicator.Get());
    }
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
  if (fullscreen_occluded_ || items_.empty()) {
    return;
  }
  CancelHideTimer();
  if (!shown_) {
    shown_ = true;
    Layout();
    ShowWindow(hwnd_, SW_SHOWNA);
    SetWindowPos(hwnd_, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
  }
  ArmMouseLeave();
}

void Dock::HidePill() {
  CancelHideTimer();
  pressed_ = -1;
  if (!shown_) {
    return;
  }
  shown_ = false;
  ShowWindow(hwnd_, SW_HIDE);
}

void Dock::StartHideTimer() {
  if (!shown_ || menu_open_ || hide_armed_) {
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

void Dock::PollPointer() {
  RefreshFullscreen();
  if (fullscreen_occluded_) {
    return;
  }
  if (PointerOverUi()) {
    CancelHideTimer();
    if (PointerOverHotEdge() || shown_) {
      ShowPill();
    }
  } else if (shown_ && !menu_open_) {
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
  }
}

void Dock::ShowContextMenu(POINT screen, int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    return;
  }
  const HMENU menu = CreatePopupMenu();
  if (menu == nullptr) {
    return;
  }
  menu_open_ = true;
  CancelHideTimer();
  context_index_ = index;
  const DockApp& app = items_[static_cast<size_t>(index)];
  if (app.pinned) {
    AppendMenuW(menu, MF_STRING, kUnpinCommand, L"고정 해제");
  } else if (app.can_pin) {
    AppendMenuW(menu, MF_STRING, kPinCommand, L"독에 고정");
  }
  if (app.running) {
    if (GetMenuItemCount(menu) > 0) {
      AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    }
    AppendMenuW(menu, MF_STRING, kCloseCommand, L"닫기");
  }
  if (GetMenuItemCount(menu) > 0) {
    TrackPopupMenuEx(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_RIGHTALIGN, screen.x, screen.y, hwnd_, nullptr);
  }
  DestroyMenu(menu);
  menu_open_ = false;
  if (!PointerOverUi()) {
    StartHideTimer();
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
