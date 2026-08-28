#pragma once

#include "popup_surface.hpp"
#include "task_list.hpp"

#include <d2d1.h>
#include <windows.h>
#include <wrl/client.h>

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace bamti {

inline constexpr wchar_t kDockClass[] = L"bamti.Dock";
inline constexpr wchar_t kDockHotClass[] = L"bamti.DockHot";

class DockMenuContent;
class DockSubmenuContent;

class Dock {
 public:
  Dock();
  Dock(const Dock&) = delete;
  Dock& operator=(const Dock&) = delete;
  ~Dock();

  bool Create(HINSTANCE instance);

 private:
  friend class DockMenuContent;
  friend class DockSubmenuContent;

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
  void ReleaseLayeredTarget();
  bool EnsureLayeredTarget(int width, int height);
  void ResetD2dIcons();
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
  void SyncOptionsSubmenu();
  void OpenOptionsSubmenu();
  void CloseOptionsSubmenu(const wchar_t* reason);
  static void AfterPopupTick(void* ctx);
  void ScheduleRebuild();
  void ArmMouseLeave();
  void ArmHotMouseLeave();
  void UpdateIdleTimer();
  void SetOverlaysTopmost(bool topmost);
  void SanitizePins();
  void BeginDragIfNeeded(POINT client);
  void UpdateDrag(POINT client);
  void EndDrag(bool commit);
  void TickDragAnim();
  void StartDragAnimTimer();
  void StopDragAnimTimer(bool log);
  void SnapAnimX();
  float SlotIconX(size_t slot) const;
  bool NoteDragLog();
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
  int hover_ = -1;
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
  std::map<std::wstring, Microsoft::WRL::ComPtr<ID2D1Bitmap>> d2d_icons_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> layered_rt_;
  HBITMAP layered_dib_ = nullptr;
  HDC layered_mem_ = nullptr;
  HGDIOBJ layered_old_ = nullptr;
  int layered_w_ = 0;
  int layered_h_ = 0;
  std::wstring tooltip_text_;
  std::vector<HWINEVENTHOOK> hooks_;
  PopupSurface popup_;
  PopupSurface submenu_;
  std::unique_ptr<DockMenuContent> menu_content_;
  std::unique_ptr<DockSubmenuContent> submenu_content_;
  std::wstring last_collect_snap_;
  uint64_t last_window_fp_ = 0;
  ULONGLONG last_menu_open_ = 0;
  ULONGLONG last_popup_tick_ = 0;
  UINT drag_logs_ = 0;
  UINT drag_move_logs_ = 0;
  std::vector<float> anim_x_;
  ULONGLONG last_anim_tick_ = 0;
  UINT anim_frames_ = 0;
  double anim_ms_sum_ = 0;
  bool anim_timer_on_ = false;
};

}  // namespace bamti
