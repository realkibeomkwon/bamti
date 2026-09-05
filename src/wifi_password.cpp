#include "wifi_password.hpp"

#include "corner.hpp"
#include "dwm.hpp"
#include "log.hpp"

#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <windowsx.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace bamti {
namespace {

constexpr wchar_t kClass[] = L"bamti.WifiPassword";
constexpr wchar_t kLabel[] = L"암호";
constexpr wchar_t kFluentFont[] = L"Segoe Fluent Icons";
constexpr wchar_t kEyeShowFluent[] = L"\xE890";
constexpr wchar_t kEyeHideFluent[] = L"\xED1A";
constexpr wchar_t kEyeShowFallback[] = L"\x25CB";
constexpr wchar_t kEyeHideFallback[] = L"\x00D8";
constexpr int kErrorHDip = 18;
constexpr int kInsetDip = 14;
constexpr int kLabelGapDip = 8;
constexpr int kEditVPadDip = 1;
constexpr int kEyeSizeDip = 24;
constexpr int kEyeIconDip = 16;
constexpr int kEyePadDip = 4;
constexpr int kEditEyeGapDip = 8;
constexpr UINT_PTR kRaiseTimer = 1;
constexpr UINT kRaiseMs = 50;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

COLORREF FillRgb(bool dark) {
  return dark ? RGB(31, 31, 31) : RGB(250, 250, 250);
}

COLORREF TextRgb(bool dark) {
  return dark ? RGB(235, 235, 235) : RGB(26, 26, 26);
}

COLORREF ErrorRgb(bool dark) {
  return dark ? RGB(255, 153, 164) : RGB(196, 43, 28);
}

COLORREF MixRgb(COLORREF fg, COLORREF bg, float alpha) {
  const auto mix = [alpha](BYTE f, BYTE b) -> BYTE {
    return static_cast<BYTE>(
        std::lround(static_cast<double>(f) * alpha + static_cast<double>(b) * (1.0 - alpha)));
  };
  return RGB(mix(GetRValue(fg), GetRValue(bg)), mix(GetGValue(fg), GetGValue(bg)),
             mix(GetBValue(fg), GetBValue(bg)));
}

COLORREF LabelRgb(bool dark) {
  return MixRgb(TextRgb(dark), FillRgb(dark), 0.55f);
}

COLORREF IconRgb(bool dark) {
  return MixRgb(TextRgb(dark), FillRgb(dark), 0.80f);
}

COLORREF HoverRgb(bool dark) {
  if (dark) {
    return MixRgb(RGB(255, 255, 255), FillRgb(true), 0.08f);
  }
  return MixRgb(RGB(0, 0, 0), FillRgb(false), 0.06f);
}

int CALLBACK FontExistsProc(const LOGFONTW*, const TEXTMETRICW*, DWORD, LPARAM lp) {
  *reinterpret_cast<bool*>(lp) = true;
  return 0;
}

bool HasGdiFont(const wchar_t* name) {
  if (name == nullptr || name[0] == 0) {
    return false;
  }
  const HDC dc = GetDC(nullptr);
  if (dc == nullptr) {
    return false;
  }
  LOGFONTW lf{};
  lf.lfCharSet = DEFAULT_CHARSET;
  wcsncpy_s(lf.lfFaceName, name, _TRUNCATE);
  bool found = false;
  EnumFontFamiliesExW(dc, &lf, FontExistsProc, reinterpret_cast<LPARAM>(&found), 0);
  ReleaseDC(nullptr, dc);
  return found;
}

bool FontHasGlyph(HDC dc, const wchar_t* text) {
  if (dc == nullptr || text == nullptr || text[0] == 0) {
    return false;
  }
  WORD index = 0;
  if (GetGlyphIndicesW(dc, text, 1, &index, GGI_MARK_NONEXISTING_GLYPHS) <= 0) {
    return false;
  }
  return index != 0xFFFF;
}

void WipeWide(std::wstring* text) {
  if (text == nullptr || text->empty()) {
    return;
  }
  SecureZeroMemory(text->data(), text->size() * sizeof(wchar_t));
  text->clear();
}

void TrackLeave(HWND hwnd) {
  TRACKMOUSEEVENT track{};
  track.cbSize = sizeof(track);
  track.dwFlags = TME_LEAVE;
  track.hwndTrack = hwnd;
  TrackMouseEvent(&track);
}

}  // namespace

