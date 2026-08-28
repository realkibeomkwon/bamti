#pragma once

#include "popup_surface.hpp"
#include "task_list.hpp"

#include <windows.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bamti {

inline constexpr wchar_t kDockClass[] = L"bamti.Dock";
inline constexpr wchar_t kDockHotClass[] = L"bamti.DockHot";

class DockMenuContent;

class Dock {
 public:
  Dock();
  Dock(const Dock&) = delete;
  Dock& operator=(const Dock&) = delete;
  ~Dock();

  bool Create(HINSTANCE instance);

 private:
  friend class DockMenuContent;

  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  static LRESULT CALLBACK HotProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  static void CALLBACK WinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                    DWORD thread, DWORD time);
  static void CALLBACK TrayWinEventProc(HWINEVENTHOOK hook, DWORD event, HWND hwnd, LONG object, LONG child,
                                        DWORD thread, DWORD time);

  LRESULT HandleMessage(UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT HandleHot(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);

  bool RegisterClasses(HINSTANCE instance);
  bool CreateTooltip();
  void ApplyBackdrop();
  void Layout();
  void LayoutHot();
  void Paint();
  void RenderLayered();
  void Rebuild();
  void ResetIconCache();
  void EnsureIcons();
  void ShowPill();
  void HidePill();
  void StartHideTimer();
  void CancelHideTimer();
  void PollPointer();
  void RefreshFullscreen();
  void SetFullscreenOccluded(bool occluded);
  void RaiseOverlays();
  void OpenDockMenu(POINT screen, int index);
  void ApplyMenuCommand(UINT cmd, const DockApp& app, const std::vector<HWND>& window_cmds);
  void ScheduleRebuild();
  void ArmMouseLeave();
  void ArmHotMouseLeave();
  void UpdateIdleTimer();
  void SetOverlaysTopmost(bool topmost);
  void SanitizePins();
  void BeginDragIfNeeded(POINT client);
  void UpdateDrag(POINT client);
  void EndDrag(bool commit);
  bool Busy() const;
  int PinnedCount() const;
  int DropIndexAt(POINT client) const;
  std::vector<size_t> DisplayOrder() const;
  int HitTest(POINT client) const;
  bool PointerOverUi() const;
  bool PointerOverHotEdge() const;
  HBITMAP LoadIconBitmap(const DockApp& app, int px);
  UINT Dpi() const;
  int Dip(int value) const;

  HWND hwnd_ = nullptr;
  HWND hot_hwnd_ = nullptr;
  HWND tooltip_ = nullptr;
  bool shown_ = false;
  bool fullscreen_occluded_ = false;
  bool dark_ = true;
  bool hide_armed_ = false;
  bool dragging_ = false;
  bool pending_rebuild_ = false;
  bool force_collect_ = false;
  int pressed_ = -1;
  int drag_index_ = -1;
  int drop_index_ = -1;
  UINT pending_menu_cmd_ = 0;
  DockApp pending_menu_app_{};
  std::vector<HWND> pending_menu_windows_;
  POINT drag_origin_{};
  std::vector<DockApp> items_;
  std::vector<std::wstring> pins_;
  std::vector<RECT> slots_;
  std::vector<HBITMAP> icons_;
  std::map<std::wstring, HBITMAP> icon_cache_;
  std::wstring tooltip_text_;
  std::vector<HWINEVENTHOOK> hooks_;
  PopupSurface popup_;
  std::unique_ptr<DockMenuContent> menu_content_;
  std::wstring last_collect_snap_;
  uint64_t last_window_fp_ = 0;
  ULONGLONG last_menu_open_ = 0;
  ULONGLONG last_popup_tick_ = 0;
};

}  // namespace bamti
