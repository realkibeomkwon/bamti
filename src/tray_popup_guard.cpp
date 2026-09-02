#include "tray_popup_guard.hpp"

#include "log.hpp"

#include <algorithm>

namespace bamti {
namespace {
constexpr ULONGLONG kArmMs = 2000;
constexpr UINT kTimerMs = 250;
constexpr int kMaxSkipLogs = 5;

ULONGLONG armed_until = 0;
RECT anchor{};
DWORD owner_pid = 0;
HWINEVENTHOOK hook_show = nullptr;
HWINEVENTHOOK hook_menu = nullptr;
UINT_PTR timer_id = 0;
int moved = 0;
int skip_logs = 0;

void ClassName(HWND hwnd, wchar_t (&out)[256]) {
  if (hwnd == nullptr || GetClassNameW(hwnd, out, 256) <= 0) {
    out[0] = L'?';
    out[1] = L'\0';
  }
}

void Disarm() {
  if (hook_show != nullptr) {
    UnhookWinEvent(hook_show);
    hook_show = nullptr;
  }
  if (hook_menu != nullptr) {
    UnhookWinEvent(hook_menu);
    hook_menu = nullptr;
  }
  if (timer_id != 0) {
    KillTimer(nullptr, timer_id);
    timer_id = 0;
  }
  armed_until = 0;
}

bool Armed() {
  return GetTickCount64() < armed_until && moved == 0;
}

void LogSkip(HWND hwnd, DWORD pid, const wchar_t* reason, const RECT& rc) {
  if (skip_logs >= kMaxSkipLogs) {
    return;
  }
  ++skip_logs;
  wchar_t cls[256];
  ClassName(hwnd, cls);
  Log(L"tray", L"popup skip cls=%s pid=%lu reason=%s rc=%ld,%ld,%ld,%ld", cls, static_cast<unsigned long>(pid), reason,
      rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top);
}

void MovePopup(HWND hwnd, const RECT& rc, DWORD pid) {
  MONITORINFO mi{sizeof(mi)};
  const HMONITOR mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  GetMonitorInfoW(mon, &mi);

  const LONG w = rc.right - rc.left;
  const LONG h = rc.bottom - rc.top;
  const LONG gap = MulDiv(6, static_cast<int>(GetDpiForSystem()), 96);  // 6 DIP
  LONG x = (anchor.left + anchor.right) / 2 - w / 2;
  LONG y = anchor.bottom + gap;
  x = (std::max)(mi.rcWork.left, (std::min)(x, mi.rcWork.right - w));
  y = (std::max)(mi.rcWork.top, (std::min)(y, mi.rcWork.bottom - h));
  SetWindowPos(hwnd, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  wchar_t cls[256];
  ClassName(hwnd, cls);
  Log(L"tray", L"popup move cls=%s pid=%lu from=%ld,%ld,%ld,%ld to=%ld,%ld", cls, static_cast<unsigned long>(pid),
      rc.left, rc.top, w, h, x, y);
  ++moved;
  Disarm();
}

void CALLBACK GuardProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG idObject, LONG idChild, DWORD, DWORD) {
  if (idObject != OBJID_WINDOW || idChild != CHILDID_SELF || hwnd == nullptr) {
    return;
  }
  if (!Armed()) {
    return;
  }

  RECT rc{};
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);

  if (GetAncestor(hwnd, GA_ROOT) != hwnd) {
    LogSkip(hwnd, pid, L"not-toplevel", rc);
    return;
  }
  if (IsWindowVisible(hwnd) == FALSE) {
    LogSkip(hwnd, pid, L"hidden", rc);
    return;
  }
  if (GetWindowRect(hwnd, &rc) == FALSE) {
    return;
  }
  const LONG w = rc.right - rc.left;
  const LONG h = rc.bottom - rc.top;
  if (w <= 0 || h <= 0) {
    return;
  }
  if (owner_pid != 0 && pid != owner_pid) {
    LogSkip(hwnd, pid, L"pid", rc);
    return;
  }

  MONITORINFO mi{sizeof(mi)};
  const HMONITOR win_mon = MonitorFromRect(&rc, MONITOR_DEFAULTTONEAREST);
  if (GetMonitorInfoW(win_mon, &mi) != FALSE) {
    const LONG work_w = mi.rcWork.right - mi.rcWork.left;
    const LONG work_h = mi.rcWork.bottom - mi.rcWork.top;
    if ((work_w > 0 && w > MulDiv(work_w, 90, 100)) || (work_h > 0 && h > MulDiv(work_h, 90, 100))) {
      LogSkip(hwnd, pid, L"too-big", rc);
      return;
    }
  }

  MONITORINFO ami{sizeof(ami)};
  const HMONITOR anchor_mon = MonitorFromRect(&anchor, MONITOR_DEFAULTTONEAREST);
  if (GetMonitorInfoW(anchor_mon, &ami) == FALSE) {
    return;
  }
  const LONG work_mid_y = (ami.rcWork.top + ami.rcWork.bottom) / 2;
  const LONG win_mid_y = (rc.top + rc.bottom) / 2;
  if (win_mid_y <= work_mid_y) {
    LogSkip(hwnd, pid, L"not-bottom", rc);
    return;
  }

  MovePopup(hwnd, rc, pid);
}

void CALLBACK GuardTimerProc(HWND, UINT, UINT_PTR id, DWORD) {
  if (id != timer_id) {
    return;
  }
  if (!Armed()) {
    Disarm();
  }
}

}  // namespace

void TrayPopupGuardArm(const RECT& next_anchor, DWORD next_pid) {
  anchor = next_anchor;
  owner_pid = next_pid;
  armed_until = GetTickCount64() + kArmMs;
  moved = 0;
  skip_logs = 0;
  if (hook_show == nullptr) {
    hook_show = SetWinEventHook(EVENT_OBJECT_SHOW, EVENT_OBJECT_SHOW, nullptr, &GuardProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (hook_menu == nullptr) {
    hook_menu = SetWinEventHook(EVENT_SYSTEM_MENUPOPUPSTART, EVENT_SYSTEM_MENUPOPUPSTART, nullptr, &GuardProc, 0, 0,
                                WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  }
  if (timer_id == 0) {
    timer_id = SetTimer(nullptr, 0, kTimerMs, &GuardTimerProc);
  }
}

void TrayPopupGuardShutdown() {
  Disarm();
}

}  // namespace bamti
