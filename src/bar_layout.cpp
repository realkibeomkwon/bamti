#include "bar_layout.hpp"

#include <algorithm>
#include <cstddef>

namespace bamti {
namespace {

constexpr float kFontSizeDip = 13.0f;
constexpr float kPadRightDip = 14.0f;
constexpr float kStatusClockGapDip = 20.0f;
constexpr float kItemGapDip = 14.0f;
constexpr float kStartPadLeftDip = 4.0f;
constexpr float kStartHitWidthDip = 34.0f;
constexpr float kStatusIconDip = 16.0f;
constexpr float kBatteryIconDip = 24.0f;
constexpr float kStatusIconGapDip = 4.0f;
constexpr size_t kLayoutCacheMax = 64;
constexpr wchar_t kOverflowGlyph[] = L"\u2039";  // ‹

bool IconHasArt(IconKind kind) {
  return kind == IconKind::kPng || kind == IconKind::kFile || kind == IconKind::kHicon ||
         kind == IconKind::kVector;
}

bool ItemOnBar(const StatusItem& item) {
  if (!item.visible) {
    return false;
  }
  if (IconHasArt(item.icon.kind)) {
    return true;
  }
  return !StatusBarText(item).empty();
}

float StatusIconWidth(const StatusItem& item) {
  if (item.icon.kind == IconKind::kVector && item.icon.vector == VectorIcon::kBattery) {
    return kBatteryIconDip;
  }
  return kStatusIconDip;
}

}  // namespace

bool BarLayout::Initialize() {
  const HRESULT hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                                         reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf()));
  if (FAILED(hr)) {
    return false;
  }
  return EnsureTextFormat();
}

void BarLayout::SetDpi(UINT dpi) {
  const UINT next = dpi == 0 ? 96 : dpi;
  if (dpi_ == next) {
    return;
  }
  dpi_ = next;
  format_.Reset();
  cache_.clear();
}

bool BarLayout::EnsureTextFormat() {
  if (format_) {
    return true;
  }
  if (!dwrite_) {
    return false;
  }

  wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
  if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
    locale[0] = L'e';
    locale[1] = L'n';
    locale[2] = L'-';
    locale[3] = L'U';
    locale[4] = L'S';
  }

  const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
  HRESULT hr = E_FAIL;
  for (const wchar_t* family : families) {
    hr = dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, kFontSizeDip, locale, format_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
      break;
    }
  }
  if (FAILED(hr) || !format_) {
    return false;
  }

  format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  return true;
}

LONG BarLayout::ToPx(float dip) const {
  return static_cast<LONG>(dip * (static_cast<float>(dpi_) / 96.0f));
}

RECT BarLayout::PixelRect(const RECT& client, float left_dip, float width_dip) const {
  RECT rc{};
  rc.left = client.left + ToPx(left_dip);
  rc.top = client.top;
  rc.right = client.left + ToPx(left_dip + width_dip);
  rc.bottom = client.bottom;
  return rc;
}

BarLayout::CacheEntry* BarLayout::GetOrCreate(const std::wstring& text) {
  if (text.empty() || !EnsureTextFormat() || !dwrite_) {
    return nullptr;
  }
  for (size_t i = 0; i < cache_.size(); ++i) {
    if (cache_[i].text == text) {
      if (i + 1 != cache_.size()) {
        CacheEntry entry = std::move(cache_[i]);
        cache_.erase(cache_.begin() + static_cast<std::ptrdiff_t>(i));
        cache_.push_back(std::move(entry));
      }
      return &cache_.back();
    }
  }

  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  const HRESULT hr = dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format_.Get(),
                                               max_width_dip_, max_height_dip_, layout.ReleaseAndGetAddressOf());
  if (FAILED(hr) || !layout) {
    return nullptr;
  }
  Microsoft::WRL::ComPtr<IDWriteTypography> typography;
  if (SUCCEEDED(dwrite_->CreateTypography(typography.ReleaseAndGetAddressOf()))) {
    const DWRITE_FONT_FEATURE feature{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
    typography->AddFontFeature(feature);
    const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
    layout->SetTypography(typography.Get(), range);
  }
  DWRITE_TEXT_METRICS metrics{};
  if (FAILED(layout->GetMetrics(&metrics))) {
    return nullptr;
  }

  if (cache_.size() >= kLayoutCacheMax) {
    cache_.erase(cache_.begin());
  }
  CacheEntry entry;
  entry.text = text;
  entry.layout = std::move(layout);
  entry.metrics = metrics;
  cache_.push_back(std::move(entry));
  return &cache_.back();
}

IDWriteTextLayout* BarLayout::LayoutFor(const std::wstring& text) {
  CacheEntry* entry = GetOrCreate(text);
  return entry != nullptr ? entry->layout.Get() : nullptr;
}

