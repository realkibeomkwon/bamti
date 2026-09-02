#include "clock_flyout.hpp"

#include "theme.hpp"

#include <d2d1helper.h>
#include <shellapi.h>

#include <algorithm>
#include <cwchar>

namespace bamti {
namespace {

constexpr int kWidthDip = 360;
constexpr int kGapDip = 12;
constexpr int kCardRadiusDip = 8;
constexpr int kNotifyPadDip = 16;
constexpr int kNotifyHeaderDip = 40;
constexpr int kNotifyEmptyDip = 100;
constexpr int kNoteDip = 78;
constexpr int kNoteGapDip = 8;
constexpr int kNotifyMaxDip = 280;
constexpr int kCalPadDip = 14;
constexpr int kCalHeaderDip = 44;
constexpr int kMonthNavDip = 34;
constexpr int kWeekdayDip = 22;
constexpr int kDayDip = 36;
constexpr int kWeeks = 6;
constexpr int kFocusDip = 48;
constexpr int kFocusMin = 15;
constexpr int kFocusMax = 240;
constexpr int kFocusStep = 15;

constexpr wchar_t kFluentFont[] = L"Segoe Fluent Icons";
constexpr wchar_t kUiFont[] = L"Segoe UI";
constexpr wchar_t kQuietGlyph[] = L"\xEA8F";
constexpr wchar_t kChevronDown[] = L"\xE70D";
constexpr wchar_t kChevronUp[] = L"\xE70E";
constexpr wchar_t kPlayGlyph[] = L"\xE768";
constexpr wchar_t kGearGlyph[] = L"\xE713";

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

D2D1_COLOR_F ScaleAlpha(D2D1_COLOR_F color, float mul) {
  color.a *= mul;
  return color;
}

bool LeapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

int DaysInMonth(int year, int month) {
  static constexpr int kDays[] = {0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month == 2 && LeapYear(year)) {
    return 29;
  }
  if (month < 1 || month > 12) {
    return 30;
  }
  return kDays[month];
}

int WeekdaySunday(int year, int month, int day) {
  SYSTEMTIME st{};
  st.wYear = static_cast<WORD>(year);
  st.wMonth = static_cast<WORD>(month);
  st.wDay = static_cast<WORD>(day);
  FILETIME ft{};
  if (SystemTimeToFileTime(&st, &ft) == FALSE) {
    return 0;
  }
  if (FileTimeToSystemTime(&ft, &st) == FALSE) {
    return 0;
  }
  return st.wDayOfWeek;
}

int LocaleFirstWeekdaySunday() {
  DWORD first = 0;
  const int n = GetLocaleInfoEx(LOCALE_NAME_USER_DEFAULT, LOCALE_IFIRSTDAYOFWEEK | LOCALE_RETURN_NUMBER,
                                reinterpret_cast<LPWSTR>(&first), static_cast<int>(sizeof(first) / sizeof(WCHAR)));
  if (n <= 0) {
    return 0;
  }
  return static_cast<int>((first + 1) % 7);
}

const wchar_t* UserLocale() {
  static wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
  static bool ready = false;
  if (!ready) {
    if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
      lstrcpynW(locale, L"en-US", LOCALE_NAME_MAX_LENGTH);
    }
    ready = true;
  }
  return locale;
}

bool KoreanLocale() {
  return wcsncmp(UserLocale(), L"ko", 2) == 0;
}

std::wstring FormatDate(const SYSTEMTIME& st, DWORD flags, const wchar_t* picture) {
  wchar_t buf[128]{};
  if (GetDateFormatEx(UserLocale(), flags, &st, picture, buf, 128, nullptr) > 1) {
    return buf;
  }
  return {};
}

std::wstring HeaderDate(const SYSTEMTIME& st) {
  if (KoreanLocale()) {
    std::wstring text = FormatDate(st, 0, L"M월 d일 dddd");
    if (!text.empty()) {
      return text;
    }
  }
  return FormatDate(st, DATE_LONGDATE, nullptr);
}

std::wstring MonthTitle(int year, int month) {
  SYSTEMTIME st{};
  st.wYear = static_cast<WORD>(year);
  st.wMonth = static_cast<WORD>(month);
  st.wDay = 1;
  wchar_t pattern[80]{};
  if (GetLocaleInfoEx(UserLocale(), LOCALE_SYEARMONTH, pattern, 80) > 1) {
    std::wstring text = FormatDate(st, 0, pattern);
    if (!text.empty()) {
      return text;
    }
  }
  if (KoreanLocale()) {
    return FormatDate(st, 0, L"yyyy년 M월");
  }
  return FormatDate(st, 0, L"MMMM yyyy");
}

std::wstring WeekdayLabel(int sunday_based) {
  const LCTYPE names[] = {LOCALE_SABBREVDAYNAME7, LOCALE_SABBREVDAYNAME1, LOCALE_SABBREVDAYNAME2, LOCALE_SABBREVDAYNAME3,
                          LOCALE_SABBREVDAYNAME4, LOCALE_SABBREVDAYNAME5, LOCALE_SABBREVDAYNAME6};
  wchar_t buf[16]{};
  if (GetLocaleInfoEx(UserLocale(), names[sunday_based % 7], buf, 16) > 1) {
    return buf;
  }
  return {};
}

void OpenUri(const wchar_t* uri) {
  if (uri == nullptr || uri[0] == 0) {
    return;
  }
  ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

bool MakeFormat(IDWriteFactory* dwrite, const wchar_t* family, DWRITE_FONT_WEIGHT weight, float px,
                DWRITE_TEXT_ALIGNMENT align, Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
  out.Reset();
  if (dwrite == nullptr) {
    return false;
  }
  if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                      UserLocale(), out.ReleaseAndGetAddressOf()))) {
    if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                        px, L"en-US", out.ReleaseAndGetAddressOf()))) {
      return false;
    }
  }
  out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  out->SetTextAlignment(align);
  out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  return true;
}

