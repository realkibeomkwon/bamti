#include "status_panel.hpp"

#include "log.hpp"
#include "theme.hpp"

#include <d2d1helper.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>

namespace bamti {
namespace {

constexpr int kPanelPadDip = 12;
constexpr int kPanelMinWidthDip = 280;
constexpr int kPanelMaxWidthDip = 360;
constexpr int kPanelTitleDip = 22;
constexpr int kPanelSubDip = 18;
constexpr int kPanelGaugeLabelDip = 18;
constexpr int kPanelGaugeBarDip = 6;
constexpr int kPanelGaugeNoteDip = 16;
constexpr int kPanelGaugeGapDip = 10;
constexpr int kPanelSepDip = 9;
constexpr int kPanelActionDip = 28;
constexpr int kPanelKvDip = 18;
constexpr int kPanelTextDip = 18;
constexpr int kToggleTrackWDip = 34;
constexpr int kToggleTrackHDip = 18;
constexpr int kButtonGapDip = 8;
constexpr int kButtonsPerLine = 3;
constexpr int kPanelSliderLabelDip = 18;
constexpr int kPanelSliderTrackDip = 6;
constexpr int kPanelSliderThumbDip = 14;
constexpr int kPanelSliderPadDip = 8;
constexpr int kPanelSliderGapDip = 10;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

float ClampUnit(float value) {
  if (!std::isfinite(value) || value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

struct SliderGeometry {
  float lo = 0.0f;
  float hi = 0.0f;
  float thumb_r = 0.0f;
};

SliderGeometry SliderGeom(float left, float right, UINT dpi) {
  const float d = static_cast<float>(DipToPx(kPanelSliderThumbDip, dpi));
  const float r = d * 0.5f;
  return SliderGeometry{left + r, right - r, r};
}

int ButtonRunLen(const std::vector<StatusRow>& rows, size_t start) {
  int n = 0;
  while (start + static_cast<size_t>(n) < rows.size() && rows[start + static_cast<size_t>(n)].type == RowType::kButton) {
    ++n;
  }
  return n;
}

void NoteHitOutOfRange(int hit_i, size_t n, int row, size_t rows) {
  static bool logged = false;
  if (logged) {
    return;
  }
  logged = true;
  Log(L"panel", L"render hit out of range i=%d hits=%zu row=%d rows=%zu", hit_i, n, row, rows);
}

}  // namespace

void StatusPanelContent::Reset(StatusItem item, StatusPanelHost host) {
  item_ = std::move(item);
  host_ = std::move(host);
  drag_row_ = -1;
  // hits_는 Open 때 Measure가 채운다. 값만 바꿀 때는 호버/히트 영역을 유지한다.
}

int StatusPanelContent::RowCount() const {
  return static_cast<int>(hits_.size());
}

int StatusPanelContent::HitRow(int index) const {
  if (index < 0 || index >= static_cast<int>(hits_.size())) {
    return -1;
  }
  return hits_[static_cast<size_t>(index)].row;
}

SIZE StatusPanelContent::Measure(UINT dpi) {
  hits_.clear();
  const StatusPanel* panel = item_.panel ? &*item_.panel : nullptr;
  if (panel == nullptr) {
    return SIZE{};
  }

  const int pad = DipToPx(kPanelPadDip, dpi);
  int inner = 0;
  auto consider = [&](const std::wstring& text) {
    if (!text.empty()) {
      inner = (std::max)(inner, static_cast<int>(PopupTextWidth(dpi, text) + 0.5f));
    }
  };
  consider(panel->title);
  consider(panel->subtitle);
  consider(panel->updated_text);
  for (const StatusRow& row : panel->rows) {
    if (row.type == RowType::kGauge) {
      const int label = static_cast<int>(PopupTextWidth(dpi, row.label) + 0.5f);
      const int right = static_cast<int>(PopupTextWidth(dpi, row.value_text.empty() ? row.detail : row.value_text) + 0.5f);
      inner = (std::max)(inner, label + DipToPx(12, dpi) + right);
      consider(row.note);
      if (!row.value_text.empty()) {
        consider(row.detail);
      }
    } else if (row.type == RowType::kKeyValue) {
      inner = (std::max)(inner, static_cast<int>(PopupTextWidth(dpi, row.label) + PopupTextWidth(dpi, row.value_text) +
                                                 DipToPx(16, dpi) + 0.5f));
    } else if (row.type == RowType::kText) {
      consider(row.label);
    } else if (row.type == RowType::kToggle) {
      consider(row.label);
      inner = (std::max)(inner, static_cast<int>(PopupTextWidth(dpi, row.label) + 0.5f) + DipToPx(kToggleTrackWDip + 12, dpi));
    } else if (row.type == RowType::kSlider) {
      const int label = static_cast<int>(PopupTextWidth(dpi, row.label) + 0.5f);
      const int right = static_cast<int>(PopupTextWidth(dpi, row.value_text) + 0.5f);
      inner = (std::max)(inner, label + DipToPx(12, dpi) + right);
    } else if (row.type == RowType::kButton) {
      consider(row.label);
    }
  }

  int width = inner + pad * 2;
  width = (std::max)(width, DipToPx(kPanelMinWidthDip, dpi));
  width = (std::min)(width, DipToPx(kPanelMaxWidthDip, dpi));

  int y = pad;
  if (!panel->title.empty()) {
    y += DipToPx(kPanelTitleDip, dpi);
  }
  if (!panel->subtitle.empty()) {
    y += DipToPx(kPanelSubDip, dpi);
  }
  if (!panel->updated_text.empty()) {
    y += DipToPx(kPanelSubDip, dpi);
  }

  const auto& rows = panel->rows;
  for (size_t i = 0; i < rows.size();) {
    const StatusRow& row = rows[i];
    if (row.type == RowType::kButton) {
      const int run = ButtonRunLen(rows, i);
      const int h = DipToPx(kPanelActionDip, dpi);
      const int gap = DipToPx(kButtonGapDip, dpi);
      const int inner_w = width - pad * 2;
      int drawn = 0;
      while (drawn < run) {
        const int count = (std::min)(kButtonsPerLine, run - drawn);
        const int cell = (inner_w - gap * (count - 1)) / count;
        int x = pad;
        for (int c = 0; c < count; ++c) {
          Hit hit;
          hit.row = static_cast<int>(i + static_cast<size_t>(drawn + c));
          hit.rc = RECT{x, y, x + cell, y + h};
          hits_.push_back(hit);
          x += cell + gap;
        }
        y += h;
        drawn += count;
      }
      i += static_cast<size_t>(run);
      continue;
    }
    if (row.type == RowType::kGauge) {
      y += DipToPx(kPanelGaugeLabelDip, dpi);
      y += DipToPx(kPanelGaugeBarDip, dpi);
      if (!row.value_text.empty() && !row.detail.empty()) {
        y += DipToPx(kPanelGaugeNoteDip, dpi);
      }
      if (!row.note.empty()) {
        y += DipToPx(kPanelGaugeNoteDip, dpi);
      }
      y += DipToPx(kPanelGaugeGapDip, dpi);
    } else if (row.type == RowType::kKeyValue) {
      y += DipToPx(kPanelKvDip, dpi);
    } else if (row.type == RowType::kText) {
      y += DipToPx(kPanelTextDip, dpi);
    } else if (row.type == RowType::kSeparator) {
      y += DipToPx(kPanelSepDip, dpi);
    } else if (row.type == RowType::kToggle) {
      const int h = DipToPx(kPanelActionDip, dpi);
      Hit hit;
      hit.row = static_cast<int>(i);
      hit.rc = RECT{pad, y, width - pad, y + h};
      hits_.push_back(hit);
      y += h;
    } else if (row.type == RowType::kSlider) {
      y += DipToPx(kPanelSliderLabelDip, dpi);
      const int track = DipToPx(kPanelSliderTrackDip, dpi);
      const int spad = DipToPx(kPanelSliderPadDip, dpi);
      Hit hit;
      hit.row = static_cast<int>(i);
      hit.rc = RECT{pad, y, width - pad, y + spad * 2 + track};
      hits_.push_back(hit);
      y += spad * 2 + track + DipToPx(kPanelSliderGapDip, dpi);
    }
    ++i;
  }
  y += pad;
  return SIZE{width, y};
}

void StatusPanelContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr || !item_.panel) {
    return;
  }
  const StatusPanel& panel = *item_.panel;
  const bool dark = host_.dark;
  const D2D1_SIZE_F sz = target->GetSize();
  const int width = static_cast<int>(sz.width);
  const int pad = DipToPx(kPanelPadDip, dpi);

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> muted;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> line;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> track;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> fill;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> warn;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> thumb;
  const D2D1_COLOR_F text_c = ClockTextColor(dark);
  D2D1_COLOR_F muted_c = text_c;
  muted_c.a *= 0.65f;
  const D2D1_COLOR_F fill_c = item_.accent != 0 ? D2D1::ColorF(item_.accent) : DockIndicatorColor(dark);
  if (FAILED(target->CreateSolidColorBrush(text_c, text.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(muted_c, muted.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(MenuItemHoverFill(dark, false), hover.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(DockStrokeColor(dark), line.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(D2D1::ColorF(text_c.r, text_c.g, text_c.b, 0.18f), track.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(fill_c, fill.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(WarningTextColor(dark), warn.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.92f), thumb.GetAddressOf()))) {
    return;
  }

  int y = pad;
  auto draw_line = [&](const std::wstring& s, int height, ID2D1Brush* brush) {
    if (s.empty()) {
      return;
    }
    DrawPopupText(target, dpi, s,
                  D2D1::RectF(static_cast<float>(pad), static_cast<float>(y), static_cast<float>(width - pad),
                              static_cast<float>(y + height)),
                  brush);
    y += height;
  };
  draw_line(panel.title, DipToPx(kPanelTitleDip, dpi), text.Get());
  draw_line(panel.subtitle, DipToPx(kPanelSubDip, dpi), muted.Get());
  draw_line(panel.updated_text, DipToPx(kPanelSubDip, dpi), muted.Get());

  const int label_h = DipToPx(kPanelGaugeLabelDip, dpi);
  const int bar_h = DipToPx(kPanelGaugeBarDip, dpi);
  const int note_h = DipToPx(kPanelGaugeNoteDip, dpi);
  const int gap = DipToPx(kPanelGaugeGapDip, dpi);
  const auto& rows = panel.rows;
  int hit_i = 0;
  for (size_t i = 0; i < rows.size();) {
    const StatusRow& row = rows[i];
    const float left = static_cast<float>(pad);
    const float right = static_cast<float>(width - pad);
    if (row.type == RowType::kButton) {
      const int run = ButtonRunLen(rows, i);
      const int h = DipToPx(kPanelActionDip, dpi);
      int drawn = 0;
      while (drawn < run) {
        const int count = (std::min)(kButtonsPerLine, run - drawn);
        for (int c = 0; c < count; ++c) {
          if (static_cast<size_t>(hit_i) >= hits_.size()) {
            NoteHitOutOfRange(hit_i, hits_.size(), -1, rows.size());
            continue;
          }
          const Hit& hit = hits_[static_cast<size_t>(hit_i++)];
          if (hit.row < 0 || hit.row >= static_cast<int>(rows.size())) {
            NoteHitOutOfRange(hit_i - 1, hits_.size(), hit.row, rows.size());
            continue;
          }
          const StatusRow& button = rows[static_cast<size_t>(hit.row)];
          const float l = static_cast<float>(hit.rc.left);
          const float t = static_cast<float>(hit.rc.top);
          const float r = static_cast<float>(hit.rc.right);
          const float b = static_cast<float>(hit.rc.bottom);
          if (hot_index == hit_i - 1) {
            target->FillRectangle(D2D1::RectF(l, t, r, b), hover.Get());
          }
          DrawPopupText(target, dpi, button.label, D2D1::RectF(l, t, r, b),
                        button.danger ? warn.Get() : text.Get());
        }
        y += h;
        drawn += count;
      }
      i += static_cast<size_t>(run);
      continue;
    }
    if (row.type == RowType::kGauge) {
      const bool value_right = !row.value_text.empty();
      DrawPopupText(target, dpi, row.label,
                    D2D1::RectF(left, static_cast<float>(y), right * 0.55f, static_cast<float>(y + label_h)),
                    text.Get());
      DrawPopupText(target, dpi, value_right ? row.value_text : row.detail,
                    D2D1::RectF(right * 0.55f, static_cast<float>(y), right, static_cast<float>(y + label_h)),
                    muted.Get());
      y += label_h;
      const float bar_top = static_cast<float>(y);
      const float bar_bottom = bar_top + static_cast<float>(bar_h);
      target->FillRectangle(D2D1::RectF(left, bar_top, right, bar_bottom), track.Get());
      const float filled = left + (right - left) * row.value;
      if (filled > left) {
        target->FillRectangle(D2D1::RectF(left, bar_top, filled, bar_bottom), fill.Get());
      }
      y += bar_h;
      if (value_right && !row.detail.empty()) {
        DrawPopupText(target, dpi, row.detail,
                      D2D1::RectF(left, static_cast<float>(y), right, static_cast<float>(y + note_h)), muted.Get());
        y += note_h;
      }
      if (!row.note.empty()) {
        DrawPopupText(target, dpi, row.note,
                      D2D1::RectF(left, static_cast<float>(y), right, static_cast<float>(y + note_h)), fill.Get());
        y += note_h;
      }
      y += gap;
    } else if (row.type == RowType::kKeyValue) {
      const int h = DipToPx(kPanelKvDip, dpi);
      DrawPopupText(target, dpi, row.label,
                    D2D1::RectF(left, static_cast<float>(y), right * 0.55f, static_cast<float>(y + h)), text.Get());
      DrawPopupText(target, dpi, row.value_text,
                    D2D1::RectF(right * 0.55f, static_cast<float>(y), right, static_cast<float>(y + h)), muted.Get());
      y += h;
    } else if (row.type == RowType::kText) {
      const int h = DipToPx(kPanelTextDip, dpi);
      DrawPopupText(target, dpi, row.label, D2D1::RectF(left, static_cast<float>(y), right, static_cast<float>(y + h)),
                    row.muted ? muted.Get() : text.Get());
      y += h;
    } else if (row.type == RowType::kSeparator) {
      const float mid = static_cast<float>(y + DipToPx(kPanelSepDip, dpi) / 2) + 0.5f;
      target->DrawLine(D2D1::Point2F(left, mid), D2D1::Point2F(right, mid), line.Get(), 1.0f);
      y += DipToPx(kPanelSepDip, dpi);
    } else if (row.type == RowType::kToggle) {
      if (static_cast<size_t>(hit_i) >= hits_.size()) {
        NoteHitOutOfRange(hit_i, hits_.size(), static_cast<int>(i), rows.size());
      } else {
        const Hit& hit = hits_[static_cast<size_t>(hit_i++)];
        if (hit.row < 0 || hit.row >= static_cast<int>(rows.size())) {
          NoteHitOutOfRange(hit_i - 1, hits_.size(), hit.row, rows.size());
        } else {
          const float t = static_cast<float>(hit.rc.top);
          const float b = static_cast<float>(hit.rc.bottom);
          if (hot_index == hit_i - 1) {
            target->FillRectangle(D2D1::RectF(static_cast<float>(hit.rc.left), t, static_cast<float>(hit.rc.right), b),
                                  hover.Get());
          }
          const int track_w = DipToPx(kToggleTrackWDip, dpi);
          const int track_h = DipToPx(kToggleTrackHDip, dpi);
          const float track_l = right - static_cast<float>(track_w);
          const float track_t = t + (b - t - static_cast<float>(track_h)) * 0.5f;
          const float radius = static_cast<float>(track_h) * 0.5f;
          const D2D1_ROUNDED_RECT track_rc{D2D1::RectF(track_l, track_t, right, track_t + static_cast<float>(track_h)),
                                           radius, radius};
          target->FillRoundedRectangle(track_rc, row.on ? fill.Get() : track.Get());
          const float thumb_r = radius - 2.0f;
          const float thumb_cx = row.on ? right - radius : track_l + radius;
          const float thumb_cy = track_t + radius;
          target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(thumb_cx, thumb_cy), thumb_r, thumb_r), thumb.Get());
          DrawPopupText(target, dpi, row.label, D2D1::RectF(left, t, track_l - 8.0f, b), text.Get());
        }
      }
      y += DipToPx(kPanelActionDip, dpi);
    } else if (row.type == RowType::kSlider) {
      const int s_label_h = DipToPx(kPanelSliderLabelDip, dpi);
      const int s_track_h = DipToPx(kPanelSliderTrackDip, dpi);
      const int spad = DipToPx(kPanelSliderPadDip, dpi);
      const int sgap = DipToPx(kPanelSliderGapDip, dpi);
      DrawPopupText(target, dpi, row.label,
                    D2D1::RectF(left, static_cast<float>(y), right * 0.55f, static_cast<float>(y + s_label_h)),
                    text.Get());
      DrawPopupText(target, dpi, row.value_text,
                    D2D1::RectF(right * 0.55f, static_cast<float>(y), right, static_cast<float>(y + s_label_h)),
                    muted.Get());
      y += s_label_h;
      if (static_cast<size_t>(hit_i) >= hits_.size()) {
        NoteHitOutOfRange(hit_i, hits_.size(), static_cast<int>(i), rows.size());
      } else {
        const Hit& hit = hits_[static_cast<size_t>(hit_i++)];
        if (hit.row < 0 || hit.row >= static_cast<int>(rows.size())) {
          NoteHitOutOfRange(hit_i - 1, hits_.size(), hit.row, rows.size());
        } else {
          const float track_top = static_cast<float>(hit.rc.top + spad);
          const float track_bottom = track_top + static_cast<float>(s_track_h);
          const float radius = static_cast<float>(s_track_h) * 0.5f;
          const D2D1_ROUNDED_RECT track_rc{D2D1::RectF(left, track_top, right, track_bottom), radius, radius};
          target->FillRoundedRectangle(track_rc, track.Get());
          const float value = ClampUnit(row.value);
          const SliderGeometry geom = SliderGeom(left, right, dpi);
          const float cx = geom.hi > geom.lo ? geom.lo + (geom.hi - geom.lo) * value : geom.lo;
          const float cy = track_top + radius;
          if (cx > left) {
            const D2D1_ROUNDED_RECT fill_rc{D2D1::RectF(left, track_top, cx, track_bottom), radius, radius};
            target->FillRoundedRectangle(fill_rc, fill.Get());
          }
          float draw_r = geom.thumb_r;
          if (hot_index == hit_i - 1 || drag_row_ == static_cast<int>(i)) {
            draw_r += 2.0f;
          }
          target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), draw_r, draw_r), thumb.Get());
        }
      }
      y += spad * 2 + s_track_h + sgap;
    }
    ++i;
  }
}

int StatusPanelContent::HitTest(POINT client, UINT dpi) const {
  (void)dpi;
  for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
    if (PtInRect(&hits_[static_cast<size_t>(i)].rc, client)) {
      return i;
    }
  }
  return -1;
}