WifiPasswordPrompt::~WifiPasswordPrompt() {
  Hide();
  if (hwnd_ != nullptr && IsWindow(hwnd_)) {
    DestroyWindow(hwnd_);
  }
  hwnd_ = nullptr;
  edit_ = nullptr;
  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (icon_font_ != nullptr) {
    DeleteObject(icon_font_);
    icon_font_ = nullptr;
  }
  if (bg_brush_ != nullptr) {
    DeleteObject(bg_brush_);
    bg_brush_ = nullptr;
  }
}

void WifiPasswordPrompt::SetCallbacks(SubmitFn on_submit, CancelFn on_cancel) {
  on_submit_ = std::move(on_submit);
  on_cancel_ = std::move(on_cancel);
}

bool WifiPasswordPrompt::Show(HWND owner, RECT screen_row, UINT dpi, bool dark, const std::wstring& error) {
  if (screen_row.right <= screen_row.left || screen_row.bottom <= screen_row.top) {
    return false;
  }
  owner_ = owner;
  screen_row_ = screen_row;
  dpi_ = dpi != 0 ? dpi : 96;
  dark_ = dark;
  error_ = error;
  revealed_ = false;
  eye_hot_ = false;
  if (!EnsureWindow(owner)) {
    return false;
  }
  ApplyMasked();
  ApplyChrome();
  Layout();
  visible_ = true;
  if (hwnd_ != nullptr) {
    SetTimer(hwnd_, kRaiseTimer, kRaiseMs, nullptr);
    Raise();
  }
  return true;
}

void WifiPasswordPrompt::Hide() {
  ClearEdit();
  error_.clear();
  revealed_ = false;
  eye_hot_ = false;
  ApplyMasked();
  visible_ = false;
  if (hwnd_ != nullptr && IsWindow(hwnd_)) {
    KillTimer(hwnd_, kRaiseTimer);
    ShowWindow(hwnd_, SW_HIDE);
  }
}

bool WifiPasswordPrompt::EnsureWindow(HWND owner) {
  if (hwnd_ != nullptr && IsWindow(hwnd_)) {
    SetWindowLongPtrW(hwnd_, GWLP_HWNDPARENT, reinterpret_cast<LONG_PTR>(owner));
    return edit_ != nullptr;
  }

  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_HREDRAW | CS_VREDRAW;
  wc.lpfnWndProc = WndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.hCursor = LoadCursorW(nullptr, IDC_IBEAM);
  wc.hbrBackground = nullptr;
  wc.lpszClassName = kClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return false;
  }

  hwnd_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kClass, L"", WS_POPUP, 0, 0, 0, 0, owner, nullptr,
                          wc.hInstance, this);
  if (hwnd_ == nullptr) {
    return false;
  }

  edit_ = CreateWindowExW(0, L"EDIT", L"", WS_CHILD | WS_VISIBLE | ES_AUTOHSCROLL | ES_LEFT | ES_PASSWORD, 0, 0, 0, 0,
                          hwnd_, nullptr, wc.hInstance, nullptr);
  if (edit_ == nullptr) {
    return false;
  }
  SetWindowTheme(edit_, L"", L"");
  mask_char_ = 0x25CF;
  SendMessageW(edit_, EM_SETPASSWORDCHAR, mask_char_, 0);
  InvalidateRect(edit_, nullptr, TRUE);
  SetWindowLongPtrW(edit_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));
  edit_prev_ = reinterpret_cast<WNDPROC>(SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(EditProc)));
  return true;
}