void DrawGlyph(ID2D1RenderTarget* target, IDWriteFactory* dwrite, IDWriteTextFormat* format, ID2D1Brush* brush,
               const D2D1_RECT_F& box, const wchar_t* glyph) {
  if (target == nullptr || dwrite == nullptr || format == nullptr || brush == nullptr || glyph == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(glyph, 1, format, box.right - box.left, box.bottom - box.top,
                                      layout.GetAddressOf()))) {
    return;
  }
  target->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawTrimmed(ID2D1RenderTarget* target, IDWriteFactory* dwrite, IDWriteTextFormat* format, ID2D1Brush* brush,
                 const D2D1_RECT_F& box, const std::wstring& text) {
  if (target == nullptr || dwrite == nullptr || format == nullptr || brush == nullptr || text.empty()) {
    return;
  }
  const float width = (std::max)(0.0f, box.right - box.left);
  const float height = (std::max)(0.0f, box.bottom - box.top);
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format, width, height,
                                      layout.GetAddressOf()))) {
    return;
  }
  DWRITE_TRIMMING trim{DWRITE_TRIMMING_GRANULARITY_CHARACTER, 0, 0};
  Microsoft::WRL::ComPtr<IDWriteInlineObject> ellipsis;
  if (SUCCEEDED(dwrite->CreateEllipsisTrimmingSign(format, ellipsis.GetAddressOf()))) {
    layout->SetTrimming(&trim, ellipsis.Get());
  }
  target->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush, D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

}  // namespace

void ClockFlyoutContent::Reset(bool dark) {
  dark_ = dark;
  calendar_open_ = true;
  focus_minutes_ = 30;
  GetLocalTime(&today_);
  view_year_ = today_.wYear;
  view_month_ = today_.wMonth;
  sel_year_ = today_.wYear;
  sel_month_ = today_.wMonth;
  sel_day_ = today_.wDay;
  first_weekday_ = LocaleFirstWeekdaySunday();
  notes_.clear();
}

void ClockFlyoutContent::Refresh() {
  GetLocalTime(&today_);
}

int ClockFlyoutContent::RowCount() const {
  return static_cast<int>(hits_.size());
}

