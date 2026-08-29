#pragma once

#include <string>
#include <windows.h>

namespace bamti {

// explorer의 Shell_TrayWnd. 우리 프로세스의 스파이 창은 건너뛴다.
HWND FindExplorerShellTrayWnd();

class TaskbarController {
 public:
  TaskbarController() = default;
  TaskbarController(const TaskbarController&) = delete;
  TaskbarController& operator=(const TaskbarController&) = delete;
  ~TaskbarController();

  bool Hide();
  void Restore();
  void EnsureHidden();
  static bool Rehide();
  static void BeginPeek(UINT ms);
  static void EndPeek();
  static bool Peeking();
  static bool SuppressingTrayEvents();
  static void ForceRestore();
  static bool WatchTray(WINEVENTPROC proc);
  static void UnwatchTray();
  static bool RewatchTray();
  static HWND WatchedTray();

  bool hidden() const { return hidden_; }
  const std::wstring& warning() const { return warning_; }

 private:
  bool ApplyAutoHide();
  static bool HideTrayWindows();
  static void ShowTrayWindows();
  bool WriteGuard() const;
  static bool ReadGuard(UINT& state);
  static void DeleteGuard();
  static std::wstring GuardPath();
  static HWND PrimaryTray();

  bool hidden_ = false;
  UINT original_state_ = 0;
  std::wstring warning_;
};

}  // namespace bamti