void WifiPasswordPrompt::Layout() {
  if (hwnd_ == nullptr) {
    return;
  }
  const int extra = error_.empty() ? 0 : DipToPx(kErrorHDip, dpi_);
  const int x = screen_row_.left;
  const int y = screen_row_.top;
  const int w = screen_row_.right - screen_row_.left;
  const int h = (screen_row_.bottom - screen_row_.top) + extra;
  SetWindowPos(hwnd_, HWND_TOPMOST, x, y, w, h, SWP_NOACTIVATE);
  ApplyRegion();

  if (font_ != nullptr) {
    DeleteObject(font_);
    font_ = nullptr;
  }
  if (icon_font_ != nullptr) {
    DeleteObject(icon_font_);
    icon_font_ = nullptr;
  }
  LOGFONTW lf{};
  lf.lfHeight = -DipToPx(14, dpi_);
  lf.lfWeight = FW_NORMAL;
  lf.lfCharSet = DEFAULT_CHARSET;
  lf.lfQuality = CLEARTYPE_QUALITY;
  wcsncpy_s(lf.lfFaceName, L"Segoe UI", _TRUNCATE);
  font_ = CreateFontIndirectW(&lf);
  if (font_ != nullptr && edit_ != nullptr) {
    SendMessageW(edit_, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
  }

  fluent_icons_ = HasGdiFont(kFluentFont);
  LOGFONTW icon_lf = lf;
  icon_lf.lfHeight = -DipToPx(kEyeIconDip, dpi_);
  wcsncpy_s(icon_lf.lfFaceName, fluent_icons_ ? kFluentFont : L"Segoe UI", _TRUNCATE);
  icon_font_ = CreateFontIndirectW(&icon_lf);
  if (fluent_icons_ && icon_font_ != nullptr && hwnd_ != nullptr) {
    const HDC probe = GetDC(hwnd_);
    if (probe != nullptr) {
      const HFONT old = static_cast<HFONT>(SelectObject(probe, icon_font_));
      if (!FontHasGlyph(probe, kEyeShowFluent) || !FontHasGlyph(probe, kEyeHideFluent)) {
        fluent_icons_ = false;
      }
      SelectObject(probe, old);
      ReleaseDC(hwnd_, probe);
    }
    if (!fluent_icons_) {
      DeleteObject(icon_font_);
      wcsncpy_s(icon_lf.lfFaceName, L"Segoe UI", _TRUNCATE);
      icon_font_ = CreateFontIndirectW(&icon_lf);
    }
  }

  int glyph_h = 0;
  label_w_ = 0;
  const HDC dc = GetDC(hwnd_);
  if (dc != nullptr) {
    HFONT old = nullptr;
    if (font_ != nullptr) {
      old = static_cast<HFONT>(SelectObject(dc, font_));
    }
    TEXTMETRICW tm{};
    if (GetTextMetricsW(dc, &tm)) {
      glyph_h = tm.tmHeight;
    }
    SIZE sz{};
    if (GetTextExtentPoint32W(dc, kLabel, static_cast<int>(wcslen(kLabel)), &sz)) {
      label_w_ = sz.cx;
    }
    if (old != nullptr) {
      SelectObject(dc, old);
    }
    ReleaseDC(hwnd_, dc);
  }

  const RECT row = RowRect();
  const int row_h = row.bottom - row.top;
  const int inset = DipToPx(kInsetDip, dpi_);
  const int gap = DipToPx(kLabelGapDip, dpi_);
  int edit_h = glyph_h + DipToPx(kEditVPadDip, dpi_) * 2;
  if (edit_h <= 0) {
    edit_h = row_h;
  } else if (edit_h > row_h) {
    edit_h = row_h;
  }
  const int edit_top = static_cast<int>(std::lround(
      (static_cast<double>(row.top) + static_cast<double>(row.bottom) - static_cast<double>(glyph_h > 0 ? glyph_h : edit_h)) *
      0.5));
  const int edit_left = row.left + inset + label_w_ + gap;
  const RECT eye = EyeRect();
  const int edit_w = (std::max)(0, static_cast<int>(eye.left) - DipToPx(kEditEyeGapDip, dpi_) - edit_left);
  if (edit_ != nullptr) {
    SetWindowPos(edit_, nullptr, edit_left, edit_top, edit_w, edit_h, SWP_NOZORDER | SWP_NOACTIVATE);
    RECT edit_rc{};
    GetWindowRect(edit_, &edit_rc);
    RECT edit_parent = edit_rc;
    MapWindowPoints(nullptr, hwnd_, reinterpret_cast<POINT*>(&edit_parent), 2);
    const bool overlap = edit_parent.left < eye.right && eye.left < edit_parent.right &&
                         edit_parent.top < eye.bottom && eye.top < edit_parent.bottom;
    Log(L"wifi",
        L"field dpi=%u edit=(%d,%d,%d,%d) eye=(%d,%d,%d,%d) overlap=%d gap=%d", dpi_, edit_parent.left,
        edit_parent.top, edit_parent.right, edit_parent.bottom, eye.left, eye.top, eye.right, eye.bottom,
        overlap ? 1 : 0, static_cast<int>(eye.left) - static_cast<int>(edit_parent.right));
  }
}

void WifiPasswordPrompt::ApplyRegion() {
  if (hwnd_ == nullptr) {
    return;
  }
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int w = rc.right - rc.left;
  const int h = rc.bottom - rc.top;
  const int row_h = screen_row_.bottom - screen_row_.top;
  const int rad = (std::max)(2, static_cast<int>(corner::HoverPx(static_cast<float>(row_h), dpi_)));
  const HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, rad * 2, rad * 2);
  if (rgn != nullptr) {
    SetWindowRgn(hwnd_, rgn, TRUE);
  }
}

