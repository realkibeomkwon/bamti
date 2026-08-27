#pragma once

#include <windows.h>

namespace bamti::dwm {

inline constexpr DWORD kUseImmersiveDarkMode = 20;
inline constexpr DWORD kWindowCornerPreference = 33;
inline constexpr DWORD kSystemBackdropType = 38;

inline constexpr int kCornerDoNotRound = 1;
inline constexpr int kCornerRound = 2;
inline constexpr int kCornerRoundSmall = 3;
inline constexpr int kBackdropNone = 1;
inline constexpr int kBackdropMainWindow = 2;
inline constexpr int kBackdropTransientWindow = 3;

}  // namespace bamti::dwm