int ClockFlyoutContent::NotifyHeightDip() const {
  int h = kNotifyPadDip + kNotifyHeaderDip;
  if (notes_.empty()) {
    h += kNotifyEmptyDip;
  } else {
    h += static_cast<int>(notes_.size()) * kNoteDip;
    if (notes_.size() > 1) {
      h += static_cast<int>(notes_.size() - 1) * kNoteGapDip;
    }
  }
  h += kNotifyPadDip;
  return (std::min)(h, kNotifyMaxDip);
}

int ClockFlyoutContent::CalendarHeightDip() const {
  int h = kCalPadDip + kCalHeaderDip;
  if (calendar_open_) {
    h += kMonthNavDip + kWeekdayDip + kDayDip * kWeeks;
  }
  h += kFocusDip + kCalPadDip;
  return h;
}

RECT ClockFlyoutContent::NotifyRect(UINT dpi) const {
  return RECT{0, 0, DipToPx(kWidthDip, dpi), DipToPx(NotifyHeightDip(), dpi)};
}

RECT ClockFlyoutContent::CalendarRect(UINT dpi) const {
  const int y = DipToPx(NotifyHeightDip() + kGapDip, dpi);
  return RECT{0, y, DipToPx(kWidthDip, dpi), y + DipToPx(CalendarHeightDip(), dpi)};
}

int ClockFlyoutContent::FirstDayOffset() const {
  const int first = WeekdaySunday(view_year_, view_month_, 1);
  return (first - first_weekday_ + 7) % 7;
}

RECT ClockFlyoutContent::DayCell(UINT dpi, int index) const {
  const RECT cal = CalendarRect(dpi);
  const int pad = DipToPx(kCalPadDip, dpi);
  const int top = cal.top + DipToPx(kCalPadDip + kCalHeaderDip + kMonthNavDip + kWeekdayDip, dpi);
  const int grid_w = cal.right - cal.left - pad * 2;
  const int cell_w = grid_w / 7;
  const int cell_h = DipToPx(kDayDip, dpi);
  const int col = index % 7;
  const int row = index / 7;
  const int x = cal.left + pad + col * cell_w;
  const int y = top + row * cell_h;
  return RECT{x, y, x + cell_w, y + cell_h};
}

void ClockFlyoutContent::EnsureFormats(UINT dpi) {
  if (!dwrite_) {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf()));
  }
  if (dwrite_ && format_dpi_ == dpi && fluent16_ && semibold14_ && regular13_ && center12_) {
    return;
  }
  fluent16_.Reset();
  fluent14_.Reset();
  semibold14_.Reset();
  semibold13_.Reset();
  regular13_.Reset();
  regular12_.Reset();
  muted11_.Reset();
  center12_.Reset();
  center11_.Reset();
  format_dpi_ = dpi;
  if (!dwrite_) {
    return;
  }
  const float s = static_cast<float>(dpi) / 96.0f;
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 16.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent16_);
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 14.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent14_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, semibold14_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, semibold13_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 13.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular13_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 12.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular12_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 11.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, muted11_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 12.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, center12_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 11.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, center11_);
}