bool StatusPanelContent::StickyRow(int index) const {
  const int row = HitRow(index);
  if (row < 0 || !item_.panel || row >= static_cast<int>(item_.panel->rows.size())) {
    return false;
  }
  return item_.panel->rows[static_cast<size_t>(row)].type == RowType::kToggle;
}

void StatusPanelContent::Invoke(int index) {
  Activate(HitRow(index));
}

void StatusPanelContent::StickyInvoke(int index) {
  Activate(HitRow(index));
}

bool StatusPanelContent::DragRow(int index) const {
  const int row = HitRow(index);
  if (row < 0 || !item_.panel || row >= static_cast<int>(item_.panel->rows.size())) {
    return false;
  }
  return item_.panel->rows[static_cast<size_t>(row)].type == RowType::kSlider;
}

void StatusPanelContent::DragTo(int index, POINT client, UINT dpi) {
  if (!item_.panel || index < 0 || index >= static_cast<int>(hits_.size())) {
    return;
  }
  const int row = hits_[static_cast<size_t>(index)].row;
  if (row < 0 || row >= static_cast<int>(item_.panel->rows.size())) {
    return;
  }
  StatusRow& target = item_.panel->rows[static_cast<size_t>(row)];
  if (target.type != RowType::kSlider) {
    return;
  }
  const RECT& rc = hits_[static_cast<size_t>(index)].rc;
  const SliderGeometry geom = SliderGeom(static_cast<float>(rc.left), static_cast<float>(rc.right), dpi);
  float v = geom.hi > geom.lo ? (static_cast<float>(client.x) - geom.lo) / (geom.hi - geom.lo) : 0.0f;
  v = ClampUnit(v);
  v = std::round(v / 0.02f) * 0.02f;
  v = ClampUnit(v);
  if (drag_row_ == row && v == drag_value_) {
    return;
  }
  target.value = v;
  wchar_t buf[16]{};
  swprintf_s(buf, L"%d%%", static_cast<int>(v * 100.0f + 0.5f));
  target.value_text = buf;
  drag_row_ = row;
  drag_value_ = v;
  if (!host_.dispatch) {
    return;
  }
  StatusEvent ev;
  ev.id = item_.id;
  ev.event = "slide";
  ev.row_id = target.row_id;
  ev.value = v;
  host_.dispatch(ev);
}

