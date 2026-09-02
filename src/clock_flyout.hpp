#pragma once

#include "popup_surface.hpp"

#include <dwrite.h>
#include <wrl/client.h>

#include <string>
#include <vector>

namespace bamti {

class ClockFlyoutContent : public PopupContent {
 public:
  void Reset(bool dark);
  void Refresh();

  int RowCount() const override;
  bool PaintsOwnChrome() const override { return true; }
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;
  bool StickyRow(int index) const override;
  void StickyInvoke(int index) override;

 private:
  enum HitId {
    kNotifyBg = 0,
    kCalendarBg,
    kQuietHours,
    kClearAll,
    kDateHeader,
    kMonthPrev,
    kMonthNext,
    kFocusMinus,
    kFocusPlus,
    kFocusStart,
    kDay0 = 100,
  };

  struct Hit {
    RECT rc{};
    int id = -1;
  };

  struct Note {
    std::wstring app;
    std::wstring title;
    std::wstring body;
    std::wstring when;
  };

  void EnsureFormats(UINT dpi);
  void BuildHits(UINT dpi);
  RECT NotifyRect(UINT dpi) const;
  RECT CalendarRect(UINT dpi) const;
  RECT DayCell(UINT dpi, int index) const;
  int NotifyHeightDip() const;
  int CalendarHeightDip() const;
  int FirstDayOffset() const;
  void ShiftMonth(int delta);
  void SelectCell(int index);
  int HitIdAt(int index) const;

  bool dark_ = true;
  bool calendar_open_ = true;
  int focus_minutes_ = 30;
  SYSTEMTIME today_{};
  int view_year_ = 0;
  int view_month_ = 0;
  int sel_year_ = 0;
  int sel_month_ = 0;
  int sel_day_ = 0;
  int first_weekday_ = 6;
  std::vector<Note> notes_;
  std::vector<Hit> hits_;
  UINT format_dpi_ = 0;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent16_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent14_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> semibold14_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> semibold13_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> regular13_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> regular12_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> muted11_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> center12_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> center11_;
};

class ClockMenuContent : public PopupContent {
 public:
  void Reset(bool dark);

  int RowCount() const override { return 2; }
  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;

 private:
  RECT RowRect(int index, UINT dpi) const;

  bool dark_ = true;
  int width_px_ = 0;
};

}  // namespace bamti