void WifiPasswordPrompt::ApplyChrome() {
  if (bg_brush_ != nullptr) {
    DeleteObject(bg_brush_);
    bg_brush_ = nullptr;
  }
  bg_brush_ = CreateSolidBrush(FillRgb(dark_));
  if (hwnd_ == nullptr) {
    return;
  }
  const BOOL dark = dark_ ? TRUE : FALSE;
  DwmSetWindowAttribute(hwnd_, dwm::kUseImmersiveDarkMode, &dark, sizeof(dark));
}

void WifiPasswordPrompt::FocusEdit() {
  if (hwnd_ == nullptr || edit_ == nullptr) {
    return;
  }
  HWND foreground = GetForegroundWindow();
  DWORD other_tid = 0;
  if (foreground != nullptr) {
    other_tid = GetWindowThreadProcessId(foreground, nullptr);
  }
  const DWORD self_tid = GetCurrentThreadId();
  if (other_tid != 0 && other_tid != self_tid) {
    AttachThreadInput(self_tid, other_tid, TRUE);
  }
  SetForegroundWindow(hwnd_);
  if (other_tid != 0 && other_tid != self_tid) {
    AttachThreadInput(self_tid, other_tid, FALSE);
  }
  SetFocus(edit_);
  Raise();
}

RECT WifiPasswordPrompt::RowRect() const {
  const int w = screen_row_.right - screen_row_.left;
  const int h = screen_row_.bottom - screen_row_.top;
  return RECT{0, 0, w, h};
}

RECT WifiPasswordPrompt::ErrorRect() const {
  const RECT row = RowRect();
  RECT rc{};
  GetClientRect(hwnd_, &rc);
  const int inset = DipToPx(kInsetDip, dpi_);
  return RECT{inset, row.bottom, rc.right - inset, rc.bottom};
}

