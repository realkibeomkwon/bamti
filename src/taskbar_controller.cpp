#include "taskbar_controller.hpp"

#include "log.hpp"
#include "paths.hpp"

#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <utility>
#include <vector>

namespace bamti {
namespace {

constexpr wchar_t kPrimaryClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryClass[] = L"Shell_SecondaryTrayWnd";
constexpr LONG kParkY = 32000;

HWINEVENTHOOK g_tray_hooks[3] = {};
HWND g_tray_hwnd = nullptr;
WINEVENTPROC g_tray_proc = nullptr;
int g_tray_suppress_depth = 0;
ULONGLONG g_tray_suppress_until = 0;
ULONGLONG g_peek_until = 0;

constexpr DWORD kTrayEvents[] = {
    EVENT_OBJECT_SHOW,
    EVENT_OBJECT_STATECHANGE,
    EVENT_OBJECT_LOCATIONCHANGE,
};

struct TraySuppressGuard {
  TraySuppressGuard() { ++g_tray_suppress_depth; }
  ~TraySuppressGuard() {
    if (g_tray_suppress_depth > 0) {
      --g_tray_suppress_depth;
    }
    if (g_tray_suppress_depth == 0) {
      g_tray_suppress_until = GetTickCount64() + 200;
    }
  }
};

bool OwnProcessWindow(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

}  // namespace

HWND FindExplorerShellTrayWnd() {
  HWND hwnd = nullptr;
  while ((hwnd = FindWindowExW(nullptr, hwnd, kPrimaryClass, nullptr)) != nullptr) {
    if (!OwnProcessWindow(hwnd)) {
      return hwnd;
    }
  }
  return nullptr;
}

namespace {

void EnumTrays(const auto& fn) {
  if (HWND primary = FindExplorerShellTrayWnd()) {
    fn(primary);
  }
  HWND secondary = nullptr;
  while ((secondary = FindWindowExW(nullptr, secondary, kSecondaryClass, nullptr)) != nullptr) {
    fn(secondary);
  }
}

std::vector<std::pair<HWND, RECT>>& TrayBackup() {
  static std::vector<std::pair<HWND, RECT>> saved;
  return saved;
}

void RememberTray(HWND hwnd, const RECT& rc) {
  auto& saved = TrayBackup();
  for (auto& item : saved) {
    if (item.first == hwnd) {
      item.second = rc;
      return;
    }
  }
  saved.push_back({hwnd, rc});
}

bool LookupTray(HWND hwnd, RECT& rc) {
  for (const auto& item : TrayBackup()) {
    if (item.first == hwnd) {
      rc = item.second;
      return true;
    }
  }
  return false;
}

}  // namespace

TaskbarController::~TaskbarController() {
  Restore();
}

HWND TaskbarController::PrimaryTray() {
  return FindExplorerShellTrayWnd();
}

std::wstring TaskbarController::GuardPath() {
  return TaskbarGuardPath();
}

bool TaskbarController::WriteGuard() const {
  const std::wstring path = GuardPath();
  if (path.empty()) {
    return false;
  }
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"w, ccs=UTF-8") != 0 || file == nullptr) {
    return false;
  }
  fwprintf(file, L"v=1\nstate=%u\n", original_state_);
  fclose(file);
  return true;
}

bool TaskbarController::ReadGuard(UINT& state) {
  const std::wstring path = GuardPath();
  if (path.empty()) {
    return false;
  }
  FILE* file = nullptr;
  if (_wfopen_s(&file, path.c_str(), L"r, ccs=UTF-8") != 0 || file == nullptr) {
    return false;
  }
  wchar_t line[64]{};
  bool found = false;
  while (fgetws(line, 64, file) != nullptr) {
    unsigned int value = 0;
    if (swscanf_s(line, L"state=%u", &value) == 1) {
      state = value;
      found = true;
    }
  }
  fclose(file);
  return found;
}

