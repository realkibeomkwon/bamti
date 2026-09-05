#pragma once

#include <windows.h>

#include <functional>
#include <string>

namespace bamti {

class WifiPasswordPrompt {
 public:
  using SubmitFn = std::function<void(std::wstring)>;
  using CancelFn = std::function<void()>;

  WifiPasswordPrompt() = default;
  WifiPasswordPrompt(const WifiPasswordPrompt&) = delete;
  WifiPasswordPrompt& operator=(const WifiPasswordPrompt&) = delete;
  ~WifiPasswordPrompt();

  void SetCallbacks(SubmitFn on_submit, CancelFn on_cancel);
  bool Show(HWND owner, RECT screen_row, UINT dpi, bool dark, const std::wstring& error);
  void Hide();
  void FocusEdit();
  bool visible() const { return visible_; }
  HWND hwnd() const { return hwnd_; }

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK EditProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  bool EnsureWindow(HWND owner);
  void Layout();
  void ApplyRegion();
  void ApplyChrome();
  void Submit();
  void Cancel();
  void ClearEdit();
  void ApplyMasked();
  void ToggleReveal();
  void SetEyeHot(bool hot);
  void DrawEye(HDC dc, HWND origin) const;
  void Raise();
  bool HitEye(POINT parent_pt) const;
  POINT ToParent(HWND from, LPARAM lp) const;
  RECT RowRect() const;
  RECT ErrorRect() const;
  RECT LabelRect() const;
  RECT EyeRect() const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND edit_ = nullptr;
  WNDPROC edit_prev_ = nullptr;
  HFONT font_ = nullptr;
  HFONT icon_font_ = nullptr;
  HBRUSH bg_brush_ = nullptr;
  UINT dpi_ = 96;
  bool dark_ = true;
  bool visible_ = false;
  bool revealed_ = false;
  bool fluent_icons_ = false;
  bool eye_hot_ = false;
  wchar_t mask_char_ = 0;
  int label_w_ = 0;
  RECT screen_row_{};
  std::wstring error_;
  SubmitFn on_submit_;
  CancelFn on_cancel_;
};

}  // namespace bamti