RECT WifiPasswordPrompt::LabelRect() const {
  const RECT row = RowRect();
  const int inset = DipToPx(kInsetDip, dpi_);
  return RECT{row.left + inset, row.top, row.left + inset + label_w_, row.bottom};
}

RECT WifiPasswordPrompt::EyeRect() const {
  const RECT row = RowRect();
  const int inset = DipToPx(kInsetDip, dpi_);
  const int pad = DipToPx(kEyePadDip, dpi_);
  const int size = DipToPx(kEyeSizeDip, dpi_);
  RECT rc{};
  rc.right = row.right - inset - pad;
  rc.left = rc.right - size;
  rc.top = static_cast<int>(
      std::lround((static_cast<double>(row.top) + static_cast<double>(row.bottom) - static_cast<double>(size)) * 0.5));
  rc.bottom = rc.top + size;
  return rc;
}

void WifiPasswordPrompt::Raise() {
  if (hwnd_ == nullptr || !visible_) {
    return;
  }
  SetWindowPos(hwnd_, HWND_TOP, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
}

void WifiPasswordPrompt::ApplyMasked() {
  if (edit_ == nullptr || mask_char_ == 0) {
    return;
  }
  SendMessageW(edit_, EM_SETPASSWORDCHAR, mask_char_, 0);
  InvalidateRect(edit_, nullptr, TRUE);
}

void WifiPasswordPrompt::ToggleReveal() {
  if (edit_ == nullptr) {
    return;
  }
  DWORD sel_start = 0;
  DWORD sel_end = 0;
  SendMessageW(edit_, EM_GETSEL, reinterpret_cast<WPARAM>(&sel_start), reinterpret_cast<LPARAM>(&sel_end));
  revealed_ = !revealed_;
  if (revealed_) {
    SendMessageW(edit_, EM_SETPASSWORDCHAR, 0, 0);
  } else if (mask_char_ != 0) {
    SendMessageW(edit_, EM_SETPASSWORDCHAR, mask_char_, 0);
  }
  SendMessageW(edit_, EM_SETSEL, sel_start, sel_end);
  InvalidateRect(edit_, nullptr, TRUE);
  if (hwnd_ != nullptr) {
    const RECT eye = EyeRect();
    InvalidateRect(hwnd_, &eye, FALSE);
  }
}

void WifiPasswordPrompt::SetEyeHot(bool hot) {
  if (eye_hot_ == hot) {
    return;
  }
  eye_hot_ = hot;
  const RECT eye = EyeRect();
  if (hwnd_ != nullptr) {
    InvalidateRect(hwnd_, &eye, FALSE);
  }
  if (edit_ != nullptr) {
    RECT local = eye;
    MapWindowPoints(hwnd_, edit_, reinterpret_cast<POINT*>(&local), 2);
    InvalidateRect(edit_, &local, FALSE);
  }
}

void WifiPasswordPrompt::DrawEye(HDC dc, HWND origin) const {
  if (dc == nullptr) {
    return;
  }
  RECT eye = EyeRect();
  if (origin != nullptr && origin != hwnd_) {
    MapWindowPoints(hwnd_, origin, reinterpret_cast<POINT*>(&eye), 2);
  }
  if (eye_hot_) {
    const HBRUSH br = CreateSolidBrush(HoverRgb(dark_));
    const HGDIOBJ old_br = SelectObject(dc, br != nullptr ? br : GetStockObject(NULL_BRUSH));
    const HGDIOBJ old_pen = SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, eye.left, eye.top, eye.right, eye.bottom);
    SelectObject(dc, old_pen);
    SelectObject(dc, old_br);
    if (br != nullptr) {
      DeleteObject(br);
    }
  }
  const wchar_t* glyph = revealed_ ? (fluent_icons_ ? kEyeHideFluent : kEyeHideFallback)
                                   : (fluent_icons_ ? kEyeShowFluent : kEyeShowFallback);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, IconRgb(dark_));
  HFONT old = nullptr;
  if (icon_font_ != nullptr) {
    old = static_cast<HFONT>(SelectObject(dc, icon_font_));
  }
  DrawTextW(dc, glyph, -1, &eye, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
  if (old != nullptr) {
    SelectObject(dc, old);
  }
}

