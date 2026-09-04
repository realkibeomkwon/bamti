#include "icon_cache.hpp"

#include "log.hpp"
#include "theme.hpp"

#include <algorithm>
#include <cstring>
#include <d2d1helper.h>

namespace bamti {
namespace {

constexpr size_t kIconCacheMax = 64;
constexpr UINT kPngMaxEdge = 64;

Microsoft::WRL::ComPtr<ID2D1Bitmap> ScaleWicToBitmap(ID2D1RenderTarget* rt, IWICImagingFactory* wic,
                                                     IWICBitmapSource* source, int px) {
  Microsoft::WRL::ComPtr<ID2D1Bitmap> out;
  if (rt == nullptr || wic == nullptr || source == nullptr || px <= 0) {
    return out;
  }
  UINT src_w = 0;
  UINT src_h = 0;
  if (FAILED(source->GetSize(&src_w, &src_h)) || src_w == 0 || src_h == 0) {
    return out;
  }
  int fit_w = px;
  int fit_h = px;
  if (src_w != src_h) {
    if (src_w > src_h) {
      fit_h = (std::max)(1, static_cast<int>(src_h) * px / static_cast<int>(src_w));
    } else {
      fit_w = (std::max)(1, static_cast<int>(src_w) * px / static_cast<int>(src_h));
    }
  }
  Microsoft::WRL::ComPtr<IWICBitmapSource> scaled = source;
  Microsoft::WRL::ComPtr<IWICBitmapScaler> scaler;
  if (fit_w != static_cast<int>(src_w) || fit_h != static_cast<int>(src_h)) {
    const bool down = fit_w < static_cast<int>(src_w) || fit_h < static_cast<int>(src_h);
    const WICBitmapInterpolationMode mode =
        down ? WICBitmapInterpolationModeFant : WICBitmapInterpolationModeLinear;
    if (FAILED(wic->CreateBitmapScaler(scaler.GetAddressOf())) ||
        FAILED(scaler->Initialize(source, static_cast<UINT>(fit_w), static_cast<UINT>(fit_h), mode))) {
      return out;
    }
    scaled = scaler;
  }
  Microsoft::WRL::ComPtr<IWICFormatConverter> converter;
  if (FAILED(wic->CreateFormatConverter(converter.GetAddressOf())) ||
      FAILED(converter->Initialize(scaled.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                   WICBitmapPaletteTypeCustom))) {
    return out;
  }
  if (FAILED(rt->CreateBitmapFromWicBitmap(converter.Get(), out.GetAddressOf()))) {
    out.Reset();
  }
  return out;
}

struct MonoStats {
  int ink = 0;
  int bright = 0;
  int colorful = 0;
  int gray = 0;
  double mean_lum = 0;
  double mean_sat = 0;
  double gray_mean_lum = 0;
};

MonoStats MeasureMono(const BgraImage& image) {
  MonoStats s;
  double lum_w = 0;
  double sat_w = 0;
  double a_sum = 0;
  double gray_lum_w = 0;
  double gray_a_sum = 0;
  const size_t n = image.pixels.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    const int b = image.pixels[i * 4 + 0];
    const int g = image.pixels[i * 4 + 1];
    const int r = image.pixels[i * 4 + 2];
    const int a = image.pixels[i * 4 + 3];
    if (a <= 0) {
      continue;
    }
    const int mx = (std::max)(r, (std::max)(g, b));
    const int mn = (std::min)(r, (std::min)(g, b));
    const int sat = mx - mn;
    lum_w += static_cast<double>(mx) * a;
    sat_w += static_cast<double>(sat) * a;
    a_sum += a;
    if (a < 160) {
      continue;
    }
    ++s.ink;
    if (sat > 28) {
      ++s.colorful;
    } else {
      ++s.gray;
      gray_lum_w += static_cast<double>(mx) * a;
      gray_a_sum += a;
      if (mn >= 160) {
        ++s.bright;
      }
    }
  }
  if (a_sum > 0) {
    s.mean_lum = lum_w / a_sum;
    s.mean_sat = sat_w / a_sum;
  }
  if (gray_a_sum > 0) {
    s.gray_mean_lum = gray_lum_w / gray_a_sum;
  }
  return s;
}

bool IsBrightMonochrome(const MonoStats& s) {
  // 불투명 회색 화소가 충분히 밝고 전체의 7할 이상이면 단색 아이콘으로 본다.
  // 주황 배지 같은 채색 화소가 조금 있어도 포기하지 않는다.
  if (s.gray < 8) {
    return false;
  }
  if (s.gray_mean_lum < 200.0) {
    return false;
  }
  if (s.gray * 100 < s.ink * 70) {
    return false;
  }
  return true;
}

void RecolorGrayKeepAlpha(BgraImage& image, D2D1_COLOR_F color) {
  const std::uint8_t cr = static_cast<std::uint8_t>(color.r * 255.0f + 0.5f);
  const std::uint8_t cg = static_cast<std::uint8_t>(color.g * 255.0f + 0.5f);
  const std::uint8_t cb = static_cast<std::uint8_t>(color.b * 255.0f + 0.5f);
  const size_t n = image.pixels.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    if (image.pixels[i * 4 + 3] == 0) {
      continue;
    }
    const int b = image.pixels[i * 4 + 0];
    const int g = image.pixels[i * 4 + 1];
    const int r = image.pixels[i * 4 + 2];
    const int mx = (std::max)(r, (std::max)(g, b));
    const int mn = (std::min)(r, (std::min)(g, b));
    if (mx - mn > 28) {
      continue;
    }
    image.pixels[i * 4 + 0] = cb;
    image.pixels[i * 4 + 1] = cg;
    image.pixels[i * 4 + 2] = cr;
  }
}

