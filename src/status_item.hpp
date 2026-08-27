#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <windows.h>

namespace bamti {

inline constexpr size_t kStatusTextMaxChars = 32;
inline constexpr size_t kStatusPanelTextMaxChars = 128;
inline constexpr size_t kStatusGaugeMax = 16;
inline constexpr size_t kStatusActionMax = 8;

struct StatusGauge {
  std::wstring label;
  float value = 0.0f;
  std::wstring detail;
  std::wstring note;
};

struct StatusPanel {
  std::wstring title;
  std::wstring subtitle;
  std::wstring updated_text;
  std::vector<StatusGauge> gauges;
  std::vector<std::wstring> actions;
};

struct StatusItem {
  std::string id;
  std::wstring text;
  std::wstring icon_glyph;
  std::wstring tooltip;
  uint32_t accent = 0;
  int priority = 0;
  std::optional<StatusPanel> panel;
};

struct StatusHit {
  std::string id;
  RECT rect{};
  std::wstring tooltip;
};

inline constexpr wchar_t kStatusPipeName[] = L"\\\\.\\pipe\\bamti-status";
inline constexpr UINT kStatusChangedMsg = WM_APP + 2;

}  // namespace bamti
