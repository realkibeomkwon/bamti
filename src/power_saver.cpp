#include "power_saver.hpp"

#include "log.hpp"
#include "settings.hpp"

#include <initguid.h>
#include <windows.h>
#include <powrprof.h>

#include <atomic>

namespace bamti {
namespace {

constexpr DWORD kDefaultThreshold = 20;
constexpr DWORD kSaverOnThreshold = 100;
constexpr DWORD kFlagPollMs = 200;
constexpr DWORD kFlagWaitMaxMs = 3000;
constexpr int kToggleFailLimit = 3;

std::atomic<bool> g_toggle_ok{true};
std::atomic<int> g_fail_count{0};

void LogGuidsOnce() {
  static bool logged = false;
  if (logged) {
    return;
  }
  logged = true;
  Log(L"power",
      L"GUID_ENERGY_SAVER_SUBGROUP={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} "
      L"GUID_ENERGY_SAVER_BATTERY_THRESHOLD={%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
      static_cast<unsigned long>(GUID_ENERGY_SAVER_SUBGROUP.Data1), GUID_ENERGY_SAVER_SUBGROUP.Data2,
      GUID_ENERGY_SAVER_SUBGROUP.Data3, GUID_ENERGY_SAVER_SUBGROUP.Data4[0], GUID_ENERGY_SAVER_SUBGROUP.Data4[1],
      GUID_ENERGY_SAVER_SUBGROUP.Data4[2], GUID_ENERGY_SAVER_SUBGROUP.Data4[3], GUID_ENERGY_SAVER_SUBGROUP.Data4[4],
      GUID_ENERGY_SAVER_SUBGROUP.Data4[5], GUID_ENERGY_SAVER_SUBGROUP.Data4[6], GUID_ENERGY_SAVER_SUBGROUP.Data4[7],
      static_cast<unsigned long>(GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data1), GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data2,
      GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data3, GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[0],
      GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[1], GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[2],
      GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[3], GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[4],
      GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[5], GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[6],
      GUID_ENERGY_SAVER_BATTERY_THRESHOLD.Data4[7]);
}

void MarkFailed(const wchar_t* why, DWORD err) {
  if (!g_toggle_ok.exchange(false)) {
    return;
  }
  Log(L"power", L"saver toggle abandoned (%s) err=%lu; falling back to ms-settings:batterysaver",
      why != nullptr ? why : L"unknown", static_cast<unsigned long>(err));
}

void NoteSuccess() {
  g_fail_count.store(0);
}

void NoteFailure(const wchar_t* why, DWORD err) {
  const int n = g_fail_count.fetch_add(1) + 1;
  Log(L"power", L"saver toggle failed (%s) err=%lu streak=%d", why != nullptr ? why : L"unknown",
      static_cast<unsigned long>(err), n);
  if (n >= kToggleFailLimit) {
    MarkFailed(why, err);
  }
}

bool SaverFlagOn();

void LogFlagMiss(DWORD waited_ms, DWORD threshold) {
  SYSTEM_POWER_STATUS status{};
  int pct = -1;
  int ac = -1;
  if (GetSystemPowerStatus(&status) != FALSE) {
    pct = static_cast<int>(status.BatteryLifePercent);
    ac = static_cast<int>(status.ACLineStatus);
  }
  Log(L"power", L"saver flag did not follow within %lu ms (pct=%d ac=%d threshold=%lu)",
      static_cast<unsigned long>(waited_ms), pct, ac, static_cast<unsigned long>(threshold));
}

bool WaitSaverFlag(bool want_on, DWORD* waited_ms) {
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    if (SaverFlagOn() == want_on) {
      if (waited_ms != nullptr) {
        *waited_ms = static_cast<DWORD>(GetTickCount64() - start);
      }
      return true;
    }
    const ULONGLONG elapsed = GetTickCount64() - start;
    if (elapsed >= kFlagWaitMaxMs) {
      if (waited_ms != nullptr) {
        *waited_ms = static_cast<DWORD>(elapsed);
      }
      return false;
    }
    Sleep(kFlagPollMs);
  }
}

bool ActiveScheme(GUID** scheme) {
  if (scheme == nullptr) {
    return false;
  }
  *scheme = nullptr;
  const DWORD err = PowerGetActiveScheme(nullptr, scheme);
  if (err != ERROR_SUCCESS || *scheme == nullptr) {
    Log(L"power", L"PowerGetActiveScheme failed err=%lu", static_cast<unsigned long>(err));
    return false;
  }
  return true;
}

bool ReadThreshold(DWORD* value) {
  if (value == nullptr) {
    return false;
  }
  LogGuidsOnce();
  GUID* scheme = nullptr;
  if (!ActiveScheme(&scheme)) {
    return false;
  }
  DWORD index = 0;
  const DWORD err = PowerReadDCValueIndex(nullptr, scheme, &GUID_ENERGY_SAVER_SUBGROUP,
                                          &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, &index);
  LocalFree(scheme);
  if (err != ERROR_SUCCESS) {
    Log(L"power", L"PowerReadDCValueIndex failed err=%lu", static_cast<unsigned long>(err));
    return false;
  }
  *value = index;
  return true;
}

bool WriteThreshold(DWORD next) {
  DWORD old = 0;
  const bool have_old = ReadThreshold(&old);
  Log(L"power", L"saver threshold %d -> %d", have_old ? static_cast<int>(old) : -1, static_cast<int>(next));
  GUID* scheme = nullptr;
  if (!ActiveScheme(&scheme)) {
    return false;
  }
  const DWORD write = PowerWriteDCValueIndex(nullptr, scheme, &GUID_ENERGY_SAVER_SUBGROUP,
                                             &GUID_ENERGY_SAVER_BATTERY_THRESHOLD, next);
  DWORD apply = ERROR_SUCCESS;
  if (write == ERROR_SUCCESS) {
    apply = PowerSetActiveScheme(nullptr, scheme);
  }
  LocalFree(scheme);
  if (write != ERROR_SUCCESS) {
    Log(L"power", L"PowerWriteDCValueIndex failed err=%lu", static_cast<unsigned long>(write));
    return false;
  }
  if (apply != ERROR_SUCCESS) {
    Log(L"power", L"PowerSetActiveScheme failed err=%lu", static_cast<unsigned long>(apply));
    return false;
  }
  return true;
}

bool SaverFlagOn() {
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status) == FALSE) {
    return false;
  }
  return (status.SystemStatusFlag & 1) != 0;
}

