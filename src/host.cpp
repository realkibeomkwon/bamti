#include "host.hpp"

#include "dock.hpp"
#include "menu_bar.hpp"
#include "taskbar_controller.hpp"

#include <objbase.h>
#include <shellapi.h>
#include <uxtheme.h>

namespace bamti {
namespace {

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

}  // namespace

int Run(HINSTANCE instance) {
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

  if (restore_taskbar) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    TaskbarController::ForceRestore();
    if (SUCCEEDED(com)) {
      CoUninitialize();
    }
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
  }

  if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
    CloseHandle(mutex);
    return 1;
  }
  BufferedPaintInit();

  int exit_code = 1;
  {
    MenuBar bar;
    Dock dock;
    if (bar.Create(instance)) {
      if (bar.taskbar_hidden()) {
        dock.Create(instance);
      }
      MSG msg{};
      while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
      exit_code = static_cast<int>(msg.wParam);
    }
  }

  BufferedPaintUnInit();
  CoUninitialize();
  ReleaseMutex(mutex);
  CloseHandle(mutex);
  return exit_code;
}

}  // namespace bamti