void ClockFlyoutContent::BuildHits(UINT dpi) {
  hits_.clear();
  auto add = [&](int id, RECT rc) {
    Hit hit;
    hit.id = id;
    hit.rc = rc;
    hits_.push_back(hit);
  };
  const RECT notify = NotifyRect(dpi);
  const RECT cal = CalendarRect(dpi);
  add(kNotifyBg, notify);
  add(kCalendarBg, cal);

  const int pad = DipToPx(kNotifyPadDip, dpi);
  const int header_h = DipToPx(kNotifyHeaderDip, dpi);
  const int icon = DipToPx(28, dpi);
  RECT quiet{notify.right - pad - icon, notify.top + pad, notify.right - pad, notify.top + pad + header_h};
  if (!notes_.empty()) {
    const int clear_w = DipToPx(72, dpi);
    RECT clear{quiet.left - DipToPx(8, dpi) - clear_w, quiet.top, quiet.left - DipToPx(8, dpi), quiet.bottom};
    add(kClearAll, clear);
    quiet.left -= clear_w + DipToPx(8, dpi);
    quiet.right -= clear_w + DipToPx(8, dpi);
  }
  add(kQuietHours, quiet);

  const int cpad = DipToPx(kCalPadDip, dpi);
  const int chev = DipToPx(28, dpi);
  RECT date_hdr{cal.left + cpad, cal.top + cpad, cal.right - cpad - chev, cal.top + cpad + DipToPx(kCalHeaderDip, dpi)};
  RECT date_chev{cal.right - cpad - chev, date_hdr.top, cal.right - cpad, date_hdr.bottom};
  add(kDateHeader, RECT{date_hdr.left, date_hdr.top, date_chev.right, date_hdr.bottom});

  if (calendar_open_) {
    const int nav_y = date_hdr.bottom;
    const int nav_h = DipToPx(kMonthNavDip, dpi);
    const int nav_btn = DipToPx(28, dpi);
    add(kMonthPrev, RECT{cal.right - cpad - nav_btn * 2 - DipToPx(4, dpi), nav_y, cal.right - cpad - nav_btn - DipToPx(4, dpi),
                         nav_y + nav_h});
    add(kMonthNext, RECT{cal.right - cpad - nav_btn, nav_y, cal.right - cpad, nav_y + nav_h});
    for (int i = 0; i < 7 * kWeeks; ++i) {
      add(kDay0 + i, DayCell(dpi, i));
    }
  }

  const int focus_h = DipToPx(kFocusDip, dpi);
  const int focus_y = cal.bottom - cpad - focus_h;
  const int btn = DipToPx(28, dpi);
  add(kFocusMinus, RECT{cal.left + cpad, focus_y + (focus_h - btn) / 2, cal.left + cpad + btn,
                        focus_y + (focus_h - btn) / 2 + btn});
  add(kFocusPlus, RECT{cal.left + cpad + btn + DipToPx(64, dpi), focus_y + (focus_h - btn) / 2,
                       cal.left + cpad + btn * 2 + DipToPx(64, dpi), focus_y + (focus_h - btn) / 2 + btn});
  add(kFocusStart, RECT{cal.right - cpad - DipToPx(72, dpi), focus_y, cal.right - cpad, cal.bottom - cpad});
}

SIZE ClockFlyoutContent::Measure(UINT dpi) {
  EnsureFormats(dpi);
  BuildHits(dpi);
  return SIZE{DipToPx(kWidthDip, dpi), DipToPx(NotifyHeightDip() + kGapDip + CalendarHeightDip(), dpi)};
}

int ClockFlyoutContent::HitIdAt(int index) const {
  if (index < 0 || index >= static_cast<int>(hits_.size())) {
    return -1;
  }
  return hits_[static_cast<size_t>(index)].id;
}

int ClockFlyoutContent::HitTest(POINT client, UINT dpi) const {
  (void)dpi;
  for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i) {
    if (PtInRect(&hits_[static_cast<size_t>(i)].rc, client) != FALSE) {
      return i;
    }
  }
  return -1;
}

bool ClockFlyoutContent::StickyRow(int index) const {
  return HitIdAt(index) >= 0;
}

void ClockFlyoutContent::Invoke(int index) {
  StickyInvoke(index);
}

void ClockFlyoutContent::ShiftMonth(int delta) {
  int month = view_month_ + delta;
  int year = view_year_;
  while (month < 1) {
    month += 12;
    --year;
  }
  while (month > 12) {
    month -= 12;
    ++year;
  }
  if (year < 1601 || year > 30827) {
    return;
  }
  view_year_ = year;
  view_month_ = month;
}

void ClockFlyoutContent::SelectCell(int index) {
  const int offset = FirstDayOffset();
  const int dim = DaysInMonth(view_year_, view_month_);
  const int day = index - offset + 1;
  if (day < 1) {
    ShiftMonth(-1);
    sel_day_ = DaysInMonth(view_year_, view_month_) + day;
  } else if (day > dim) {
    ShiftMonth(1);
    sel_day_ = day - dim;
  } else {
    sel_day_ = day;
  }
  sel_year_ = view_year_;
  sel_month_ = view_month_;
}