bool WifiPasswordPrompt::HitEye(POINT parent_pt) const {
  const RECT eye = EyeRect();
  return PtInRect(&eye, parent_pt) != FALSE;
}

POINT WifiPasswordPrompt::ToParent(HWND from, LPARAM lp) const {
  POINT pt{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
  if (from != nullptr && hwnd_ != nullptr && from != hwnd_) {
    MapWindowPoints(from, hwnd_, &pt, 1);
  }
  return pt;
}

void WifiPasswordPrompt::ClearEdit() {
  if (edit_ == nullptr) {
    return;
  }
  const int n = GetWindowTextLengthW(edit_);
  if (n > 0) {
    std::wstring wipe(static_cast<size_t>(n), L'\0');
    GetWindowTextW(edit_, wipe.data(), n + 1);
    SetWindowTextW(edit_, L"");
    WipeWide(&wipe);
  } else {
    SetWindowTextW(edit_, L"");
  }
}

void WifiPasswordPrompt::Submit() {
  if (!visible_ || edit_ == nullptr) {
    return;
  }
  const int n = GetWindowTextLengthW(edit_);
  std::wstring password(static_cast<size_t>(n), L'\0');
  if (n > 0) {
    GetWindowTextW(edit_, password.data(), n + 1);
  }
  SetWindowTextW(edit_, L"");
  if (on_submit_) {
    on_submit_(std::move(password));
  } else {
    WipeWide(&password);
  }
}

void WifiPasswordPrompt::Cancel() {
  ClearEdit();
  Hide();
  if (on_cancel_) {
    on_cancel_();
  }
}

LRESULT CALLBACK WifiPasswordPrompt::WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  WifiPasswordPrompt* self = nullptr;
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    self = static_cast<WifiPasswordPrompt*>(cs->lpCreateParams);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
    if (self != nullptr) {
      self->hwnd_ = hwnd;
    }
  } else {
    self = reinterpret_cast<WifiPasswordPrompt*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }
  if (self == nullptr) {
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  return self->Handle(msg, wp, lp);
}

LRESULT CALLBACK WifiPasswordPrompt::EditProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  auto* self = reinterpret_cast<WifiPasswordPrompt*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  if (self == nullptr || self->edit_prev_ == nullptr) {
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  if (msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN) {
    if (wp == VK_RETURN) {
      self->Submit();
      return 0;
    }
    if (wp == VK_ESCAPE) {
      self->Cancel();
      return 0;
    }
  }
  if (msg == WM_CHAR && (wp == VK_RETURN || wp == L'\n' || wp == VK_ESCAPE)) {
    return 0;
  }
  if (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) {
    if (self->HitEye(self->ToParent(hwnd, lp))) {
      self->ToggleReveal();
      SetFocus(self->edit_);
      return 0;
    }
  }
  if (msg == WM_MOUSEMOVE) {
    TrackLeave(hwnd);
    self->SetEyeHot(self->HitEye(self->ToParent(hwnd, lp)));
  }
  if (msg == WM_MOUSELEAVE) {
    self->SetEyeHot(false);
  }
  if (msg == WM_SETCURSOR) {
    POINT pt{};
    GetCursorPos(&pt);
    MapWindowPoints(nullptr, self->hwnd_, &pt, 1);
    if (self->HitEye(pt)) {
      SetCursor(LoadCursorW(nullptr, IDC_HAND));
      return TRUE;
    }
  }
  if (msg == WM_PAINT) {
    const LRESULT result = CallWindowProcW(self->edit_prev_, hwnd, msg, wp, lp);
    const HDC dc = GetDC(hwnd);
    if (dc != nullptr) {
      self->DrawEye(dc, hwnd);
      ReleaseDC(hwnd, dc);
    }
    return result;
  }
  return CallWindowProcW(self->edit_prev_, hwnd, msg, wp, lp);
}