const BarLayoutResult& BarLayout::Compute(const RECT& client, const std::wstring& clock_text,
                                          const std::wstring& warning_text, const std::vector<StatusItem>& items) {
  last_ = BarLayoutResult{};
  last_.dpi = dpi_;
  last_.client = client;

  max_width_dip_ = static_cast<float>(client.right - client.left) * 96.0f / static_cast<float>(dpi_);
  max_height_dip_ = static_cast<float>(client.bottom - client.top) * 96.0f / static_cast<float>(dpi_);

  BarSegment start;
  start.kind = SegmentKind::kStart;
  start.rect = PixelRect(client, kStartPadLeftDip, kStartHitWidthDip);

  std::vector<BarSegment> left;
  left.push_back(std::move(start));

  float left_limit = kStartPadLeftDip + kStartHitWidthDip + kItemGapDip;
  if (!warning_text.empty()) {
    if (CacheEntry* warn = GetOrCreate(warning_text)) {
      BarSegment seg;
      seg.kind = SegmentKind::kWarning;
      seg.text = warning_text;
      seg.rect = PixelRect(client, left_limit, warn->metrics.widthIncludingTrailingWhitespace);
      left_limit += warn->metrics.widthIncludingTrailingWhitespace + kItemGapDip;
      left.push_back(std::move(seg));
    }
  }

  float cursor = max_width_dip_ - kPadRightDip;
  BarSegment clock;
  clock.kind = SegmentKind::kClock;
  if (!clock_text.empty()) {
    if (CacheEntry* entry = GetOrCreate(clock_text)) {
      const float width = entry->metrics.widthIncludingTrailingWhitespace;
      const float x = cursor - width;
      clock.text = clock_text;
      clock.rect = PixelRect(client, x, width);
      cursor = x - kStatusClockGapDip;
    }
  }

  const std::vector<StatusItem>& ordered = items;

  std::vector<BarSegment> status;
  std::vector<StatusItem> visible;
  std::vector<float> status_x;
  bool overflowed = false;
  for (size_t i = 0; i < ordered.size(); ++i) {
    const StatusItem& item = ordered[i];
    if (!item.visible) {
      last_.overflow.push_back(item);
      overflowed = true;
      continue;
    }
    if (!ItemOnBar(item)) {
      continue;
    }
    const std::wstring label = StatusBarText(item);
    const bool bitmap = IconHasArt(item.icon.kind);
    float text_w = 0.0f;
    if (!label.empty()) {
      CacheEntry* entry = GetOrCreate(label);
      if (entry == nullptr && !bitmap) {
        continue;
      }
      if (entry != nullptr) {
        text_w = entry->metrics.widthIncludingTrailingWhitespace;
      }
    } else if (!bitmap) {
      continue;
    }
    const float icon_w = bitmap ? StatusIconWidth(item) : 0.0f;
    const float gap = (bitmap && text_w > 0.0f) ? kStatusIconGapDip : 0.0f;
    const float width = icon_w + gap + text_w;
    if (width <= 0.0f) {
      continue;
    }
    const float x = cursor - width;
    if (x < left_limit) {
      for (size_t j = i; j < ordered.size(); ++j) {
        if (ItemOnBar(ordered[j]) || !ordered[j].visible) {
          last_.overflow.push_back(ordered[j]);
        }
      }
      overflowed = true;
      break;
    }
    BarSegment seg;
    seg.kind = SegmentKind::kStatus;
    seg.id = item.id;
    seg.text = label;
    seg.tooltip = item.tooltip;
    seg.accent = item.accent;
    if (IconHasArt(item.icon.kind)) {
      seg.icon_kind = item.icon.kind;
      seg.icon_key = item.icon.cache_key;
      seg.icon = item.icon;
    }
    seg.rect = PixelRect(client, x, width);
    cursor = x - kItemGapDip;
    status_x.push_back(x);
    visible.push_back(item);
    status.push_back(std::move(seg));
  }

  BarSegment chevron;
  if (overflowed || !last_.overflow.empty()) {
    if (CacheEntry* mark = GetOrCreate(kOverflowGlyph)) {
      const float width = mark->metrics.widthIncludingTrailingWhitespace;
      for (int pass = 0; pass < 2; ++pass) {
        float x = cursor - width;
        if (!status_x.empty()) {
          x = status_x.back() - kItemGapDip - width;
        }
        if (x >= left_limit) {
          chevron.kind = SegmentKind::kOverflow;
          chevron.text = kOverflowGlyph;
          chevron.tooltip = L"접힌 항목";
          chevron.rect = PixelRect(client, x, width);
          break;
        }
        if (status.empty()) {
          break;
        }
        last_.overflow.insert(last_.overflow.begin(), visible.back());
        visible.pop_back();
        status.pop_back();
        status_x.pop_back();
        if (!status_x.empty()) {
          cursor = status_x.back() - kItemGapDip;
        } else {
          cursor = max_width_dip_ - kPadRightDip;
          if (!clock.text.empty()) {
            cursor -= static_cast<float>(clock.rect.right - clock.rect.left) * 96.0f / static_cast<float>(dpi_) +
                      kStatusClockGapDip;
          }
        }
      }
    }
  }

  last_.segments = std::move(left);
  if (chevron.kind == SegmentKind::kOverflow) {
    last_.segments.push_back(std::move(chevron));
  }
  for (auto it = status.rbegin(); it != status.rend(); ++it) {
    last_.segments.push_back(std::move(*it));
  }
  if (!clock.text.empty() || clock.rect.right > clock.rect.left) {
    last_.segments.push_back(std::move(clock));
  }
  return last_;
}

}  // namespace bamti