void ClockFlyoutContent::StickyInvoke(int index) {
  const int id = HitIdAt(index);
  switch (id) {
    case kQuietHours:
      OpenUri(L"ms-settings:quiethours");
      break;
    case kClearAll:
      notes_.clear();
      break;
    case kDateHeader:
      calendar_open_ = !calendar_open_;
      break;
    case kMonthPrev:
      ShiftMonth(-1);
      break;
    case kMonthNext:
      ShiftMonth(1);
      break;
    case kFocusMinus:
      focus_minutes_ = (std::max)(kFocusMin, focus_minutes_ - kFocusStep);
      break;
    case kFocusPlus:
      focus_minutes_ = (std::min)(kFocusMax, focus_minutes_ + kFocusStep);
      break;
    case kFocusStart:
      OpenUri(L"ms-clock:");
      break;
    default:
      if (id >= kDay0 && id < kDay0 + 7 * kWeeks) {
        SelectCell(id - kDay0);
      }
      break;
  }
}

void ClockFlyoutContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr) {
    return;
  }
  EnsureFormats(dpi);
  BuildHits(dpi);
  const bool dark = dark_;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(ClockTextColor(dark), brush.GetAddressOf());
  if (!brush) {
    return;
  }
  const int hot_id = HitIdAt(hot_index);
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  const float radius = static_cast<float>(DipToPx(kCardRadiusDip, dpi));
  auto fill_round = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               radius, radius};
    target->FillRoundedRectangle(rr, brush.Get());
  };
  auto stroke_round = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left) + 0.5f, static_cast<float>(rc.top) + 0.5f,
                                           static_cast<float>(rc.right) - 0.5f, static_cast<float>(rc.bottom) - 0.5f),
                               radius, radius};
    target->DrawRoundedRectangle(rr, brush.Get(), 1.0f);
  };

  const RECT notify = NotifyRect(dpi);
  const RECT cal = CalendarRect(dpi);
  fill_round(notify, DockFillColor(dark));
  stroke_round(notify, DockStrokeColor(dark));
  fill_round(cal, DockFillColor(dark));
  stroke_round(cal, DockStrokeColor(dark));

  const int npad = DipToPx(kNotifyPadDip, dpi);
  const int header_h = DipToPx(kNotifyHeaderDip, dpi);
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), semibold14_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(notify.left + npad), static_cast<float>(notify.top + npad),
                          static_cast<float>(notify.right - npad - DipToPx(40, dpi)),
                          static_cast<float>(notify.top + npad + header_h)),
              L"알림");

  auto hover_fill = [&](int id, const RECT& rc) {
    if (hot_id == id) {
      const float rr = static_cast<float>(DipToPx(4, dpi));
      brush->SetColor(MenuItemHoverFill(dark, false));
      target->FillRoundedRectangle(
          D2D1_ROUNDED_RECT{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                        static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                            rr, rr},
          brush.Get());
    }
  };

  RECT quiet{};
  RECT clear{};
  bool has_clear = false;
  for (const Hit& hit : hits_) {
    if (hit.id == kQuietHours) {
      quiet = hit.rc;
    } else if (hit.id == kClearAll) {
      clear = hit.rc;
      has_clear = true;
    }
  }
  if (has_clear) {
    hover_fill(kClearAll, clear);
    brush->SetColor(hot_id == kClearAll ? fg : muted);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(clear.left), static_cast<float>(clear.top),
                            static_cast<float>(clear.right), static_cast<float>(clear.bottom)),
                L"모두 지우기");
  }
  hover_fill(kQuietHours, quiet);
  brush->SetColor(hot_id == kQuietHours ? fg : muted);
  if (dwrite_ && fluent16_) {
    DrawGlyph(target, dwrite_.Get(), fluent16_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(quiet.left), static_cast<float>(quiet.top),
                          static_cast<float>(quiet.right), static_cast<float>(quiet.bottom)),
              kQuietGlyph);
  }

  if (notes_.empty()) {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), center12_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(notify.left + npad),
                            static_cast<float>(notify.top + npad + header_h),
                            static_cast<float>(notify.right - npad), static_cast<float>(notify.bottom - npad)),
                L"새 알림 없음");
  } else {
    int y = notify.top + npad + header_h;
    const int note_h = DipToPx(kNoteDip, dpi);
    const int gap = DipToPx(kNoteGapDip, dpi);
    const int inner = DipToPx(12, dpi);
    for (const Note& note : notes_) {
      RECT card{notify.left + npad, y, notify.right - npad, y + note_h};
      fill_round(card, CardFillColor(dark));
      brush->SetColor(fg);
      DrawTrimmed(target, dwrite_.Get(), semibold13_.Get(), brush.Get(),
                  D2D1::RectF(static_cast<float>(card.left + inner), static_cast<float>(card.top + DipToPx(8, dpi)),
                              static_cast<float>(card.right - inner - DipToPx(64, dpi)),
                              static_cast<float>(card.top + DipToPx(26, dpi))),
                  note.app);
      brush->SetColor(muted);
      DrawTrimmed(target, dwrite_.Get(), muted11_.Get(), brush.Get(),
                  D2D1::RectF(static_cast<float>(card.right - inner - DipToPx(72, dpi)),
                              static_cast<float>(card.top + DipToPx(8, dpi)),
                              static_cast<float>(card.right - inner), static_cast<float>(card.top + DipToPx(26, dpi))),
                  note.when);
      brush->SetColor(fg);
      DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush.Get(),
                  D2D1::RectF(static_cast<float>(card.left + inner), static_cast<float>(card.top + DipToPx(28, dpi)),
                              static_cast<float>(card.right - inner), static_cast<float>(card.top + DipToPx(48, dpi))),
                  note.title);
      brush->SetColor(muted);
      DrawTrimmed(target, dwrite_.Get(), muted11_.Get(), brush.Get(),
                  D2D1::RectF(static_cast<float>(card.left + inner), static_cast<float>(card.top + DipToPx(48, dpi)),
                              static_cast<float>(card.right - inner), static_cast<float>(card.bottom - DipToPx(8, dpi))),
                  note.body);
      y += note_h + gap;
      if (y > notify.bottom - npad) {
        break;
      }
    }
  }

  const int cpad = DipToPx(kCalPadDip, dpi);
  RECT date_hdr{};
  for (const Hit& hit : hits_) {
    if (hit.id == kDateHeader) {
      date_hdr = hit.rc;
      break;
    }
  }
  hover_fill(kDateHeader, date_hdr);
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), semibold14_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(cal.left + cpad), static_cast<float>(date_hdr.top),
                          static_cast<float>(date_hdr.right - DipToPx(8, dpi)), static_cast<float>(date_hdr.bottom)),
              HeaderDate(today_));
  brush->SetColor(hot_id == kDateHeader ? fg : muted);
  if (dwrite_ && fluent14_) {
    DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(date_hdr.right - DipToPx(28, dpi)), static_cast<float>(date_hdr.top),
                          static_cast<float>(date_hdr.right), static_cast<float>(date_hdr.bottom)),
              calendar_open_ ? kChevronDown : kChevronUp);
  }

  if (calendar_open_) {
    RECT prev{};
    RECT next{};
    for (const Hit& hit : hits_) {
      if (hit.id == kMonthPrev) {
        prev = hit.rc;
      } else if (hit.id == kMonthNext) {
        next = hit.rc;
      }
    }
    const int nav_y = date_hdr.bottom;
    const int nav_h = DipToPx(kMonthNavDip, dpi);
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), semibold13_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(cal.left + cpad), static_cast<float>(nav_y),
                            static_cast<float>(prev.left - DipToPx(8, dpi)), static_cast<float>(nav_y + nav_h)),
                MonthTitle(view_year_, view_month_));
    hover_fill(kMonthPrev, prev);
    hover_fill(kMonthNext, next);
    brush->SetColor(hot_id == kMonthPrev ? fg : muted);
    if (dwrite_ && fluent14_) {
      DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(prev.left), static_cast<float>(prev.top), static_cast<float>(prev.right),
                            static_cast<float>(prev.bottom)),
                kChevronUp);
    }
    brush->SetColor(hot_id == kMonthNext ? fg : muted);
    if (dwrite_ && fluent14_) {
      DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(next.left), static_cast<float>(next.top), static_cast<float>(next.right),
                            static_cast<float>(next.bottom)),
                kChevronDown);
    }

    const int offset = FirstDayOffset();
    const int dim = DaysInMonth(view_year_, view_month_);
    const int weekday_y = nav_y + nav_h;
    const int weekday_h = DipToPx(kWeekdayDip, dpi);
    for (int i = 0; i < 7; ++i) {
      const RECT cell = DayCell(dpi, i);
      brush->SetColor(muted);
      DrawTrimmed(target, dwrite_.Get(), center11_.Get(), brush.Get(),
                  D2D1::RectF(static_cast<float>(cell.left), static_cast<float>(weekday_y),
                              static_cast<float>(cell.right), static_cast<float>(weekday_y + weekday_h)),
                  WeekdayLabel((first_weekday_ + i) % 7));
    }

    int prev_year = view_year_;
    int prev_month = view_month_ - 1;
    if (prev_month < 1) {
      prev_month = 12;
      --prev_year;
    }
    const int prev_dim = DaysInMonth(prev_year, prev_month);
    for (int i = 0; i < 7 * kWeeks; ++i) {
      const RECT cell = DayCell(dpi, i);
      const int day = i - offset + 1;
      int y = view_year_;
      int m = view_month_;
      int d = day;
      bool other = false;
      if (day < 1) {
        y = prev_year;
        m = prev_month;
        d = prev_dim + day;
        other = true;
      } else if (day > dim) {
        y = view_month_ == 12 ? view_year_ + 1 : view_year_;
        m = view_month_ == 12 ? 1 : view_month_ + 1;
        d = day - dim;
        other = true;
      }
      const bool today = y == today_.wYear && m == today_.wMonth && d == today_.wDay;
      const bool selected = y == sel_year_ && m == sel_month_ && d == sel_day_;
      const float cx = static_cast<float>(cell.left + cell.right) * 0.5f;
      const float cy = static_cast<float>(cell.top + cell.bottom) * 0.5f;
      const float r = static_cast<float>(DipToPx(14, dpi));
      if (hot_id == kDay0 + i && !today) {
        brush->SetColor(MenuItemHoverFill(dark, false));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush.Get());
      }
      if (today) {
        brush->SetColor(AccentFillColor(dark));
        target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush.Get());
      } else if (selected) {
        brush->SetColor(AccentFillColor(dark));
        target->DrawEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r - 0.5f, r - 0.5f), brush.Get(), 1.5f);
      }
      wchar_t label[8]{};
      swprintf_s(label, L"%d", d);
      if (today) {
        brush->SetColor(AccentOnColor(dark));
      } else if (other) {
        brush->SetColor(ScaleAlpha(fg, 0.38f));
      } else {
        brush->SetColor(fg);
      }
      DrawTrimmed(target, dwrite_.Get(), center12_.Get(), brush.Get(),
                  D2D1::RectF(static_cast<float>(cell.left), static_cast<float>(cell.top),
                              static_cast<float>(cell.right), static_cast<float>(cell.bottom)),
                  label);
    }
  }

  RECT minus{};
  RECT plus{};
  RECT start{};
  for (const Hit& hit : hits_) {
    if (hit.id == kFocusMinus) {
      minus = hit.rc;
    } else if (hit.id == kFocusPlus) {
      plus = hit.rc;
    } else if (hit.id == kFocusStart) {
      start = hit.rc;
    }
  }
  hover_fill(kFocusMinus, minus);
  hover_fill(kFocusPlus, plus);
  hover_fill(kFocusStart, start);
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), center12_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(minus.left), static_cast<float>(minus.top),
                          static_cast<float>(minus.right), static_cast<float>(minus.bottom)),
              L"\u2212");
  wchar_t minutes[32]{};
  swprintf_s(minutes, L"%d 분", focus_minutes_);
  DrawTrimmed(target, dwrite_.Get(), center12_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(minus.right), static_cast<float>(minus.top),
                          static_cast<float>(plus.left), static_cast<float>(plus.bottom)),
              minutes);
  DrawTrimmed(target, dwrite_.Get(), center12_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(plus.left), static_cast<float>(plus.top), static_cast<float>(plus.right),
                          static_cast<float>(plus.bottom)),
              L"+");
  if (dwrite_ && fluent14_) {
    brush->SetColor(hot_id == kFocusStart ? fg : muted);
    const float play_w = static_cast<float>(DipToPx(18, dpi));
    DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(start.left), static_cast<float>(start.top),
                          static_cast<float>(start.left) + play_w, static_cast<float>(start.bottom)),
              kPlayGlyph);
  }
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), regular13_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(start.left + DipToPx(18, dpi)), static_cast<float>(start.top),
                          static_cast<float>(start.right), static_cast<float>(start.bottom)),
              L"집중");
}

