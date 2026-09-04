#pragma once

#include "status_item.hpp"

#include <d2d1.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <cstdint>
#include <list>
#include <unordered_map>
#include <vector>

namespace bamti {

IWICImagingFactory* WicFactory();

struct BgraImage {
  int width = 0;
  int height = 0;
  std::vector<std::uint8_t> pixels;
};

bool BitmapToBgra(HBITMAP bmp, BgraImage& out);
void CropPaddedJumbo(BgraImage& image);
void ZeroTransparentRgb(BgraImage& image);
bool HasStraightAlpha(const BgraImage& image);
void StraightToPremul(BgraImage& image);
void PremulToStraight(BgraImage& image);
void DefringePremul(BgraImage& image);
HBITMAP BgraToBitmap(const BgraImage& image, int px);
HBITMAP FinalizeIconBitmap(HBITMAP source, int px, bool straight_alpha, bool trim_padding = false);
HBITMAP BitmapFromIcon(HICON icon, int px);

class IconCache {
 public:
  void SetRenderTarget(ID2D1RenderTarget* rt);
  void SetDark(bool dark);
  ID2D1Bitmap* Get(const StatusIcon& icon, int px);
  void Clear();

 private:
  struct Key {
    uint64_t cache_key = 0;
    int px = 0;
    bool operator==(const Key& other) const { return cache_key == other.cache_key && px == other.px; }
  };
  struct KeyHash {
    size_t operator()(const Key& key) const {
      return static_cast<size_t>(key.cache_key ^ (static_cast<uint64_t>(static_cast<uint32_t>(key.px)) << 32));
    }
  };
  struct Slot {
    Microsoft::WRL::ComPtr<ID2D1Bitmap> bitmap;
    std::list<Key>::iterator order;
  };

  Microsoft::WRL::ComPtr<ID2D1Bitmap> Decode(const StatusIcon& icon, int px);
  Microsoft::WRL::ComPtr<ID2D1Bitmap> BitmapFromWic(IWICBitmapSource* source, int px);
  Microsoft::WRL::ComPtr<ID2D1Bitmap> BitmapFromHbitmap(HBITMAP bmp, int px);

  ID2D1RenderTarget* rt_ = nullptr;
  bool dark_ = true;
  std::list<Key> lru_;
  std::unordered_map<Key, Slot, KeyHash> map_;
};

}  // namespace bamti
