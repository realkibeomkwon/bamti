#include "taskbar_controller.hpp"

#include <knownfolders.h>
#include <shlobj.h>
#include <shellapi.h>

#include <cstdio>
#include <cwchar>
#include <vector>

namespace bamti {
namespace {

constexpr wchar_t kPrimaryClass[] = L"Shell_TrayWnd";
constexpr wchar_t kSecondaryClass[] = L"Shell_SecondaryTrayWnd";

void EnumTrays(const auto& fn) {
  if (HWND primary = FindWindowW(kPrimaryClass, nullptr)) {
    fn(primary);
  }
  HWND secondary = nullptr;
  while ((secondary = FindWindowExW(nullptr, secondary, kSecondaryClass, nullptr)) != nullptr) {
    fn(secondary);
  }
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
    ShowWindow(hwnd, SW_HIDE);
    if (IsWindowVisible(hwnd)) {
      still_visible = true;
    }
  });
  return any && !still_visible;
}

void TaskbarController::ShowTrayWindows() {
  EnumTrays([](HWND hwnd) { ShowWindow(hwnd, SW_SHOWNA); });
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