LRESULT WifiPasswordPrompt::Handle(UINT msg, WPARAM wp, LPARAM lp) {
  switch (msg) {
    case WM_ERASEBKGND:
      return 1;
    case WM_PAINT: {
      PAINTSTRUCT ps{};
      const HDC dc = BeginPaint(hwnd_, &ps);
      RECT client{};
      GetClientRect(hwnd_, &client);
      if (bg_brush_ != nullptr) {
        FillRect(dc, &client, bg_brush_);
      }
      {
        const RECT label = LabelRect();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, LabelRgb(dark_));
        HFONT old = nullptr;
        if (font_ != nullptr) {
          old = static_cast<HFONT>(SelectObject(dc, font_));
        }
        DrawTextW(dc, kLabel, -1, const_cast<RECT*>(&label), DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_NOPREFIX);
        if (old != nullptr) {
          SelectObject(dc, old);
        }
      }
      DrawEye(dc, hwnd_);
      if (!error_.empty()) {
        const RECT err = ErrorRect();
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, ErrorRgb(dark_));
        HFONT old = nullptr;
        if (font_ != nullptr) {
          old = static_cast<HFONT>(SelectObject(dc, font_));
        }
        DrawTextW(dc, error_.c_str(), -1, const_cast<RECT*>(&err),
                  DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        if (old != nullptr) {
          SelectObject(dc, old);
        }
      }
      EndPaint(hwnd_, &ps);
      return 0;
    }
    case WM_CTLCOLOREDIT: {
      const HDC dc = reinterpret_cast<HDC>(wp);
      SetTextColor(dc, TextRgb(dark_));
      SetBkColor(dc, FillRgb(dark_));
      SetBkMode(dc, OPAQUE);
      return reinterpret_cast<LRESULT>(bg_brush_ != nullptr ? bg_brush_ : GetStockObject(BLACK_BRUSH));
    }
    case WM_MOUSEMOVE:
      TrackLeave(hwnd_);
      SetEyeHot(HitEye(ToParent(hwnd_, lp)));
      return 0;
    case WM_MOUSELEAVE:
      SetEyeHot(false);
      return 0;
    case WM_LBUTTONDOWN:
      if (HitEye(ToParent(hwnd_, lp))) {
        ToggleReveal();
        if (edit_ != nullptr) {
          SetFocus(edit_);
        }
        return 0;
      }
      if (edit_ != nullptr) {
        SetFocus(edit_);
      }
      return 0;
    case WM_SETCURSOR: {
      POINT pt{};
      GetCursorPos(&pt);
      MapWindowPoints(nullptr, hwnd_, &pt, 1);
      if (HitEye(pt)) {
        SetCursor(LoadCursorW(nullptr, IDC_HAND));
        return TRUE;
      }
      break;
    }
    case WM_TIMER:
      if (wp == kRaiseTimer) {
        Raise();
        return 0;
      }
      break;
    case WM_COMMAND:
      if (HIWORD(wp) == EN_SETFOCUS && edit_ != nullptr) {
        SetFocus(edit_);
      }
      return 0;
    case WM_DESTROY:
      KillTimer(hwnd_, kRaiseTimer);
      if (edit_ != nullptr && edit_prev_ != nullptr) {
        SetWindowLongPtrW(edit_, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(edit_prev_));
      }
      edit_ = nullptr;
      edit_prev_ = nullptr;
      hwnd_ = nullptr;
      visible_ = false;
      return 0;
    default:
      break;
  }
  return DefWindowProcW(hwnd_, msg, wp, lp);
}

}  // namespace bamti
