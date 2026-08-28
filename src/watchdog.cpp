#include "watchdog.hpp"

#include "log.hpp"

#include <atomic>
#include <cstdint>
#include <thread>

namespace bamti {
namespace {

std::atomic<const wchar_t*> g_stage{L"idle"};
std::atomic<bool> g_stop{false};
HWND g_hwnd = nullptr;
HANDLE g_stop_event = nullptr;
HANDLE g_file = INVALID_HANDLE_VALUE;
HANDLE g_ui_thread = nullptr;
std::thread g_thread;

const wchar_t* CurrentStage() {
  const wchar_t* stage = g_stage.load(std::memory_order_relaxed);
  return stage != nullptr ? stage : L"?";
}

void Emit(const wchar_t* body) {
  SYSTEMTIME st{};
  GetLocalTime(&st);
  wchar_t line[1024]{};
  swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [watchdog] %s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, body != nullptr ? body : L"");
  OutputDebugStringW(line);
  if (g_file == nullptr || g_file == INVALID_HANDLE_VALUE) {
    return;
  }
  char utf8[2048]{};
  const int bytes = WideCharToMultiByte(CP_UTF8, 0, line, -1, utf8, sizeof(utf8), nullptr, nullptr);
  if (bytes <= 1) {
    return;
  }
  DWORD written = 0;
  WriteFile(g_file, utf8, static_cast<DWORD>(bytes - 1), &written, nullptr);
  FlushFileBuffers(g_file);
}

bool OpenWatchdogFile() {
  wchar_t path[MAX_PATH]{};
  const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", path, MAX_PATH - 32);
  if (n == 0 || n >= MAX_PATH - 32) {
    return false;
  }
  if (wcscat_s(path, L"\\.bamti") != 0) {
    return false;
  }
  CreateDirectoryW(path, nullptr);
  if (wcscat_s(path, L"\\watchdog.log") != 0) {
    return false;
  }
  g_file = CreateFileW(path, GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                       FILE_ATTRIBUTE_NORMAL, nullptr);
  if (g_file == INVALID_HANDLE_VALUE) {
    return false;
  }
  SetFilePointer(g_file, 0, nullptr, FILE_END);
  return true;
}

void CaptureRip(uintptr_t* rip, uintptr_t* rva, bool* have_rip) {
  *rip = 0;
  *rva = 0;
  *have_rip = false;
  if (g_ui_thread == nullptr) {
    return;
  }
  const DWORD susp = SuspendThread(g_ui_thread);
  if (susp == static_cast<DWORD>(-1)) {
    return;
  }
  CONTEXT ctx{};
  ctx.ContextFlags = CONTEXT_CONTROL;
  const BOOL got = GetThreadContext(g_ui_thread, &ctx);
  ResumeThread(g_ui_thread);
  if (!got) {
    return;
  }
  *have_rip = true;
  *rip = static_cast<uintptr_t>(ctx.Rip);
  if (const HMODULE self = GetModuleHandleW(nullptr)) {
    *rva = *rip - reinterpret_cast<uintptr_t>(self);
  }
}

void WatchdogLoop() {
  bool stuck = false;
  ULONGLONG stuck_at = 0;
  ULONGLONG last_stuck_log = 0;
  ULONGLONG last_alive_log = 0;
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
    const wchar_t* stage = CurrentStage();
    if (timed_out) {
      wchar_t body[512]{};
      if (!stuck) {
        stuck = true;
        stuck_at = now;
        last_stuck_log = now;
        uintptr_t rip = 0;
        uintptr_t rva = 0;
        bool have_rip = false;
        CaptureRip(&rip, &rva, &have_rip);
        const int log_busy = LogBusy() ? 1 : 0;
        if (have_rip) {
          swprintf_s(body, L"STUCK stage=%s ping=timeout rip=0x%llx rva=0x%llx log_busy=%d", stage,
                     static_cast<unsigned long long>(rip), static_cast<unsigned long long>(rva), log_busy);
        } else {
          swprintf_s(body, L"STUCK stage=%s ping=timeout log_busy=%d", stage, log_busy);
        }
        Emit(body);
      } else if (now - last_stuck_log >= 5000) {
        last_stuck_log = now;
        swprintf_s(body, L"STUCK stage=%s ping=timeout log_busy=%d", stage, LogBusy() ? 1 : 0);
        Emit(body);
      }
    } else {
      if (stuck) {
        wchar_t body[256]{};
        swprintf_s(body, L"recovered after %ums stage=%s ping=ok", static_cast<unsigned>(now - stuck_at), stage);
        Emit(body);
        stuck = false;
      }
      if (last_alive_log == 0 || now - last_alive_log >= 5000) {
        last_alive_log = now;
        wchar_t body[256]{};
        swprintf_s(body, L"alive stage=%s ping=ok", stage);
        Emit(body);
      }
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
  OpenWatchdogFile();
  const DWORD tid = GetWindowThreadProcessId(ui_window, nullptr);
  if (tid != 0) {
    g_ui_thread = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, tid);
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
  if (g_ui_thread != nullptr) {
    CloseHandle(g_ui_thread);
    g_ui_thread = nullptr;
  }
  if (g_file != nullptr && g_file != INVALID_HANDLE_VALUE) {
    CloseHandle(g_file);
    g_file = INVALID_HANDLE_VALUE;
  }
  g_hwnd = nullptr;
}

}  // namespace bamti
