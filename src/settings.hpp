#pragma once

#include <string>
#include <vector>

namespace bamti {

inline constexpr size_t kTrayHiddenKeysMax = 64;
inline constexpr size_t kBarOrderMax = 64;

struct WidgetSettings {
  bool battery = false;
  bool cpu = false;
  bool network = false;
  bool volume = false;
  bool widget_board = false;
  bool tray_mirror = true;
  bool tray_system_icons = false;
  bool tray_overflow_icons = true;
  std::string tray_backend = "uia";
  std::vector<std::string> tray_hidden_keys;
  std::vector<std::string> bar_order;  // 화면 오른쪽부터의 순서
  bool Any() const { return battery || cpu || network || volume || widget_board; }
};

std::wstring SettingsPath();
WidgetSettings LoadWidgetSettings();
bool SaveWidgetSettings(const WidgetSettings& s);

}  // namespace bamti