bool OnBattery() {
  SYSTEM_POWER_STATUS status{};
  if (GetSystemPowerStatus(&status) == FALSE) {
    return false;
  }
  return status.ACLineStatus == 0;
}

}  // namespace

bool BatterySaverToggleAvailable() {
  return g_toggle_ok.load();
}

bool RestoreAbandonedSaver(WidgetSettings* settings) {
  if (settings == nullptr || settings->saver_threshold_backup == -1) {
    return false;
  }
  const int backup = settings->saver_threshold_backup;
  const DWORD next = backup >= 0 ? static_cast<DWORD>(backup) : kDefaultThreshold;
  if (!WriteThreshold(next)) {
    MarkFailed(L"startup restore write", GetLastError());
    return false;
  }
  settings->saver_threshold_backup = -1;
  Log(L"power", L"restored abandoned saver threshold %d", static_cast<int>(next));
  return true;
}

bool SetBatterySaver(bool on, WidgetSettings* settings, const std::function<void()>& persist) {
  if (settings == nullptr) {
    return false;
  }
  if (!g_toggle_ok.load()) {
    return false;
  }
  if (on && !OnBattery()) {
    return false;
  }

  DWORD current = kDefaultThreshold;
  if (!ReadThreshold(&current)) {
    NoteFailure(L"read", GetLastError());
    return false;
  }

  if (on) {
    if (settings->saver_threshold_backup == -1) {
      settings->saver_threshold_backup = static_cast<int>(current);
      if (persist) {
        persist();
      }
    }
    if (!WriteThreshold(kSaverOnThreshold)) {
      NoteFailure(L"write on", GetLastError());
      return false;
    }
    if (OnBattery()) {
      DWORD waited = 0;
      if (!WaitSaverFlag(true, &waited)) {
        LogFlagMiss(waited, kSaverOnThreshold);
        const DWORD revert =
            settings->saver_threshold_backup >= 0 ? static_cast<DWORD>(settings->saver_threshold_backup)
                                                  : kDefaultThreshold;
        if (WriteThreshold(revert)) {
          settings->saver_threshold_backup = -1;
        }
        NoteFailure(L"SystemStatusFlag did not follow", 0);
        return false;
      }
    }
    NoteSuccess();
    return true;
  }

  const DWORD next =
      settings->saver_threshold_backup >= 0 ? static_cast<DWORD>(settings->saver_threshold_backup) : kDefaultThreshold;
  if (!WriteThreshold(next)) {
    NoteFailure(L"write off", GetLastError());
    return false;
  }
  settings->saver_threshold_backup = -1;
  if (OnBattery()) {
    DWORD waited = 0;
    if (!WaitSaverFlag(false, &waited)) {
      LogFlagMiss(waited, next);
      NoteFailure(L"SystemStatusFlag stayed on", 0);
      return false;
    }
  }
  NoteSuccess();
  return true;
}

}  // namespace bamti
