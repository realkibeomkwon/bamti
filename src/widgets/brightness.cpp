#include "widgets/brightness.hpp"

#include "log.hpp"

#include <highlevelmonitorconfigurationapi.h>
#include <physicalmonitorenumerationapi.h>

#include <cstdint>
#include <vector>

namespace bamti {
namespace {

double QpcMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  if (freq.QuadPart == 0) {
    return 0.0;
  }
  return (end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

struct PhysicalMon {
  HANDLE handle = nullptr;
};

BOOL CALLBACK CollectPhysical(HMONITOR monitor, HDC, LPRECT, LPARAM lp) {
  auto* out = reinterpret_cast<std::vector<PhysicalMon>*>(lp);
  DWORD count = 0;
  if (GetNumberOfPhysicalMonitorsFromHMONITOR(monitor, &count) == FALSE || count == 0) {
    return TRUE;
  }
  std::vector<PHYSICAL_MONITOR> mons(count);
  if (GetPhysicalMonitorsFromHMONITOR(monitor, count, mons.data()) == FALSE) {
    return TRUE;
  }
  for (DWORD i = 0; i < count; ++i) {
    out->push_back(PhysicalMon{mons[i].hPhysicalMonitor});
  }
  return TRUE;
}

void ClosePhysical(std::vector<PhysicalMon>& mons) {
  if (mons.empty()) {
    return;
  }
  std::vector<PHYSICAL_MONITOR> raw(mons.size());
  for (size_t i = 0; i < mons.size(); ++i) {
    raw[i].hPhysicalMonitor = mons[i].handle;
  }
  DestroyPhysicalMonitors(static_cast<DWORD>(raw.size()), raw.data());
  mons.clear();
}

}  // namespace

BrightnessSample ProbeDdcciBrightness() {
  BrightnessSample out;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  std::vector<PhysicalMon> mons;
  EnumDisplayMonitors(nullptr, nullptr, CollectPhysical, reinterpret_cast<LPARAM>(&mons));
  for (const PhysicalMon& mon : mons) {
    DWORD mn = 0;
    DWORD cur = 0;
    DWORD mx = 0;
    if (GetMonitorBrightness(mon.handle, &mn, &cur, &mx) == FALSE || mx <= mn) {
      continue;
    }
    out.ok = true;
    out.value = static_cast<unsigned>(((cur - mn) * 100u) / (mx - mn));
    out.backend = BrightnessBackend::kDdcci;
    break;
  }
  ClosePhysical(mons);
  QueryPerformanceCounter(&t1);
  out.ms = QpcMs(t0, t1);
  return out;
}

bool SetDdcciBrightness(unsigned percent) {
  if (percent > 100) {
    percent = 100;
  }
  std::vector<PhysicalMon> mons;
  EnumDisplayMonitors(nullptr, nullptr, CollectPhysical, reinterpret_cast<LPARAM>(&mons));
  bool ok = false;
  for (const PhysicalMon& mon : mons) {
    DWORD mn = 0;
    DWORD cur = 0;
    DWORD mx = 0;
    if (GetMonitorBrightness(mon.handle, &mn, &cur, &mx) == FALSE || mx <= mn) {
      continue;
    }
    const DWORD next = mn + static_cast<DWORD>(((mx - mn) * percent) / 100u);
    if (SetMonitorBrightness(mon.handle, next) != FALSE) {
      ok = true;
      break;
    }
  }
  ClosePhysical(mons);
  return ok;
}

}  // namespace bamti