struct InkBounds {
  int min_x = 0;
  int min_y = 0;
  int max_x = -1;
  int max_y = -1;
  int ink = 0;

  bool ok() const { return max_x >= min_x; }
  int width() const { return ok() ? max_x - min_x + 1 : 0; }
  int height() const { return ok() ? max_y - min_y + 1 : 0; }
};

InkBounds FindInkBounds(const BgraImage& image, int alpha_min) {
  InkBounds b;
  b.min_x = image.width;
  b.min_y = image.height;
  for (int y = 0; y < image.height; ++y) {
    for (int x = 0; x < image.width; ++x) {
      if (image.pixels[(static_cast<size_t>(y) * image.width + x) * 4 + 3] > alpha_min) {
        b.min_x = (std::min)(b.min_x, x);
        b.min_y = (std::min)(b.min_y, y);
        b.max_x = (std::max)(b.max_x, x);
        b.max_y = (std::max)(b.max_y, y);
        ++b.ink;
      }
    }
  }
  return b;
}

bool FillsMostOfCanvas(const InkBounds& b, int width, int height) {
  return b.width() >= width * 7 / 10 && b.height() >= height * 7 / 10;
}

void CropImageToBounds(BgraImage& image, InkBounds b) {
  b.min_x = (std::max)(0, b.min_x - 1);
  b.min_y = (std::max)(0, b.min_y - 1);
  b.max_x = (std::min)(image.width - 1, b.max_x + 1);
  b.max_y = (std::min)(image.height - 1, b.max_y + 1);
  const int new_w = b.max_x - b.min_x + 1;
  const int new_h = b.max_y - b.min_y + 1;
  if (new_w == image.width && new_h == image.height && b.min_x == 0 && b.min_y == 0) {
    return;
  }
  std::vector<std::uint8_t> cropped(static_cast<size_t>(new_w) * static_cast<size_t>(new_h) * 4);
  for (int y = 0; y < new_h; ++y) {
    const std::uint8_t* src = image.pixels.data() + (static_cast<size_t>(b.min_y + y) * image.width + b.min_x) * 4;
    std::uint8_t* dst = cropped.data() + static_cast<size_t>(y) * new_w * 4;
    memcpy(dst, src, static_cast<size_t>(new_w) * 4);
  }
  image.width = new_w;
  image.height = new_h;
  image.pixels.swap(cropped);
}

}  // namespace

IWICImagingFactory* WicFactory() {
  static Microsoft::WRL::ComPtr<IWICImagingFactory> factory;
  if (!factory) {
    CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
  }
  return factory.Get();
}

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
  const InkBounds loose = FindInkBounds(image, 12);
  if (!loose.ok()) {
    return;
  }
  // IShellItemImageFactory 점보 비트맵은 32/48px 아이콘을 256 캔버스 중앙에 두고
  // 가장자리에 희미한 테두리를 남기는 경우가 있다. alpha>12 경계만 보면 캔버스
  // 전체가 내용으로 보여 크롭이 실패하므로, 불투명 잉크(alpha>128)도 본다.
  const InkBounds tight = FindInkBounds(image, 128);
  const bool loose_fills = FillsMostOfCanvas(loose, image.width, image.height);
  const int loose_area = loose.width() * loose.height();
  const int loose_fill_pct = loose_area > 0 ? loose.ink * 100 / loose_area : 0;
  const bool framed_jumbo = image.width >= 64 && image.height >= 64 && loose_fills && loose_fill_pct < 35 &&
                            tight.ok() && tight.width() >= 8 && tight.height() >= 8 &&
                            !FillsMostOfCanvas(tight, image.width, image.height);
  if (framed_jumbo) {
    CropImageToBounds(image, tight);
    return;
  }
  if (loose_fills) {
    return;
  }
  CropImageToBounds(image, loose);
}

