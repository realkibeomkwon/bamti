#pragma once

#include <d2d1.h>

namespace bamti {

bool ShellUsesDarkMode();
D2D1_COLOR_F ClockTextColor(bool dark);
D2D1_COLOR_F WarningTextColor(bool dark);
D2D1_COLOR_F MenuItemHoverFill(bool dark, bool pressed);
D2D1_COLOR_F DockFillColor(bool dark);
D2D1_COLOR_F DockStrokeColor(bool dark);
D2D1_COLOR_F DockIndicatorColor(bool dark);

}  // namespace bamti
