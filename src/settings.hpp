#pragma once

#include <string>

namespace bamti {

struct WidgetSettings {
  bool battery = false;
  bool cpu = false;
  bool network = false;
  bool widget_board = false;
  bool Any() const { return battery || cpu || network || widget_board; }
};

std::wstring SettingsPath();
WidgetSettings LoadWidgetSettings();
bool SaveWidgetSettings(const WidgetSettings& s);

}  // namespace bamti
