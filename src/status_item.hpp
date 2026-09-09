#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <windows.h>

namespace bamti {

inline constexpr size_t kStatusTextMaxChars = 32;
inline constexpr size_t kStatusPanelTextMaxChars = 128;
inline constexpr size_t kStatusGlyphMaxChars = 8;
inline constexpr size_t kStatusRowMax = 32;
inline constexpr size_t kStatusGaugeMax = 16;
inline constexpr size_t kStatusActionMax = 8;
inline constexpr size_t kStatusItemsPerClient = 16;
inline constexpr size_t kStatusItemsMax = 64;
inline constexpr size_t kStatusIconPngMaxBytes = 8192;
inline constexpr size_t kStatusLineMaxBytes = 65536;

enum class StatusState { kNormal, kWarn, kError, kOn, kOff, kBusy };
enum class IconKind { kNone, kGlyph, kPng, kFile, kHicon, kVector };
enum class RowType { kGauge, kKeyValue, kText, kSeparator, kToggle, kButton, kSlider };

enum class VectorIcon : uint8_t {
  kNone = 0,
  kBattery,
  kCpu,
  kEthernet,
  kVolume,
  kWifi,
  kSearch,
  kControlCenter,
  kBluetooth,
};

inline constexpr uint32_t kVectorFlagCharging = 1u << 0;
inline constexpr uint32_t kVectorFlagMuted = 1u << 1;
inline constexpr uint32_t kVectorFlagOffline = 1u << 2;

struct StatusIcon {
  IconKind kind = IconKind::kNone;
  std::wstring glyph;
  std::vector<uint8_t> bytes;
  std::wstring path;
  HICON hicon = nullptr;
  VectorIcon vector = VectorIcon::kNone;
  float value = 0.0f;
  uint32_t flags = 0;
  uint64_t cache_key = 0;
};

struct StatusRow {
  RowType type = RowType::kText;
  std::string row_id;
  std::wstring label;
  std::wstring value_text;
  std::wstring detail;
  std::wstring note;
  std::wstring fallback_text;
  float value = 0.0f;
  uint32_t fill_rgb = 0;
  bool on = false;
  bool danger = false;
  bool muted = false;
};

struct StatusPanel {
  std::wstring title;
  std::wstring subtitle;
  std::wstring updated_text;
  std::vector<StatusRow> rows;
};

struct StatusItem {
  std::string id;
  std::string source;
  StatusIcon icon;
  std::wstring text;
  std::wstring tooltip;
  StatusState state = StatusState::kNormal;
  uint32_t accent = 0;
  int priority = 0;
  bool visible = true;
  std::optional<StatusPanel> panel;
  uint64_t revision = 0;
};

struct StatusHit {
  std::string id;
  RECT rect{};
  std::wstring tooltip;
};

inline constexpr wchar_t kStatusPipeName[] = L"\\\\.\\pipe\\bamti-status";
inline constexpr UINT kStatusChangedMsg = WM_APP + 2;

inline uint64_t Fnv1a64(const uint8_t* data, size_t n, uint64_t hash = 14695981039346656037ull) {
  for (size_t i = 0; i < n; ++i) {
    hash ^= data[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

inline std::string WideToUtf8Bytes(std::wstring_view wide) {
  if (wide.empty()) {
    return {};
  }
  const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr,
                                    nullptr);
  if (n <= 0) {
    return {};
  }
  std::string out(static_cast<size_t>(n), '\0');
  WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
  return out;
}

inline uint64_t HashStatusIcon(const StatusIcon& icon) {
  const uint8_t kind = static_cast<uint8_t>(icon.kind);
  uint64_t hash = Fnv1a64(&kind, 1);
  switch (icon.kind) {
    case IconKind::kGlyph: {
      const std::string u8 = WideToUtf8Bytes(icon.glyph);
      return Fnv1a64(reinterpret_cast<const uint8_t*>(u8.data()), u8.size(), hash);
    }
    case IconKind::kPng:
      return Fnv1a64(icon.bytes.data(), icon.bytes.size(), hash);
    case IconKind::kFile: {
      const std::string u8 = WideToUtf8Bytes(icon.path);
      return Fnv1a64(reinterpret_cast<const uint8_t*>(u8.data()), u8.size(), hash);
    }
    case IconKind::kHicon: {
      const uintptr_t handle = reinterpret_cast<uintptr_t>(icon.hicon);
      return Fnv1a64(reinterpret_cast<const uint8_t*>(&handle), sizeof(handle), hash);
    }
    case IconKind::kVector: {
      const uint8_t v = static_cast<uint8_t>(icon.vector);
      hash = Fnv1a64(&v, 1, hash);
      hash = Fnv1a64(reinterpret_cast<const uint8_t*>(&icon.value), sizeof(icon.value), hash);
      return Fnv1a64(reinterpret_cast<const uint8_t*>(&icon.flags), sizeof(icon.flags), hash);
    }
    case IconKind::kNone:
    default:
      return hash;
  }
}

inline std::wstring StatusBarText(const StatusItem& item) {
  if (item.icon.kind == IconKind::kVector) {
    return item.text;
  }
  const bool glyph = item.icon.kind == IconKind::kGlyph && !item.icon.glyph.empty();
  if (!glyph) {
    return item.text;
  }
  if (item.text.empty()) {
    return item.icon.glyph;
  }
  return item.icon.glyph + L" " + item.text;
}

}  // namespace bamti
