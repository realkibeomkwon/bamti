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

D2D1_COLOR_F DockLabelFill(bool dark) {
  if (dark) {
    return D2D1::ColorF(0.16f, 0.16f, 0.16f, 0.95f);
  }
  return D2D1::ColorF(0.96f, 0.96f, 0.96f, 0.95f);
}

D2D1_COLOR_F DockLabelText(bool dark) {
  if (dark) {
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f);
  }
  return D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.85f);
}

D2D1_COLOR_F AccentFillColor(bool dark) {
  return dark ? D2D1::ColorF(0x0A84FF) : D2D1::ColorF(0x0078D4);
}

D2D1_COLOR_F AccentOnColor(bool /*dark*/) {
  return D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f);
}

D2D1_COLOR_F CardFillColor(bool dark) {
  if (dark) {
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.06f);
  }
  return D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.045f);
}

D2D1_COLOR_F BadgeOffFill(bool dark) {
  if (dark) {
    return D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.14f);
  }
  return D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.09f);
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

uint32_t CpuFillRgb(bool dark, float usage) {
  if (usage < 0.40f) {
    return dark ? 0x5BC85Bu : 0x0F7B0Fu;
  }
  if (usage < 0.60f) {
    return dark ? 0xFCE100u : 0xC9A000u;
  }
  if (usage < 0.85f) {
    return dark ? 0xFF8C00u : 0xC4520Au;
  }
  return dark ? 0xFF6B6Bu : 0xC42B1Cu;
}

D2D1_COLOR_F CpuRingColor(bool dark, float usage) {
  return D2D1::ColorF(CpuFillRgb(dark, usage));
}

}  // namespace bamti