// 앱 자산이 규격대로 남겨 둔 투명 여백을 걷어내서, 그림이 아이콘 칸을 이웃과 같은 정도로
// 채우게 한다. 여백이 아니라 작은 그림을 크게 늘리는 일이 없도록, 이미 캔버스의 절반 이상을
// 채우고 있을 때에만 자른다.
void TrimTransparentBorder(BgraImage& image) {
  if (image.width < 8 || image.height < 8) {
    return;
  }
  const InkBounds ink = FindInkBounds(image, 12);
  if (!ink.ok()) {
    return;
  }
  if (ink.width() < image.width / 2 || ink.height() < image.height / 2) {
    return;
  }
  CropImageToBounds(image, ink);
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

void PremulToStraight(BgraImage& image) {
  auto unpremul = [](int c, int a) -> std::uint8_t {
    const int v = (c * 255 + a / 2) / a;
    return static_cast<std::uint8_t>(v > 255 ? 255 : v);
  };
  const size_t n = image.pixels.size() / 4;
  for (size_t i = 0; i < n; ++i) {
    const int a = image.pixels[i * 4 + 3];
    if (a == 0) {
      image.pixels[i * 4 + 0] = 0;
      image.pixels[i * 4 + 1] = 0;
      image.pixels[i * 4 + 2] = 0;
      continue;
    }
    if (a == 255) {
      continue;
    }
    image.pixels[i * 4 + 0] = unpremul(image.pixels[i * 4 + 0], a);
    image.pixels[i * 4 + 1] = unpremul(image.pixels[i * 4 + 1], a);
    image.pixels[i * 4 + 2] = unpremul(image.pixels[i * 4 + 2], a);
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

HBITMAP FinalizeIconBitmap(HBITMAP source, int px, bool straight_alpha, bool trim_padding) {
  if (source == nullptr) {
    return nullptr;
  }
  BgraImage image;
  const bool ok = BitmapToBgra(source, image);
  DeleteObject(source);
  if (!ok) {
    return nullptr;
  }
  if (trim_padding) {
    TrimTransparentBorder(image);
  } else {
    CropPaddedJumbo(image);
  }
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
  return FinalizeIconBitmap(bmp, px, false);
}

void IconCache::SetRenderTarget(ID2D1RenderTarget* rt) {
  if (rt_ == rt) {
    return;
  }
  Clear();
  rt_ = rt;
}

void IconCache::SetDark(bool dark) {
  if (dark_ == dark) {
    return;
  }
  dark_ = dark;
  Clear();
}

void IconCache::Clear() {
  map_.clear();
  lru_.clear();
}

ID2D1Bitmap* IconCache::Get(const StatusIcon& icon, int px) {
  if (rt_ == nullptr || px <= 0) {
    return nullptr;
  }
  if (icon.kind != IconKind::kPng && icon.kind != IconKind::kFile && icon.kind != IconKind::kHicon) {
    return nullptr;
  }
  const Key key{icon.cache_key, px};
  if (const auto it = map_.find(key); it != map_.end() && it->second.bitmap) {
    lru_.splice(lru_.begin(), lru_, it->second.order);
    it->second.order = lru_.begin();
    return it->second.bitmap.Get();
  }

  Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap = Decode(icon, px);
  if (!bitmap) {
    return nullptr;
  }
  Log(L"icon", L"decode key=%llu px=%d kind=%d", static_cast<unsigned long long>(icon.cache_key), px,
      static_cast<int>(icon.kind));
  if (map_.size() >= kIconCacheMax && !lru_.empty()) {
    const Key evict = lru_.back();
    map_.erase(evict);
    lru_.pop_back();
  }
  lru_.push_front(key);
  Slot slot;
  slot.bitmap = std::move(bitmap);
  slot.order = lru_.begin();
  ID2D1Bitmap* raw = slot.bitmap.Get();
  map_[key] = std::move(slot);
  return raw;
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> IconCache::BitmapFromWic(IWICBitmapSource* source, int px) {
  return ScaleWicToBitmap(rt_, WicFactory(), source, px);
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> IconCache::BitmapFromHbitmap(HBITMAP bmp, int px) {
  Microsoft::WRL::ComPtr<ID2D1Bitmap> out;
  IWICImagingFactory* wic = WicFactory();
  if (wic == nullptr || bmp == nullptr || rt_ == nullptr) {
    return out;
  }
  Microsoft::WRL::ComPtr<IWICBitmap> wic_bmp;
  if (FAILED(wic->CreateBitmapFromHBITMAP(bmp, nullptr, WICBitmapUsePremultipliedAlpha, wic_bmp.GetAddressOf()))) {
    return out;
  }
  return BitmapFromWic(wic_bmp.Get(), px);
}

Microsoft::WRL::ComPtr<ID2D1Bitmap> IconCache::Decode(const StatusIcon& icon, int px) {
  Microsoft::WRL::ComPtr<ID2D1Bitmap> out;
  IWICImagingFactory* wic = WicFactory();
  if (wic == nullptr) {
    return out;
  }
  if (icon.kind == IconKind::kPng) {
    if (icon.bytes.empty() || icon.bytes.size() > kStatusIconPngMaxBytes) {
      Log(L"icon", L"png rejected size=%zu", icon.bytes.size());
      return out;
    }
    Microsoft::WRL::ComPtr<IWICStream> stream;
    if (FAILED(wic->CreateStream(stream.GetAddressOf())) ||
        FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(icon.bytes.data()),
                                            static_cast<DWORD>(icon.bytes.size())))) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnLoad,
                                            decoder.GetAddressOf()))) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    UINT w = 0;
    UINT h = 0;
    if (FAILED(frame->GetSize(&w, &h)) || w == 0 || h == 0) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    if (w > kPngMaxEdge || h > kPngMaxEdge) {
      Log(L"icon", L"png too large %ux%u", w, h);
      return out;
    }
    Microsoft::WRL::ComPtr<IWICFormatConverter> bgra;
    if (FAILED(wic->CreateFormatConverter(bgra.GetAddressOf())) ||
        FAILED(bgra->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                                WICBitmapPaletteTypeCustom))) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    BgraImage image;
    image.width = static_cast<int>(w);
    image.height = static_cast<int>(h);
    image.pixels.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4);
    if (FAILED(bgra->CopyPixels(nullptr, w * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data()))) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    const MonoStats mono = MeasureMono(image);
    const bool recolor = IsBrightMonochrome(mono);
    Log(L"icon",
        L"mono key=%llu ink=%d bright=%d colorful=%d gray=%d mean_lum=%.0f mean_sat=%.0f gray_lum=%.0f result=%d",
        static_cast<unsigned long long>(icon.cache_key), mono.ink, mono.bright, mono.colorful, mono.gray, mono.mean_lum,
        mono.mean_sat, mono.gray_mean_lum, recolor ? 1 : 0);
    if (recolor) {
      RecolorGrayKeepAlpha(image, ClockTextColor(dark_));
    }
    Microsoft::WRL::ComPtr<IWICBitmap> mem;
    if (FAILED(wic->CreateBitmapFromMemory(w, h, GUID_WICPixelFormat32bppBGRA, w * 4,
                                           static_cast<UINT>(image.pixels.size()), image.pixels.data(),
                                           mem.GetAddressOf()))) {
      Log(L"icon", L"png decode failed");
      return out;
    }
    return BitmapFromWic(mem.Get(), px);
  }
  if (icon.kind == IconKind::kFile) {
    if (icon.path.empty()) {
      return out;
    }
    Microsoft::WRL::ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(wic->CreateDecoderFromFilename(icon.path.c_str(), nullptr, GENERIC_READ,
                                              WICDecodeMetadataCacheOnLoad, decoder.GetAddressOf()))) {
      Log(L"icon", L"file decode failed %s", icon.path.c_str());
      return out;
    }
    Microsoft::WRL::ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.GetAddressOf()))) {
      Log(L"icon", L"file decode failed %s", icon.path.c_str());
      return out;
    }
    return BitmapFromWic(frame.Get(), px);
  }
  if (icon.kind == IconKind::kHicon) {
    if (icon.hicon == nullptr) {
      return out;
    }
    HBITMAP bmp = BitmapFromIcon(icon.hicon, px);
    if (bmp == nullptr) {
      Log(L"icon", L"hicon decode failed");
      return out;
    }
    out = BitmapFromHbitmap(bmp, px);
    DeleteObject(bmp);
    return out;
  }
  return out;
}

}  // namespace bamti
