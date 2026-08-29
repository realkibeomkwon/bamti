#pragma once

#include <string>
#include <vector>

namespace bamti {

inline constexpr size_t kTrayHiddenKeysMax = 64;

struct WidgetSettings {
  bool battery = false;
  bool cpu = false;
  bool network = false;
  bool widget_board = false;
  bool tray_mirror = true;
  bool tray_system_icons = false;
  bool tray_overflow_icons = true;
  std::vector<std::string> tray_hidden_keys;
  bool Any() const { return battery || cpu || network || widget_board; }
};

std::wstring SettingsPath();
WidgetSettings LoadWidgetSettings();
bool SaveWidgetSettings(const WidgetSettings& s);

}  // namespace bamti
