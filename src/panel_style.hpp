#pragma once

#include <d2d1.h>
#include <dwrite.h>
#include <windows.h>

namespace bamti::panel {

inline constexpr int kWidthDip = 308;
inline constexpr int kInsetDip = 14;
inline constexpr int kTopPadDip = 9;
inline constexpr int kHeaderHDip = 24;
inline constexpr int kHeaderGapDip = 9;
inline constexpr int kDivHDip = 1;
inline constexpr int kDivGapDip = 8;
inline constexpr int kSectionHDip = 17;
inline constexpr int kRowHDip = 32;
inline constexpr int kSettingsHDip = 22;
inline constexpr int kBottomPadDip = 10;
inline constexpr int kToggleWDip = 54;
inline constexpr int kToggleHDip = 24;
inline constexpr int kBackWDip = 24;
inline constexpr int kCircleDip = 26;
inline constexpr float kKnobDip = 19.0f;
inline constexpr float kKnobInsetDip = 2.5f;
inline constexpr int kTextLeftDip = 49;
inline constexpr int kNoteHDip = 16;

class Stack {
 public:
  Stack() : y_(kTopPadDip) {}
  int Take(int h) {
    const int at = y_;
    y_ += h;
    return at;
  }
  void Gap(int h) { y_ += h; }
  int y() const { return y_; }
  int Finish() {
    y_ += kBottomPadDip;
    return y_;
  }

 private:
  int y_;
};

void FillHover(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const RECT& rc, UINT dpi, bool dark);
void DrawDivider(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark);
void DrawToggle(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, const RECT& rc, UINT dpi, bool on, bool enabled,
                bool dark);
void DrawRowCircle(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fluent,
                   ID2D1SolidColorBrush* brush, float cx, float cy, UINT dpi, bool dark, bool active,
                   const wchar_t* glyph);
void DrawSectionHeader(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fmt,
                       ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, const wchar_t* title);
void DrawSettingsRow(ID2D1RenderTarget* rt, IDWriteFactory* dwrite, IDWriteTextFormat* fmt,
                     ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, bool hot, const wchar_t* label);

}  // namespace bamti::panel
