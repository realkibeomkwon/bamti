#include "taskbar_controller.hpp"

#include <knownfolders.h>
#include <shlobj.h>
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

void EnumTrays(const auto& fn) {
  if (HWND primary = FindWindowW(kPrimaryClass, nullptr)) {
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

std::wstring JoinPath(const std::wstring& dir, const wchar_t* file) {
  std::wstring path = dir;
  if (!path.empty() && path.back() != L'\\') {
    path.push_back(L'\\');
  }
  path += file;
  return path;
}

}  // namespace

TaskbarController::~TaskbarController() {
  Restore();
}

HWND TaskbarController::PrimaryTray() {
  return FindWindowW(kPrimaryClass, nullptr);
}

std::wstring TaskbarController::GuardPath() {
  PWSTR root = nullptr;
  if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &root)) || root == nullptr) {
    return {};
  }
  std::wstring dir = root;
  CoTaskMemFree(root);
  dir = JoinPath(dir, L"bamti");
  CreateDirectoryW(dir.c_str(), nullptr);
  return JoinPath(dir, L"taskbar.guard");
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
    return false;
  }

  const bool autohide = ApplyAutoHide();
  const bool hidden_windows = HideTrayWindows();
  if (!autohide && !hidden_windows) {
    warning_ = L"태스크바를 숨기지 못했습니다";
    return false;
  }

  hidden_ = true;
  WriteGuard();
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
}

void TaskbarController::EnsureHidden() {
  if (!hidden_) {
    return;
  }
  if (HWND tray = PrimaryTray()) {
    APPBARDATA abd{};
    abd.cbSize = sizeof(abd);
    abd.hWnd = tray;
    abd.lParam = (original_state_ & ABS_ALWAYSONTOP) | ABS_AUTOHIDE;
    SHAppBarMessage(ABM_SETSTATE, &abd);
  }
  HideTrayWindows();
}

bool TaskbarController::Rehide() {
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

void TaskbarController::ForceRestore() {
  UINT state = 0;
  const bool had_guard = ReadGuard(state);
  ShowTrayWindows();
  if (had_guard) {
    if (HWND tray = FindWindowW(kPrimaryClass, nullptr)) {
      APPBARDATA abd{};
      abd.cbSize = sizeof(abd);
      abd.hWnd = tray;
      abd.lParam = state;
      SHAppBarMessage(ABM_SETSTATE, &abd);
    }
  }
  DeleteGuard();
}

}  // namespace bamti