void StatusPanelContent::DragEnd(int index) {
  const float v = drag_value_;
  drag_row_ = -1;
  if (!host_.arm_slider) {
    return;
  }
  const int row = HitRow(index);
  if (row < 0 || !item_.panel || row >= static_cast<int>(item_.panel->rows.size())) {
    return;
  }
  const StatusRow& target = item_.panel->rows[static_cast<size_t>(row)];
  host_.arm_slider(item_.id, target.row_id, item_.revision, v);
}

void StatusPanelContent::Activate(int row) {
  if (!item_.panel || row < 0 || row >= static_cast<int>(item_.panel->rows.size()) || !host_.dispatch) {
    return;
  }
  StatusRow& target = item_.panel->rows[static_cast<size_t>(row)];
  if (target.type == RowType::kToggle) {
    target.on = !target.on;
    StatusEvent ev;
    ev.id = item_.id;
    ev.event = "toggle";
    ev.row_id = target.row_id;
    ev.on = target.on;
    host_.dispatch(ev);
    if (host_.arm_toggle) {
      host_.arm_toggle(item_.id, target.row_id, item_.revision, target.on);
    }
    return;
  }
  if (target.type == RowType::kButton) {
    StatusEvent ev;
    ev.id = item_.id;
    ev.event = "invoke";
    ev.row_id = target.row_id;
    ev.button = WideToUtf8Bytes(target.label);
    host_.dispatch(ev);
  }
}

