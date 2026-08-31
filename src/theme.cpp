#include "theme.hpp"

#include <d2d1helper.h>
#include <windows.h>

namespace bamti {

bool ShellUsesDarkMode() {
  DWORD value = 1;
  DWORD size = sizeof(value);
  const LSTATUS status = RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"SystemUsesLightTheme",
      RRF_RT_REG_DWORD,
      nullptr,
      &value,
      &size);
  if (status != ERROR_SUCCESS) {
    return true;
  }
  return value == 0;
}

D2D1_COLOR_F ClockTextColor(bool dark) {
  if (dark) {
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f);
  }
  return D2D1::ColorF(0.10f, 0.10f, 0.10f, 0.92f);
}

D2D1_COLOR_F MenuItemHoverFill(bool dark, bool pressed) {
  if (dark) {
    return pressed ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.16f) : D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.08f);
  }
  return pressed ? D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.10f) : D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.06f);
}

D2D1_COLOR_F WarningTextColor(bool dark) {
  if (dark) {
    return D2D1::ColorF(1.0f, 0.78f, 0.40f, 0.96f);
  }
  return D2D1::ColorF(0.62f, 0.32f, 0.04f, 0.96f);
}

D2D1_COLOR_F DockFillColor(bool dark) {
  if (dark) {
    return D2D1::ColorF(0.12f, 0.12f, 0.12f, 0.78f);
  }
  return D2D1::ColorF(0.98f, 0.98f, 0.98f, 0.82f);
}

D2D1_COLOR_F DockStrokeColor(bool dark) {
  if (dark) {
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
  }
  return D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.08f);
}

D2D1_COLOR_F DockIndicatorColor(bool dark) {
  if (dark) {
    return D2D1::ColorF(0.94f, 0.94f, 0.94f, 0.88f);
  }
  return D2D1::ColorF(0.22f, 0.22f, 0.22f, 0.55f);
}

D2D1_COLOR_F BatteryFillColor(bool dark, float level, bool charging) {
  if (charging || level > 0.20f) {
    return dark ? D2D1::ColorF(0x5BC85B) : D2D1::ColorF(0x0F7B0F);
  }
  if (level <= 0.10f) {
    return dark ? D2D1::ColorF(0xFF99A4) : D2D1::ColorF(0xC42B1C);
  }
  return dark ? D2D1::ColorF(0xFCE100) : D2D1::ColorF(0x9D5D00);
}

uint32_t BatteryFillRgb(bool dark, float level, bool charging) {
  if (charging || level > 0.20f) {
    return dark ? 0x5BC85Bu : 0x0F7B0Fu;
  }
  if (level <= 0.10f) {
    return dark ? 0xFF99A4u : 0xC42B1Cu;
  }
  return dark ? 0xFCE100u : 0x9D5D00u;
}

D2D1_COLOR_F CpuRingColor(bool dark, float usage) {
  if (usage < 0.30f) {
    return dark ? D2D1::ColorF(0x5BC85B) : D2D1::ColorF(0x0F7B0F);
  }
  if (usage < 0.60f) {
    return dark ? D2D1::ColorF(0xFCE100) : D2D1::ColorF(0x9D5D00);
  }
  if (usage < 0.85f) {
    return dark ? D2D1::ColorF(0xFF8C00) : D2D1::ColorF(0xC4520A);
  }
  return dark ? D2D1::ColorF(0xFF99A4) : D2D1::ColorF(0xC42B1C);
}

}  // namespace bamti