void TaskbarController::DeleteGuard() {
  const std::wstring path = GuardPath();
  if (!path.empty()) {
    DeleteFileW(path.c_str());
  }
}

bool TaskbarController::ApplyAutoHide() {
  HWND tray = PrimaryTray();
  if (tray == nullptr) {
    return false;
  }
  APPBARDATA abd{};
  abd.cbSize = sizeof(abd);
  abd.hWnd = tray;
  original_state_ = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));

  abd.lParam = (original_state_ & ABS_ALWAYSONTOP) | ABS_AUTOHIDE;
  SHAppBarMessage(ABM_SETSTATE, &abd);

  const UINT now = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
  return (now & ABS_AUTOHIDE) != 0;
}

bool TaskbarController::HideTrayWindows() {
  TraySuppressGuard suppress;
  static_cast<void>(suppress);
  bool any = false;
  bool still_visible = false;
  EnumTrays([&](HWND hwnd) {
    any = true;
    RECT rc{};
    GetWindowRect(hwnd, &rc);
    if (rc.top < kParkY / 2) {
      RememberTray(hwnd, rc);
    }
    ShowWindow(hwnd, SW_HIDE);
    SetWindowPos(hwnd, nullptr, rc.left, kParkY, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_NOSENDCHANGING);
    if (IsWindowVisible(hwnd)) {
      still_visible = true;
    }
  });
  return any && !still_visible;
}

void TaskbarController::ShowTrayWindows() {
  TraySuppressGuard suppress;
  static_cast<void>(suppress);
  EnumTrays([](HWND hwnd) {
    RECT rc{};
    if (LookupTray(hwnd, rc)) {
      SetWindowPos(hwnd, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top,
                   SWP_NOZORDER | SWP_NOACTIVATE);
    } else if (GetWindowRect(hwnd, &rc) != FALSE && rc.top >= kParkY / 2) {
      MONITORINFO info{};
      info.cbSize = sizeof(info);
      if (GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTOPRIMARY), &info) != FALSE) {
        const int height = (std::max)(40, static_cast<int>(rc.bottom - rc.top));
        SetWindowPos(hwnd, nullptr, info.rcMonitor.left, info.rcMonitor.bottom - height,
                     info.rcMonitor.right - info.rcMonitor.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
      }
    }
    ShowWindow(hwnd, SW_SHOWNA);
  });
}

bool TaskbarController::Hide() {
  warning_.clear();
  if (hidden_) {
    return true;
  }

  const HWND tray = PrimaryTray();
  if (tray == nullptr) {
    warning_ = L"태스크바를 찾지 못했습니다";
    Log(L"tray", L"hide failed: tray not found");
    return false;
  }

  const bool autohide = ApplyAutoHide();
  const bool hidden_windows = HideTrayWindows();
  if (!autohide && !hidden_windows) {
    warning_ = L"태스크바를 숨기지 못했습니다";
    Log(L"tray", L"hide failed autohide=%d windows=%d", autohide ? 1 : 0, hidden_windows ? 1 : 0);
    return false;
  }

  hidden_ = true;
  WriteGuard();
  Log(L"tray", L"hidden autohide=%d windows=%d guard=%s", autohide ? 1 : 0, hidden_windows ? 1 : 0,
      GuardPath().c_str());
  if (!hidden_windows && autohide) {
    // Auto-hide still peeks at the edge, but work area is released. Not a hard failure.
  }
  return true;
}

void TaskbarController::Restore() {
  if (!hidden_) {
    UINT leftover = 0;
    if (ReadGuard(leftover)) {
      original_state_ = leftover;
      ShowTrayWindows();
      if (HWND tray = PrimaryTray()) {
        APPBARDATA abd{};
        abd.cbSize = sizeof(abd);
        abd.hWnd = tray;
        abd.lParam = original_state_;
        SHAppBarMessage(ABM_SETSTATE, &abd);
      }
      DeleteGuard();
    }
    return;
  }

  ShowTrayWindows();
  if (HWND tray = PrimaryTray()) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = tray;
    abd.lParam = original_state_;
    SHAppBarMessage(ABM_SETSTATE, &abd);
  }
  hidden_ = false;
  warning_.clear();
  DeleteGuard();
  Log(L"tray", L"restored");
}

