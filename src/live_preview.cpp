#include "live_preview.hpp"

#include "log.hpp"

namespace bamti {
namespace {

using Fn5 = HRESULT(WINAPI*)(BOOL activate, HWND exclude, HWND insert_before, UINT trigger, RECT* final_rect);

constexpr UINT kShowDesktopTrigger = 1;

Fn5 Resolve() {
  static Fn5 fn = nullptr;
  static bool tried = false;
  if (!tried) {
    tried = true;
    HMODULE dwm = GetModuleHandleW(L"dwmapi.dll");
    if (dwm == nullptr) {
      Log(L"peek", L"dwmapi.dll not loaded");
    } else {
      fn = reinterpret_cast<Fn5>(GetProcAddress(dwm, MAKEINTRESOURCEA(113)));
      Log(L"peek", L"fn113=%p", fn);
    }
  }
  return fn;
}

}  // namespace

bool LivePreviewAvailable() {
  return Resolve() != nullptr;
}

void SetLivePreview(bool on, HWND exclude) {
  Fn5 fn = Resolve();
  if (fn == nullptr) {
    return;
  }
  const HRESULT hr = fn(on ? TRUE : FALSE, exclude, nullptr, kShowDesktopTrigger, nullptr);
  Log(L"peek", L"set on=%d hr=0x%08lX", on ? 1 : 0, static_cast<unsigned long>(hr));
}

}  // namespace bamti