void OverflowContent::Reset(std::vector<StatusItem> items, OverflowHost host) {
  host_ = std::move(host);
  items_ = std::move(items);
  row_hits_.clear();
}

int OverflowContent::RowCount() const {
  return static_cast<int>(items_.size());
}

SIZE OverflowContent::Measure(UINT dpi) {
  row_hits_.clear();
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int row = DipToPx(kPanelActionDip, dpi);
  int inner = DipToPx(120, dpi);
  for (const StatusItem& item : items_) {
    inner = (std::max)(inner, static_cast<int>(PopupTextWidth(dpi, StatusBarText(item)) + 0.5f));
  }
  const int width = inner + pad * 2;
  int y = pad;
  for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
    row_hits_.push_back(RECT{pad, y, width - pad, y + row});
    y += row;
  }
  y += pad;
  return SIZE{width, y};
}

void OverflowContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr) {
    return;
  }
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> text;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> hover;
  if (FAILED(target->CreateSolidColorBrush(ClockTextColor(host_.dark), text.GetAddressOf())) ||
      FAILED(target->CreateSolidColorBrush(MenuItemHoverFill(host_.dark, false), hover.GetAddressOf()))) {
    return;
  }
  for (int i = 0; i < static_cast<int>(items_.size()); ++i) {
    const RECT& rc = row_hits_[static_cast<size_t>(i)];
    if (i == hot_index) {
      target->FillRectangle(D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                        static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                            hover.Get());
    }
    DrawPopupText(target, dpi, StatusBarText(items_[static_cast<size_t>(i)]),
                  D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.right),
                              static_cast<float>(rc.bottom)),
                  text.Get());
  }
}

int OverflowContent::HitTest(POINT client, UINT dpi) const {
  (void)dpi;
  for (int i = 0; i < static_cast<int>(row_hits_.size()); ++i) {
    if (PtInRect(&row_hits_[static_cast<size_t>(i)], client)) {
      return i;
    }
  }
  return -1;
}

void OverflowContent::Invoke(int index) {
  if (index < 0 || index >= static_cast<int>(items_.size())) {
    return;
  }
  const StatusItem& item = items_[static_cast<size_t>(index)];
  if (item.panel) {
    if (host_.open_panel) {
      host_.open_panel(item.id);
    }
    return;
  }
  if (host_.dispatch) {
    StatusEvent ev;
    ev.id = item.id;
    ev.event = "click";
    ev.button = "left";
    host_.dispatch(ev);
  }
}

}  // namespace bamti
