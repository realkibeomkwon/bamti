#include "host.hpp"

#include "dock.hpp"
#include "log.hpp"
#include "menu_bar.hpp"
#include "paths.hpp"
#include "taskbar_controller.hpp"
#include "tray_probe.hpp"
#include "watchdog.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

#include <cstdint>

namespace bamti {
namespace {

constexpr UINT kMsgFloodLimit = 500;
constexpr size_t kMsgFloodSlots = 64;

struct MsgFloodSlot {
  HWND hwnd;
  UINT message;
  UINT count;
};

struct MsgFlood {
  MsgFloodSlot slots[kMsgFloodSlots];
  UINT total;
  ULONGLONG window_start;
};

size_t MsgFloodIndex(HWND hwnd, UINT message) {
  auto h = reinterpret_cast<std::uintptr_t>(hwnd);
  h ^= static_cast<std::uintptr_t>(message) * 0x9E3779B9u;
  h ^= h >> 16;
  return static_cast<size_t>(h & (kMsgFloodSlots - 1));
}

void ResetMsgFlood(MsgFlood& flood) {
  flood.total = 0;
  flood.window_start = 0;
  for (size_t i = 0; i < kMsgFloodSlots; ++i) {
    flood.slots[i].hwnd = nullptr;
    flood.slots[i].message = 0;
    flood.slots[i].count = 0;
  }
}

void ClassNameForLog(HWND hwnd, wchar_t (&out)[256]) {
  if (hwnd == nullptr || GetClassNameW(hwnd, out, 256) <= 0) {
    out[0] = L'?';
    out[1] = L'\0';
  }
}

void LogMsgFlood(const MsgFlood& flood) {
  const MsgFloodSlot* top[3] = {};
  UINT best[3] = {};
  for (size_t i = 0; i < kMsgFloodSlots; ++i) {
    const UINT count = flood.slots[i].count;
    if (count == 0) {
      continue;
    }
    if (count > best[0]) {
      top[2] = top[1];
      best[2] = best[1];
      top[1] = top[0];
      best[1] = best[0];
      top[0] = &flood.slots[i];
      best[0] = count;
    } else if (count > best[1]) {
      top[2] = top[1];
      best[2] = best[1];
      top[1] = &flood.slots[i];
      best[1] = count;
    } else if (count > best[2]) {
      top[2] = &flood.slots[i];
      best[2] = count;
    }
  }

  wchar_t c0[256];
  wchar_t c1[256];
  wchar_t c2[256];
  ClassNameForLog(top[0] != nullptr ? top[0]->hwnd : nullptr, c0);
  ClassNameForLog(top[1] != nullptr ? top[1]->hwnd : nullptr, c1);
  ClassNameForLog(top[2] != nullptr ? top[2]->hwnd : nullptr, c2);
  Log(L"perf", L"msg flood %u/s top=[%s:0x%04X x%u] [%s:0x%04X x%u] [%s:0x%04X x%u]", flood.total, c0,
      top[0] != nullptr ? top[0]->message : 0, top[0] != nullptr ? top[0]->count : 0, c1,
      top[1] != nullptr ? top[1]->message : 0, top[1] != nullptr ? top[1]->count : 0, c2,
      top[2] != nullptr ? top[2]->message : 0, top[2] != nullptr ? top[2]->count : 0);
}

void NoteDispatched(const MSG& msg) {
  static MsgFlood flood{};
  const ULONGLONG now = GetTickCount64();
  if (flood.window_start != 0 && now - flood.window_start >= 1000) {
    if (flood.total > kMsgFloodLimit) {
      LogMsgFlood(flood);
    }
    ResetMsgFlood(flood);
  }
  if (flood.window_start == 0) {
    flood.window_start = now;
  }
  ++flood.total;

  size_t index = MsgFloodIndex(msg.hwnd, msg.message);
  for (int probe = 0; probe < 8; ++probe) {
    MsgFloodSlot& slot = flood.slots[(index + static_cast<size_t>(probe)) & (kMsgFloodSlots - 1)];
    if (slot.count == 0) {
      slot.hwnd = msg.hwnd;
      slot.message = msg.message;
      slot.count = 1;
      return;
    }
    if (slot.hwnd == msg.hwnd && slot.message == msg.message) {
      ++slot.count;
      return;
    }
  }
}

bool CommandLineHasRestoreTaskbar() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return false;
  }
  bool restore = false;
  for (int i = 1; i < argc; ++i) {
    if (lstrcmpiW(argv[i], L"--restore-taskbar") == 0) {
      restore = true;
      break;
    }
  }
  LocalFree(argv);
  return restore;
}

bool CommandLineHasProbeTray() {
  int argc = 0;
  LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  if (argv == nullptr) {
    return false;
  }
  bool probe = false;
  for (int i = 1; i < argc; ++i) {
    if (lstrcmpiW(argv[i], L"--probe-tray") == 0) {
      probe = true;
      break;
    }
  }
  LocalFree(argv);
  return probe;
}

}  // namespace

int Run(HINSTANCE instance) {
  if (CommandLineHasProbeTray()) {
    LogInit();
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const int code = RunTrayProbe();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    LogShutdown();
    return code;
  }

  const bool restore_taskbar = CommandLineHasRestoreTaskbar();
  const HANDLE mutex = CreateMutexW(nullptr, TRUE, L"Local\\bamti.singleton");
  if (mutex == nullptr) {
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS) {
    CloseHandle(mutex);
    if (restore_taskbar) {
      return 0;
    }
    if (const HWND existing = FindWindowW(kMenuBarClass, nullptr)) {
      ShowWindow(existing, SW_SHOWNA);
    }
    return 0;
  }

  LogInit();
  Log(L"host", L"start data=%s log=%s", DataDir().c_str(), LogFilePath().c_str());

  if (restore_taskbar) {
    Log(L"host", L"restore-taskbar");
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    TaskbarController::ForceRestore();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    LogShutdown();
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    Log(L"host", L"CoInitializeEx failed");
    LogShutdown();
    CloseHandle(mutex);
    return 1;
  }
  BufferedPaintInit();

  int exit_code = 1;
  {
    MenuBar bar;
    Dock dock;
    if (bar.Create(instance)) {
      WatchdogStart(bar.hwnd());
      Log(L"host", L"menu bar ready taskbar_hidden=%d", bar.taskbar_hidden() ? 1 : 0);
      if (bar.taskbar_hidden()) {
        if (!dock.Create(instance)) {
          Log(L"host", L"dock create failed");
        }
      }
      MSG msg{};
      for (;;) {
        WatchdogStage(L"idle");
        if (GetMessageW(&msg, nullptr, 0, 0) <= 0) {
          break;
        }
        NoteDispatched(msg);
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      WatchdogStop();
      exit_code = static_cast<int>(msg.wParam);
    } else {
      Log(L"host", L"menu bar create failed");
    }
  }

  Log(L"host", L"exit %d", exit_code);
  LogShutdown();
  BufferedPaintUnInit();
  CoUninitialize();
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return exit_code;
}

}  // namespace bamti
