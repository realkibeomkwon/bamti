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

}  // namespace bamti
