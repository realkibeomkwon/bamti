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
D2D1_COLOR_F AccentFillColor(bool dark);
D2D1_COLOR_F AccentOnColor(bool dark);
D2D1_COLOR_F CardFillColor(bool dark);
D2D1_COLOR_F BadgeOffFill(bool dark);

// 독 알약과 팝업 메뉴가 같은 곡률을 쓴다. 값을 갈라 놓지 말 것.
// 예전 DWM RoundSmall(약 4px) 메뉴에 맞춘다.
inline constexpr int kCornerRadiusDip = 4;

D2D1_COLOR_F BatteryFillColor(bool dark, float level, bool charging);
uint32_t BatteryFillRgb(bool dark, float level, bool charging);
D2D1_COLOR_F CpuRingColor(bool dark, float usage);
uint32_t CpuFillRgb(bool dark, float usage);

}  // namespace bamti
