#pragma once

#include <windows.h>

namespace bamti {

enum class BrightnessBackend { kNone, kDdcci, kWmi };

struct BrightnessSample {
  bool ok = false;
  unsigned value = 0;
  BrightnessBackend backend = BrightnessBackend::kNone;
  double ms = 0.0;
};

BrightnessSample ProbeDdcciBrightness();
BrightnessSample ProbeWmiBrightness();
bool SetDdcciBrightness(unsigned percent);
bool SetWmiBrightness(unsigned percent);

}  // namespace bamti
