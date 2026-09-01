#pragma once

#include "status_item.hpp"

#include <dwrite.h>
#include <wrl/client.h>

#include <cstdint>
#include <string>
#include <vector>

namespace bamti {

enum class SegmentKind { kStart, kSpotlight, kWarning, kStatus, kOverflow, kControlCenter, kClock };

struct BarSegment {
  SegmentKind kind = SegmentKind::kStatus;
  std::string id;
  std::wstring text;
  std::wstring tooltip;
  uint32_t accent = 0;
  IconKind icon_kind = IconKind::kNone;
  uint64_t icon_key = 0;
  StatusIcon icon;
  RECT rect{};
};

struct BarLayoutResult {
  std::vector<BarSegment> segments;
  std::vector<StatusItem> overflow;
  UINT dpi = 96;
  RECT client{};
};

class BarLayout {
 public:
  bool Initialize();
  void SetDpi(UINT dpi);

  const BarLayoutResult& Compute(const RECT& client, const std::wstring& clock_text,
                                 const std::wstring& warning_text, const std::vector<StatusItem>& items,
                                 bool show_control_center);

  IDWriteTextLayout* LayoutFor(const std::wstring& text);

  const BarLayoutResult& last() const { return last_; }

 private:
  struct CacheEntry {
    std::wstring text;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
  };

  bool EnsureTextFormat();
  CacheEntry* GetOrCreate(const std::wstring& text);
  LONG ToPx(float dip) const;
  RECT PixelRect(const RECT& client, float left_dip, float width_dip) const;

  BarLayoutResult last_;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> format_;
  std::vector<CacheEntry> cache_;
  UINT dpi_ = 96;
  float max_width_dip_ = 0.0f;
  float max_height_dip_ = 0.0f;
};

}  // namespace bamti
