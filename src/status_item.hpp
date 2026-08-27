#pragma once

#include <string>
#include <windows.h>

namespace bamti {

struct StatusItem {
  std::string id;
  std::wstring text;
  std::wstring tooltip;
  int priority = 0;
};

struct StatusHit {
  std::string id;
  RECT rect{};
  std::wstring tooltip;
};

inline constexpr wchar_t kStatusPipeName[] = L"\\\\.\\pipe\\bamti-status";
inline constexpr UINT kStatusChangedMsg = WM_APP + 2;
inline constexpr size_t kStatusTextMaxChars = 32;

}  // namespace bamti