void ClockMenuContent::Reset(bool dark) {
  dark_ = dark;
}

RECT ClockMenuContent::RowRect(int index, UINT dpi) const {
  const int pad = DipToPx(6, dpi);
  const int row = DipToPx(36, dpi);
  const int y = pad + index * row;
  const int width = width_px_ > 0 ? width_px_ : DipToPx(220, dpi);
  return RECT{pad, y, width - pad, y + row};
}

SIZE ClockMenuContent::Measure(UINT dpi) {
  const int pad = DipToPx(6, dpi);
  const int row = DipToPx(36, dpi);
  const int icon = DipToPx(28, dpi);
  const float w1 = PopupTextWidth(dpi, L"날짜 및 시간 조정");
  const float w2 = PopupTextWidth(dpi, L"알림 설정");
  int width = static_cast<int>((std::max)(w1, w2) + 0.5f) + pad * 2 + icon + DipToPx(20, dpi);
  width = (std::max)(width, DipToPx(200, dpi));
  width_px_ = width;
  return SIZE{width, pad * 2 + row * 2};
}

void ClockMenuContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr) {
    return;
  }
  const D2D1_SIZE_F sz = target->GetSize();
  width_px_ = static_cast<int>(sz.width);
  const D2D1_COLOR_F fg = ClockTextColor(dark_);
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(fg, brush.GetAddressOf());
  if (!brush) {
    return;
  }
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent;
  const float s = static_cast<float>(dpi) / 96.0f;
  if (dwrite) {
    MakeFormat(dwrite.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 14.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent);
  }
  const wchar_t* labels[] = {L"날짜 및 시간 조정", L"알림 설정"};
  for (int i = 0; i < 2; ++i) {
    const RECT rc = RowRect(i, dpi);
    if (i == hot_index) {
      brush->SetColor(MenuItemHoverFill(dark_, false));
      const float rr = static_cast<float>(DipToPx(4, dpi));
      target->FillRoundedRectangle(
          D2D1_ROUNDED_RECT{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                        static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                            rr, rr},
          brush.Get());
    }
    const int icon = DipToPx(28, dpi);
    brush->SetColor(ScaleAlpha(fg, 0.85f));
    if (dwrite && fluent) {
      DrawGlyph(target, dwrite.Get(), fluent.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                            static_cast<float>(rc.left + icon), static_cast<float>(rc.bottom)),
                kGearGlyph);
    }
    brush->SetColor(fg);
    DrawPopupText(target, dpi, labels[i],
                  D2D1::RectF(static_cast<float>(rc.left + icon), static_cast<float>(rc.top),
                              static_cast<float>(rc.right - DipToPx(8, dpi)), static_cast<float>(rc.bottom)),
                  brush.Get());
  }
}

int ClockMenuContent::HitTest(POINT client, UINT dpi) const {
  for (int i = 0; i < 2; ++i) {
    const RECT rc = RowRect(i, dpi);
    if (PtInRect(&rc, client) != FALSE) {
      return i;
    }
  }
  return -1;
}

void ClockMenuContent::Invoke(int index) {
  if (index == 0) {
    OpenUri(L"ms-settings:dateandtime");
  } else if (index == 1) {
    OpenUri(L"ms-settings:notifications");
  }
}

}  // namespace bamti
