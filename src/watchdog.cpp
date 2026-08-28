#include "watchdog.hpp"

#include "log.hpp"

#include <atomic>
#include <thread>

namespace bamti {
namespace {

std::atomic<const wchar_t*> g_stage{L"idle"};
std::atomic<bool> g_stop{false};
HWND g_hwnd = nullptr;
HANDLE g_stop_event = nullptr;
std::thread g_thread;

const wchar_t* CurrentStage() {
  const wchar_t* stage = g_stage.load(std::memory_order_relaxed);
  return stage != nullptr ? stage : L"?";
}

void WatchdogLoop() {
  bool stuck = false;
  ULONGLONG stuck_at = 0;
  ULONGLONG last_stuck_log = 0;
  while (g_stop_event != nullptr && WaitForSingleObject(g_stop_event, 500) == WAIT_TIMEOUT) {
    if (g_stop.load(std::memory_order_acquire)) {
      break;
    }
    const HWND hwnd = g_hwnd;
    if (hwnd == nullptr || !IsWindow(hwnd)) {
      continue;
    }
    DWORD_PTR result = 0;
    const LRESULT ok = SendMessageTimeoutW(hwnd, WM_NULL, 0, 0, SMTO_NORMAL, 1000, &result);
    static_cast<void>(result);
    const bool timed_out = ok == 0 && GetLastError() == ERROR_TIMEOUT;
    const ULONGLONG now = GetTickCount64();
    if (timed_out) {
      if (!stuck) {
        stuck = true;
        stuck_at = now;
        last_stuck_log = now;
        LogTry(L"watchdog", L"UI STUCK stage=%s", CurrentStage());
      } else if (now - last_stuck_log >= 5000) {
        last_stuck_log = now;
        LogTry(L"watchdog", L"UI STUCK stage=%s", CurrentStage());
      }
    } else if (stuck) {
      LogTry(L"watchdog", L"UI recovered after %ums stage=%s", static_cast<unsigned>(now - stuck_at), CurrentStage());
      stuck = false;
    }
  }
}

}  // namespace

void WatchdogStage(const wchar_t* stage) {
  if (stage != nullptr) {
    g_stage.store(stage, std::memory_order_relaxed);
  }
}

bool WatchdogStart(HWND ui_window) {
  if (ui_window == nullptr || !IsWindow(ui_window) || g_thread.joinable()) {
    return false;
  }
  g_stop.store(false, std::memory_order_release);
  g_stop_event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (g_stop_event == nullptr) {
    return false;
  }
  g_hwnd = ui_window;
  g_thread = std::thread(WatchdogLoop);
  return true;
}

void WatchdogStop() {
  g_stop.store(true, std::memory_order_release);
  if (g_stop_event != nullptr) {
    SetEvent(g_stop_event);
  }
  if (g_thread.joinable()) {
    g_thread.join();
  }
  if (g_stop_event != nullptr) {
    CloseHandle(g_stop_event);
    g_stop_event = nullptr;
  }
  g_hwnd = nullptr;
}

}  // namespace bamti
