#include "desktop_toggle.hpp"

#include "log.hpp"
#include "task_list.hpp"

namespace bamti {

bool DesktopToggle::Revealed() {
  return CollectDesktopClearWindows().empty();
}

void DesktopToggle::Conceal(std::vector<HWND> windows) {
  if (windows.empty()) {
    return;
  }
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
  RestoreHwnds(concealed_);
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
