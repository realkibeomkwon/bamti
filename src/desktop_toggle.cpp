#include "desktop_toggle.hpp"

#include "dwm.hpp"
#include "log.hpp"
#include "task_list.hpp"

#include <dwmapi.h>

namespace bamti {
namespace {

void LogTransitionsBatch(const wchar_t* which, int n, int fail, HRESULT first_fail) {
  if (fail == 0) {
    Log(L"peek", L"transitions %s n=%d fail=0", which, n);
  } else {
    Log(L"peek", L"transitions %s n=%d fail=%d hr=0x%08lx", which, n, fail,
        static_cast<unsigned long>(first_fail));
  }
}

}  // namespace

bool DesktopToggle::Revealed() {
  return CollectDesktopClearWindows().empty();
}

HRESULT DesktopToggle::SetTransitions(HWND hwnd, bool enabled) {
  BOOL disable = enabled ? FALSE : TRUE;
  return DwmSetWindowAttribute(hwnd, dwm::kTransitionsForceDisabled, &disable, sizeof(disable));
}

void DesktopToggle::DisableTransitions(const std::vector<HWND>& windows) {
  int n = 0;
  int fail = 0;
  HRESULT first_fail = S_OK;
  for (HWND hwnd : windows) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
      continue;
    }
    const HRESULT hr = SetTransitions(hwnd, false);
    ++n;
    if (FAILED(hr)) {
      if (fail == 0) {
        first_fail = hr;
      }
      ++fail;
    }
    bool already = false;
    for (HWND kept : transitions_off_) {
      if (kept == hwnd) {
        already = true;
        break;
      }
    }
    if (!already) {
      transitions_off_.push_back(hwnd);
    }
  }
  LogTransitionsBatch(L"off", n, fail, first_fail);
}

void DesktopToggle::RestoreTransitions() {
  if (transitions_off_.empty()) {
    return;
  }
  int n = 0;
  int fail = 0;
  HRESULT first_fail = S_OK;
  for (HWND hwnd : transitions_off_) {
    if (hwnd == nullptr || !IsWindow(hwnd)) {
      continue;
    }
    const HRESULT hr = SetTransitions(hwnd, true);
    ++n;
    if (FAILED(hr)) {
      if (fail == 0) {
        first_fail = hr;
      }
      ++fail;
    }
  }
  transitions_off_.clear();
  LogTransitionsBatch(L"on", n, fail, first_fail);
}

DesktopToggle::~DesktopToggle() {
  RestoreTransitions();
}

void DesktopToggle::Conceal(std::vector<HWND> windows) {
  if (windows.empty()) {
    return;
  }
  DisableTransitions(windows);
  const std::vector<HWND> back_to_front(windows.rbegin(), windows.rend());
  HideHwnds(back_to_front);
  std::vector<HWND> merged;
  merged.reserve(windows.size() + concealed_.size());
  for (HWND hwnd : windows) {
    merged.push_back(hwnd);
  }
  for (HWND hwnd : concealed_) {
    if (!IsWindow(hwnd)) {
      continue;
    }
    bool already = false;
    for (HWND kept : merged) {
      if (kept == hwnd) {
        already = true;
        break;
      }
    }
    if (!already) {
      merged.push_back(hwnd);
    }
  }
  concealed_ = std::move(merged);
  wchar_t cls[256]{};
  GetClassNameW(concealed_.front(), cls, 256);
  Log(L"peek", L"conceal n=%d total=%d top=%s", static_cast<int>(windows.size()),
      static_cast<int>(concealed_.size()), cls);
}

void DesktopToggle::Reveal() {
  if (concealed_.empty()) {
    return;
  }
  const int n = static_cast<int>(concealed_.size());
  DisableTransitions(concealed_);
  int living = 0;
  for (auto it = concealed_.rbegin(); it != concealed_.rend(); ++it) {
    HWND hwnd = *it;
    if (hwnd == nullptr || !IsWindow(hwnd)) {
      continue;
    }
    if (IsIconic(hwnd)) {
      ShowWindow(hwnd, SW_RESTORE);
    } else if (!IsWindowVisible(hwnd)) {
      ShowWindow(hwnd, SW_SHOW);
    }
    ++living;
  }
  bool ordered = false;
  if (living > 0) {
    HDWP hdwp = BeginDeferWindowPos(living);
    if (hdwp != nullptr) {
      for (auto it = concealed_.rbegin(); it != concealed_.rend(); ++it) {
        HWND hwnd = *it;
        if (hwnd == nullptr || !IsWindow(hwnd)) {
          continue;
        }
        hdwp = DeferWindowPos(hdwp, hwnd, HWND_TOP, 0, 0, 0, 0,
                              SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        if (hdwp == nullptr) {
          break;
        }
      }
      if (hdwp != nullptr) {
        EndDeferWindowPos(hdwp);
        ordered = true;
      }
    }
    if (!ordered) {
      for (auto it = concealed_.rbegin(); it != concealed_.rend(); ++it) {
        HWND hwnd = *it;
        if (hwnd == nullptr || !IsWindow(hwnd)) {
          continue;
        }
        SetWindowPos(hwnd, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
      }
    }
  }
  ActivateHwnd(concealed_.front());
  concealed_.clear();
  Log(L"peek", L"reveal n=%d", n);
}

void DesktopToggle::Toggle() {
  std::vector<HWND> visible = CollectDesktopClearWindows();
  if (visible.empty()) {
    Reveal();
  } else {
    Conceal(std::move(visible));
  }
}

}  // namespace bamti
