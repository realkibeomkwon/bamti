#pragma once

#include <d2d1.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

namespace bamti {

inline constexpr wchar_t kDockLabelClass[] = L"bamti.DockLabel";

class DockLabel {
 public:
  DockLabel() = default;
  DockLabel(const DockLabel&) = delete;
  DockLabel& operator=(const DockLabel&) = delete;
  ~DockLabel();

  bool Create(HINSTANCE instance, HWND owner);
  void Destroy();
  void Show(const std::wstring& text, POINT icon_center_screen, int dock_top, bool dark);
  void Hide();
  bool IsShown() const { return shown_; }
  HWND hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam);
  LRESULT Handle(UINT msg, WPARAM wparam, LPARAM lparam);
  void EnsureLayeredTarget();
  void ReleaseLayeredTarget();
  void Present();
  UINT Dpi() const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  bool shown_ = false;
  bool dark_ = true;
  std::wstring text_;
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
  HDC mem_dc_ = nullptr;
  HBITMAP dib_ = nullptr;
  HGDIOBJ old_dib_ = nullptr;
  int dib_w_ = 0;
  int dib_h_ = 0;
};

}  // namespace bamti
