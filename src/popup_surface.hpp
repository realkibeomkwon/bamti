#pragma once

#include "corner.hpp"

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
  virtual bool StickyRow(int /*index*/) const { return false; }
  virtual void StickyInvoke(int /*index*/) {}
  virtual bool DragRow(int /*index*/) const { return false; }
  virtual void DragTo(int /*index*/, POINT /*client*/, UINT /*dpi*/) {}
  virtual void DragEnd(int /*index*/) {}
  virtual bool Selectable(int /*index*/) const { return true; }
  // 참이면 PopupSurface가 배경 둥근 사각형을 그리지 않는다. 내용이 카드 여러 장을 직접 그린다.
  virtual bool PaintsOwnChrome() const { return false; }
  // 이 콘텐츠를 담는 창의 곡률 계층. 기본은 떠 있는 창이다.
  // 초점 표면으로 올리려면 corner::kHeroDip 을 돌려주면 된다.
  virtual int CornerDip() const { return corner::kOverlayDip; }
};

float PopupTextWidth(UINT dpi, const std::wstring& text);
void DrawPopupText(ID2D1RenderTarget* target, UINT dpi, const std::wstring& text, const D2D1_RECT_F& rect,
                   ID2D1Brush* brush, DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING);

class PopupSurface {
 public:
  // AboveCenter 는 앵커의 가로 중심에 메뉴의 가운데를 맞추고 앵커 위에 놓는다.
  enum class Anchor { AboveCenter, BelowAt, RightOf };

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

  bool Open(PopupContent* content, POINT anchor_screen, Anchor mode, bool capture = true);
  void Close();
  bool IsOpen() const { return open_; }
  bool Dragging() const { return drag_index_ >= 0; }
  HWND hwnd() const { return hwnd_; }
  int Hot() const { return hot_; }

  void SetDark(bool dark);
  void Present();
  void SetAllied(PopupSurface* allied) { allied_ = allied; }
  void SetAlliedHwnd(HWND hwnd);
  void SetAfterTick(void (*fn)(void*), void* ctx) {
    after_tick_ = fn;
    after_tick_ctx_ = ctx;
  }
  int HitTestScreen(POINT screen) const;
  void TrackHotScreen(POINT screen);
  void InvokeRow(int index);
  void SetHot(int index);
  void Tick();

 private:
  static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
  LRESULT Handle(UINT msg, WPARAM wp, LPARAM lp);
  void Dismiss(int invoke_index, DismissReason reason);
  void EnsureLayeredTarget();
  void ReleaseLayeredTarget();
  void Render();
  void Place(SIZE size, POINT anchor_screen, Anchor mode);
  void ApplyChrome();
  void ArmGuardTimer();
  void Tick(const wchar_t* src);
  void MoveHot(int delta);
  void HitTree(POINT screen, bool* in_self, bool* in_allied) const;
  bool AlliedHwndAlive() const;
  bool PointInAlliedHwnd(POINT screen) const;
  bool IsAlliedHwnd(HWND hwnd) const;
  UINT Dpi() const;

  HWND hwnd_ = nullptr;
  HWND owner_ = nullptr;
  HWND last_fg_ = nullptr;
  PopupSurface* allied_ = nullptr;
  HWND allied_hwnd_ = nullptr;
  void (*after_tick_)(void*) = nullptr;
  void* after_tick_ctx_ = nullptr;
  PopupContent* content_ = nullptr;
  bool open_ = false;
  bool esc_down_ = false;
  bool win_down_ = false;
  bool down_down_ = false;
  bool up_down_ = false;
  bool return_down_ = false;
  bool mouse_down_ = false;
  bool press_inside_ = false;
  bool saw_mousemove_ = false;
  bool armed_ = false;
  bool ticking_ = false;
  bool presenting_ = false;
  bool dark_ = true;
  bool capture_ = true;
  bool skip_esc_until_up_ = false;
  int hot_ = -1;
  int drag_index_ = -1;
  unsigned tick_ = 0;
  Anchor mode_ = Anchor::AboveCenter;
  POINT anchor_{};
  Microsoft::WRL::ComPtr<ID2D1Factory> d2d_;
  Microsoft::WRL::ComPtr<ID2D1DCRenderTarget> target_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> stroke_;
  corner::SquircleCache squircle_;
  HDC mem_dc_ = nullptr;
  HBITMAP dib_ = nullptr;
  HGDIOBJ old_dib_ = nullptr;
  int dib_w_ = 0;
  int dib_h_ = 0;
};

}  // namespace bamti
