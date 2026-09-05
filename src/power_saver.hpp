#pragma once

#include <functional>

namespace bamti {

struct WidgetSettings;

bool RestoreAbandonedSaver(WidgetSettings* settings);
bool SetBatterySaver(bool on, WidgetSettings* settings, const std::function<void()>& persist);
bool BatterySaverToggleAvailable();

}  // namespace bamti
