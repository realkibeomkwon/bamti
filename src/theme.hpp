#pragma once

#include <cstdint>
#include <d2d1.h>

namespace bamti {

bool ShellUsesDarkMode();
D2D1_COLOR_F ClockTextColor(bool dark);
D2D1_COLOR_F WarningTextColor(bool dark);
D2D1_COLOR_F MenuItemHoverFill(bool dark, bool pressed);
D2D1_COLOR_F DockFillColor(bool dark);
D2D1_COLOR_F DockStrokeColor(bool dark);
D2D1_COLOR_F DockIndicatorColor(bool dark);

// 독 알약과 팝업 메뉴가 같은 곡률을 쓴다. 값을 갈라 놓지 말 것.
// macOS Dock is ~20pt at the default bar height (~64pt). DWM ROUND/ROUNDSMALL
// cannot express that, so both surfaces draw the radius with Direct2D.
inline constexpr int kCornerRadiusDip = 20;

D2D1_COLOR_F BatteryFillColor(bool dark, float level, bool charging);
uint32_t BatteryFillRgb(bool dark, float level, bool charging);
D2D1_COLOR_F CpuRingColor(bool dark, float usage);

}  // namespace bamti
