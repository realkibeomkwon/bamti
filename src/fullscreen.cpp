#include "fullscreen.hpp"

#include "watchdog.hpp"

#include <dwmapi.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace bamti {
namespace {

bool RectCovers(const RECT& window, const RECT& monitor, int slop) {
  return window.left <= monitor.left + slop && window.top <= monitor.top + slop &&
         window.right >= monitor.right - slop && window.bottom >= monitor.bottom - slop;
}

int FrameSlopPx(HWND hwnd) {
  UINT dpi = GetDpiForWindow(hwnd);
  if (dpi == 0) {
    dpi = 96;
  }
  const int frame = GetSystemMetricsForDpi(SM_CXSIZEFRAME, dpi);
  const int pad = GetSystemMetricsForDpi(SM_CXPADDEDBORDER, dpi);
  const int slop = frame + pad;
  return slop < 8 ? 8 : slop;
}

bool IsShellForegroundClass(HWND hwnd) {
  wchar_t cls[256]{};
  GetClassNameW(hwnd, cls, 256);
  if (cls[0] != L'\0' && _wcsnicmp(cls, L"bamti.", 6) == 0) {
    return true;
  }
  static const wchar_t* kSkip[] = {
      L"Progman",
      L"WorkerW",
      L"Shell_TrayWnd",
      L"Shell_SecondaryTrayWnd",
      L"NotifyIconOverflowWindow",
      L"ForegroundStaging",
      L"Windows.UI.Core.CoreWindow",
      L"Xaml_WindowedPopupClass",
  };
  for (const wchar_t* skip : kSkip) {
    if (lstrcmpiW(cls, skip) == 0) {
      return true;
    }
  }
  return false;
}

bool HasCaptionFrame(LONG style) {
  return (style & WS_CAPTION) == WS_CAPTION;
}

struct Watcher {
  HWND hwnd = nullptr;
  UINT msg = 0;
};

std::vector<Watcher> g_watchers;
HWINEVENTHOOK g_fg_hook = nullptr;
Microsoft::WRL::ComPtr<IAppVisibility> g_app_vis;
DWORD g_advise_cookie = 0;

void NotifyWatchers() {
  for (const Watcher& w : g_watchers) {
    if (w.hwnd != nullptr && IsWindow(w.hwnd)) {
      PostMessageW(w.hwnd, w.msg, 0, 0);
    }
  }
}

void CALLBACK ForegroundProc(HWINEVENTHOOK, DWORD, HWND hwnd, LONG object, LONG child, DWORD, DWORD) {
  if (object != OBJID_WINDOW || child != CHILDID_SELF || hwnd == nullptr) {
    return;
  }
  NotifyWatchers();
}

class AppVisSink final : public IAppVisibilityEvents {
 public:
  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }
  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&ref_);
    if (n == 0) {
      delete this;
    }
    return static_cast<ULONG>(n);
  }
  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == IID_IAppVisibilityEvents) {
      *ppv = static_cast<IAppVisibilityEvents*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }
  HRESULT STDMETHODCALLTYPE AppVisibilityOnMonitorChanged(HMONITOR, MONITOR_APP_VISIBILITY,
                                                          MONITOR_APP_VISIBILITY) override {
    NotifyWatchers();
    return S_OK;
  }
  HRESULT STDMETHODCALLTYPE LauncherVisibilityChange(BOOL) override {
    NotifyWatchers();
    return S_OK;
  }

 private:
  LONG ref_ = 1;
};

Microsoft::WRL::ComPtr<IAppVisibilityEvents> g_sink;

void EnsureWatchInfra() {
  if (g_fg_hook == nullptr) {
    const DWORD flags = WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS | WINEVENT_SKIPOWNTHREAD;
    g_fg_hook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, ForegroundProc, 0, 0, flags);
  }
  if (!g_app_vis) {
    CoCreateInstance(CLSID_AppVisibility, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(g_app_vis.ReleaseAndGetAddressOf()));
  }
  if (g_app_vis && g_advise_cookie == 0) {
    auto* sink = new AppVisSink();
    g_sink = sink;
    sink->Release();
    if (FAILED(g_app_vis->Advise(g_sink.Get(), &g_advise_cookie))) {
      g_advise_cookie = 0;
      g_sink.Reset();
    }
  }
}

void TearWatchInfra() {
  if (g_app_vis && g_advise_cookie != 0) {
    g_app_vis->Unadvise(g_advise_cookie);
    g_advise_cookie = 0;
  }
  g_sink.Reset();
  g_app_vis.Reset();
  if (g_fg_hook != nullptr) {
    UnhookWinEvent(g_fg_hook);
    g_fg_hook = nullptr;
  }
}

bool ImmersiveAppVisible(HWND self) {
  if (!g_app_vis) {
    return false;
  }
  HWND origin = self != nullptr ? self : GetDesktopWindow();
  const HMONITOR monitor = MonitorFromWindow(origin, MONITOR_DEFAULTTOPRIMARY);
  MONITOR_APP_VISIBILITY mode = MAV_UNKNOWN;
  if (FAILED(g_app_vis->GetAppVisibilityOnMonitor(monitor, &mode))) {
    return false;
  }
  return mode == MAV_APP_VISIBLE;
}

}  // namespace

bool IsTrueFullscreen(HWND self) {
  WatchdogStage(L"fullscreen");
  QUERY_USER_NOTIFICATION_STATE state{};
  if (SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN) {
    return true;
  }
  if (ImmersiveAppVisible(self)) {
    return true;
  }

  const HWND fg = GetForegroundWindow();
  if (fg == nullptr || fg == self) {
    return false;
  }
  if (fg == GetDesktopWindow() || fg == GetShellWindow()) {
    return false;
  }

  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  if (pid == GetCurrentProcessId()) {
    return false;
  }

  if (!IsWindowVisible(fg) || IsIconic(fg)) {
    return false;
  }
  if (IsShellForegroundClass(fg)) {
    return false;
  }

  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(fg, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0) {
    return false;
  }

  const LONG style = GetWindowLongW(fg, GWL_STYLE);
  if ((style & WS_MINIMIZE) != 0) {
    return false;
  }

  RECT wr{};
  if (!GetWindowRect(fg, &wr)) {
    return false;
  }
  MONITORINFO mi{};
  mi.cbSize = sizeof(mi);
  const HMONITOR monitor = MonitorFromWindow(fg, MONITOR_DEFAULTTONEAREST);
  if (!GetMonitorInfoW(monitor, &mi)) {
    return false;
  }
  if (!RectCovers(wr, mi.rcMonitor, FrameSlopPx(fg))) {
    return false;
  }

  const LONG ex = GetWindowLongW(fg, GWL_EXSTYLE);
  if ((ex & WS_EX_NOACTIVATE) != 0 || (ex & WS_EX_TOOLWINDOW) != 0) {
    return false;
  }

  return !HasCaptionFrame(style);
}

bool StartFullscreenWatch(HWND notify, UINT msg) {
  if (notify == nullptr || msg == 0) {
    return false;
  }
  for (Watcher& w : g_watchers) {
    if (w.hwnd == notify) {
      w.msg = msg;
      EnsureWatchInfra();
      return true;
    }
  }
  g_watchers.push_back(Watcher{notify, msg});
  EnsureWatchInfra();
  return true;
}

void StopFullscreenWatch(HWND notify) {
  g_watchers.erase(std::remove_if(g_watchers.begin(), g_watchers.end(),
                                  [notify](const Watcher& w) { return w.hwnd == notify; }),
                   g_watchers.end());
  if (g_watchers.empty()) {
    TearWatchInfra();
  }
}

}  // namespace bamti
