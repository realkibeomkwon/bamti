#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>
#include <wrl/client.h>

#include <string>

namespace bamti {

inline constexpr wchar_t kPopupClass[] = L"bamti.PopupSurface";
inline constexpr UINT kPopupClosedMsg = WM_APP + 30;

class PopupContent {
 public:
  virtual ~PopupContent() = default;
  virtual SIZE Measure(UINT dpi) = 0;
  virtual void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) = 0;
  virtual int HitTest(POINT client, UINT dpi) const = 0;
  virtual void Invoke(int index) = 0;
  virtual int RowCount() const { return 0; }
};

float PopupTextWidth(UINT dpi, const std::wstring& text);
void DrawPopupText(ID2D1RenderTarget* target, UINT dpi, const std::wstring& text, const D2D1_RECT_F& rect,
                   ID2D1Brush* brush);

class PopupSurface {
 public:
  enum class Anchor { AboveAt, BelowAt };

  enum class DismissReason {
    kInvoke,
    kOutsideClick,
    kOutsidePoll,
    kCaptureLost,
    kCaptureGone,
    kEscape,
    kWinKey,
    kForeground,
    kReopen,
    kExplicit,
  };

  PopupSurface() = default;
  PopupSurface(const PopupSurface&) = delete;
  PopupSurface& operator=(const PopupSurface&) = delete;
  ~PopupSurface();

  bool Create(HINSTANCE instance, HWND owner);
  void Destroy();

  bool Open(PopupContent* content, POINT anchor_screen, Anchor mode);
  void Close();
  bool IsOpen() const { return open_; }
  HWND hwnd() const { return hwnd_; }

  void SetDark(bool dark);

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  void Dismiss(int invoke_index, DismissReason reason);
  void EnsureRenderTarget();
  void Render();
  void Place(SIZE size, POINT anchor_screen, Anchor mode);
  void ApplyChrome();
  void ArmGuardTimer();
  void OnGuardTimer();
  UINT Dpi() const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND last_fg_ = nullptr;
  PopupContent* content_ = nullptr;
  bool open_ = false;
  bool esc_down_ = false;
  bool win_down_ = false;
  bool mouse_down_ = false;
  bool press_inside_ = false;
  bool saw_mousemove_ = false;
  bool dark_ = true;
  int hot_ = -1;
  Anchor mode_ = Anchor::AboveAt;
  POINT anchor_{};
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<ID2D1HwndRenderTarget> target_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_;
};

}  // namespace bamti