void TaskbarController::BeginPeek(UINT ms) {
  g_peek_until = GetTickCount64() + ms;
  Log(L"tray", L"peek begin ms=%u", ms);
}

void TaskbarController::EndPeek() {
  g_peek_until = 0;
  Log(L"tray", L"peek end");
}

bool TaskbarController::Peeking() {
  if (g_peek_until == 0) {
    return false;
  }
  if (GetTickCount64() >= g_peek_until) {
    g_peek_until = 0;
    return false;
  }
  return true;
}

void TaskbarController::EnsureHidden() {
  if (Peeking() || !hidden_) {
    return;
  }
  if (HWND tray = PrimaryTray()) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = tray;
    const UINT now = static_cast<UINT>(SHAppBarMessage(ABM_GETSTATE, &abd));
    if ((now & ABS_AUTOHIDE) == 0) {
      abd.lParam = (original_state_ & ABS_ALWAYSONTOP) | ABS_AUTOHIDE;
      SHAppBarMessage(ABM_SETSTATE, &abd);
    }
  }
  Rehide();
}

bool TaskbarController::Rehide() {
  if (Peeking()) {
    return false;
  }
  bool need = false;
  EnumTrays([&](HWND hwnd) {
    if (IsWindowVisible(hwnd) != FALSE) {
      need = true;
      return;
    }
    RECT rc{};
    if (GetWindowRect(hwnd, &rc) != FALSE && rc.top < kParkY / 2) {
      need = true;
    }
  });
  if (need) {
    HideTrayWindows();
  }
  return need;
}

bool TaskbarController::SuppressingTrayEvents() {
  return g_tray_suppress_depth > 0 || GetTickCount64() < g_tray_suppress_until;
}

void TaskbarController::ForceRestore() {
  UINT state = 0;
  const bool had_guard = ReadGuard(state);
  ShowTrayWindows();
  if (had_guard) {
    if (HWND tray = FindExplorerShellTrayWnd()) {
      APPBARDATA abd{};
      abd.cbSize = sizeof(abd);
      abd.hWnd = tray;
      abd.lParam = state;
      SHAppBarMessage(ABM_SETSTATE, &abd);
    }
  }
  DeleteGuard();
}

bool TaskbarController::WatchTray(WINEVENTPROC proc) {
  UnwatchTray();
  g_tray_proc = proc;
  HWND tray = PrimaryTray();
  if (tray == nullptr || proc == nullptr) {
    return false;
  }
  DWORD pid = 0;
  const DWORD tid = GetWindowThreadProcessId(tray, &pid);
  if (tid == 0) {
    return false;
  }
  g_tray_hwnd = tray;
  bool all = true;
  for (int i = 0; i < 3; ++i) {
    g_tray_hooks[i] = SetWinEventHook(kTrayEvents[i], kTrayEvents[i], nullptr, proc, pid, tid, WINEVENT_OUTOFCONTEXT);
    if (g_tray_hooks[i] == nullptr) {
      all = false;
    }
  }
  return all;
}

void TaskbarController::UnwatchTray() {
  for (HWINEVENTHOOK& hook : g_tray_hooks) {
    if (hook != nullptr) {
      UnhookWinEvent(hook);
      hook = nullptr;
    }
  }
  g_tray_hwnd = nullptr;
}

HWND TaskbarController::WatchedTray() {
  return g_tray_hwnd;
}

bool TaskbarController::RewatchTray() {
  return g_tray_proc != nullptr && WatchTray(g_tray_proc);
}

}  // namespace bamti
