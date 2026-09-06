#include "desktop_toggle.hpp"

#include "log.hpp"
#include "task_list.hpp"

namespace bamti {

bool DesktopToggle::Revealed() {
  return CollectDesktopClearWindows().empty();
}

void DesktopToggle::Conceal() {
  std::vector<HWND> windows = CollectDesktopClearWindows();
  if (windows.empty()) {
    return;
  }
  concealed_ = std::move(windows);
  const std::vector<HWND> back_to_front(concealed_.rbegin(), concealed_.rend());
  HideHwnds(back_to_front);
  wchar_t cls[256]{};
  GetClassNameW(concealed_.front(), cls, 256);
  Log(L"peek", L"conceal n=%d top=%s", static_cast<int>(concealed_.size()), cls);
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
  if (Revealed()) {
    Reveal();
  } else {
    Conceal();
  }
}

}  // namespace bamti
