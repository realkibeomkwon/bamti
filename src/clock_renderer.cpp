#include "clock_renderer.hpp"

#include "theme.hpp"

#include <d2d1helper.h>
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace bamti {
namespace {

constexpr float kFontSizeDip = 13.0f;
constexpr float kPadRightDip = 14.0f;
constexpr float kStatusClockGapDip = 20.0f;
constexpr float kItemGapDip = 14.0f;
constexpr float kStartPadLeftDip = 4.0f;
constexpr float kStartHitWidthDip = 34.0f;
constexpr float kStartLogoDip = 20.0f;
constexpr float kStartHoverInsetXDip = 2.0f;
constexpr float kStartHoverInsetYDip = 4.0f;
constexpr float kStartHoverRadiusDip = 6.0f;
// @WLOGO_96x96.png glyph is 80px with 38px tiles and a 4px gap, color #0078D4.
constexpr float kWindowsLogoGap = 4.0f / 80.0f;

void FillWindowsLogo(ID2D1RenderTarget* rt, ID2D1SolidColorBrush* brush, float left_dip, float top_dip, float size_dip,
                     UINT dpi) {
  const float scale = static_cast<float>(dpi) / 96.0f;
  const int origin_x = static_cast<int>(std::lround(left_dip * scale));
  const int origin_y = static_cast<int>(std::lround(top_dip * scale));
  int size_px = std::max(8, static_cast<int>(std::lround(size_dip * scale)));
  int gap_px = std::max(1, static_cast<int>(std::lround(static_cast<float>(size_px) * kWindowsLogoGap)));
  if ((size_px - gap_px) % 2 != 0) {
    ++gap_px;
  }
  const int tile_px = (size_px - gap_px) / 2;
  const float inv = 96.0f / static_cast<float>(dpi);
  auto pixel_rect = [inv](int x, int y, int w, int h) {
    return D2D1::RectF(static_cast<float>(x) * inv, static_cast<float>(y) * inv,
                       static_cast<float>(x + w) * inv, static_cast<float>(y + h) * inv);
  };

  brush->SetColor(D2D1::ColorF(0x0078D4));
  const D2D1_ANTIALIAS_MODE previous = rt->GetAntialiasMode();
  rt->SetAntialiasMode(D2D1_ANTIALIAS_MODE_ALIASED);
  rt->FillRectangle(pixel_rect(origin_x, origin_y, tile_px, tile_px), brush);
  rt->FillRectangle(pixel_rect(origin_x + tile_px + gap_px, origin_y, tile_px, tile_px), brush);
  rt->FillRectangle(pixel_rect(origin_x, origin_y + tile_px + gap_px, tile_px, tile_px), brush);
  rt->FillRectangle(pixel_rect(origin_x + tile_px + gap_px, origin_y + tile_px + gap_px, tile_px, tile_px), brush);
  rt->SetAntialiasMode(previous);
}

}  // namespace

bool ClockRenderer::Initialize() {
  HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, d2d_.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    return false;
  }

  hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                           reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf()));
  return SUCCEEDED(hr);
}

void ClockRenderer::SetDpi(UINT dpi) {
  if (dpi_ == dpi) {
    return;
  }
  dpi_ = dpi == 0 ? 96 : dpi;
  format_.Reset();
  rt_.Reset();
}

void ClockRenderer::DrawStartButton(ID2D1SolidColorBrush* brush, bool dark, bool hot, bool pressed,
                                    float height_dip) {
  const float hit_left = kStartPadLeftDip;
  const float hit_right = kStartPadLeftDip + kStartHitWidthDip;
  if (hot || pressed) {
    brush->SetColor(MenuItemHoverFill(dark, pressed));
    const D2D1_ROUNDED_RECT hover{
        D2D1::RectF(hit_left + kStartHoverInsetXDip, kStartHoverInsetYDip,
                    hit_right - kStartHoverInsetXDip, height_dip - kStartHoverInsetYDip),
        kStartHoverRadiusDip, kStartHoverRadiusDip};
    rt_->FillRoundedRectangle(hover, brush);
  }

  const float logo_left = hit_left + (kStartHitWidthDip - kStartLogoDip) * 0.5f;
  const float logo_top = (height_dip - kStartLogoDip) * 0.5f;
  FillWindowsLogo(rt_.Get(), brush, logo_left, logo_top, kStartLogoDip, dpi_);
}

bool ClockRenderer::EnsureTextFormat() {
  if (format_) {
    return true;
  }

  wchar_t locale[LOCALE_NAME_MAX_LENGTH]{};
  if (GetUserDefaultLocaleName(locale, LOCALE_NAME_MAX_LENGTH) == 0) {
    locale[0] = L'e';
    locale[1] = L'n';
    locale[2] = L'-';
    locale[3] = L'U';
    locale[4] = L'S';
  }

  const wchar_t* families[] = {L"Segoe UI Variable", L"Segoe UI"};
  HRESULT hr = E_FAIL;
  for (const wchar_t* family : families) {
    hr = dwrite_->CreateTextFormat(family, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                                   DWRITE_FONT_STRETCH_NORMAL, kFontSizeDip, locale, format_.ReleaseAndGetAddressOf());
    if (SUCCEEDED(hr)) {
      break;
    }
  }
  if (FAILED(hr) || !format_) {
    return false;
  }

  format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
  format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
  return true;
}

std::wstring ClockRenderer::CurrentTimeText() const {
  SYSTEMTIME local{};
  GetLocalTime(&local);

  wchar_t date[128]{};
  wchar_t time[64]{};
  const int date_ok =
      GetDateFormatEx(LOCALE_NAME_USER_DEFAULT, DATE_LONGDATE, &local, nullptr, date, 128, nullptr);
  const int time_ok = GetTimeFormatEx(LOCALE_NAME_USER_DEFAULT, 0, &local, nullptr, time, 64);
  if (date_ok <= 1 || time_ok <= 1) {
    return L"";
  }

  std::wstring text;
  text.append(date);
  text.push_back(L' ');
  text.append(time);
  return text;
}

bool ClockRenderer::MakeLayout(const std::wstring& text, float width_dip, float height_dip,
                              Microsoft::WRL::ComPtr<IDWriteTextLayout>& layout,
                              DWRITE_TEXT_METRICS& metrics) {
  if (text.empty() || !format_ || !dwrite_) {
    return false;
  }
  const HRESULT hr = dwrite_->CreateTextLayout(text.c_str(), static_cast<UINT32>(text.size()), format_.Get(),
                                               width_dip, height_dip, layout.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    return false;
  }
  Microsoft::WRL::ComPtr<IDWriteTypography> typography;
  if (SUCCEEDED(dwrite_->CreateTypography(typography.ReleaseAndGetAddressOf()))) {
    const DWRITE_FONT_FEATURE feature{DWRITE_FONT_FEATURE_TAG_TABULAR_FIGURES, 1};
    typography->AddFontFeature(feature);
    const DWRITE_TEXT_RANGE range{0, static_cast<UINT32>(text.size())};
    layout->SetTypography(typography.Get(), range);
  }
  return SUCCEEDED(layout->GetMetrics(&metrics));
}

bool ClockRenderer::Draw(HDC hdc, const RECT& client, bool dark, const std::wstring& left_label,
                         const std::vector<StatusItem>& items, std::vector<StatusHit>* hits, RECT* start_hit,
                         bool start_hot, bool start_pressed, RECT* clock_hit) {
  if (!d2d_ || !dwrite_ || !hdc) {
    return false;
  }
  if (!EnsureTextFormat()) {
    return false;
  }

  if (!rt_) {
    const D2D1_RENDER_TARGET_PROPERTIES props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_DEFAULT,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED),
        static_cast<FLOAT>(dpi_), static_cast<FLOAT>(dpi_));
    const HRESULT hr = d2d_->CreateDCRenderTarget(&props, rt_.ReleaseAndGetAddressOf());
    if (FAILED(hr)) {
      return false;
    }
  }

  HRESULT hr = rt_->BindDC(hdc, &client);
  if (FAILED(hr)) {
    rt_.Reset();
    return false;
  }

  const float width_dip =
      static_cast<float>(client.right - client.left) * 96.0f / static_cast<float>(dpi_);
  const float height_dip =
      static_cast<float>(client.bottom - client.top) * 96.0f / static_cast<float>(dpi_);
  const float px = static_cast<float>(dpi_) / 96.0f;

  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  hr = rt_->CreateSolidColorBrush(ClockTextColor(dark), brush.ReleaseAndGetAddressOf());
  if (FAILED(hr)) {
    return false;
  }

  rt_->BeginDraw();
  rt_->Clear(D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.0f));

  DrawStartButton(brush.Get(), dark, start_hot, start_pressed, height_dip);
  brush->SetColor(ClockTextColor(dark));
  if (start_hit != nullptr) {
    start_hit->left = client.left + static_cast<LONG>(kStartPadLeftDip * px);
    start_hit->top = client.top;
    start_hit->right = client.left + static_cast<LONG>((kStartPadLeftDip + kStartHitWidthDip) * px);
    start_hit->bottom = client.bottom;
  }

  float left_limit = kStartPadLeftDip + kStartHitWidthDip + kItemGapDip;
  if (!left_label.empty()) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (MakeLayout(left_label, width_dip, height_dip, layout, metrics)) {
      brush->SetColor(WarningTextColor(dark));
      const float y = (height_dip - metrics.height) * 0.5f;
      rt_->DrawTextLayout(D2D1::Point2F(left_limit, y), layout.Get(), brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
      brush->SetColor(ClockTextColor(dark));
      left_limit += metrics.widthIncludingTrailingWhitespace + kItemGapDip;
    }
  }

  const std::wstring clock = CurrentTimeText();
  float cursor = width_dip - kPadRightDip;
  if (clock_hit != nullptr) {
    *clock_hit = RECT{};
  }
  if (!clock.empty()) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (MakeLayout(clock, width_dip, height_dip, layout, metrics)) {
      const float x = cursor - metrics.widthIncludingTrailingWhitespace;
      const float y = (height_dip - metrics.height) * 0.5f;
      brush->SetColor(ClockTextColor(dark));
      rt_->DrawTextLayout(D2D1::Point2F(x, y), layout.Get(), brush.Get(),
                          D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
      if (clock_hit != nullptr) {
        clock_hit->left = client.left + static_cast<LONG>(x * px);
        clock_hit->top = client.top;
        clock_hit->right = client.left + static_cast<LONG>((x + metrics.widthIncludingTrailingWhitespace) * px);
        clock_hit->bottom = client.bottom;
      }
      cursor = x - kStatusClockGapDip;
    }
  }

  std::vector<StatusItem> ordered = items;
  std::sort(ordered.begin(), ordered.end(), [](const StatusItem& a, const StatusItem& b) {
    if (a.priority != b.priority) {
      return a.priority > b.priority;
    }
    return a.id < b.id;
  });

  struct Fitted {
    const StatusItem* item;
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics;
  };
  std::vector<Fitted> fitted;
  for (const auto& item : ordered) {
    if (item.text.empty()) {
      continue;
    }
    Fitted next{};
    if (!MakeLayout(item.text, width_dip, height_dip, next.layout, next.metrics)) {
      continue;
    }
    const float left = cursor - next.metrics.widthIncludingTrailingWhitespace;
    if (left < left_limit) {
      continue;
    }
    next.item = &item;
    cursor = left - kItemGapDip;
    fitted.push_back(std::move(next));
  }

  if (hits != nullptr) {
    hits->clear();
  }

  cursor = width_dip - kPadRightDip;
  if (!clock.empty()) {
    Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
    DWRITE_TEXT_METRICS metrics{};
    if (MakeLayout(clock, width_dip, height_dip, layout, metrics)) {
      cursor -= metrics.widthIncludingTrailingWhitespace + kStatusClockGapDip;
    }
  }

  for (auto& entry : fitted) {
    const float width = entry.metrics.widthIncludingTrailingWhitespace;
    const float x = cursor - width;
    const float y = (height_dip - entry.metrics.height) * 0.5f;
    rt_->DrawTextLayout(D2D1::Point2F(x, y), entry.layout.Get(), brush.Get(),
                        D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT);
    if (hits != nullptr) {
      StatusHit hit;
      hit.id = entry.item->id;
      hit.tooltip = entry.item->tooltip;
      hit.rect.left = client.left + static_cast<LONG>(x * px);
      hit.rect.top = client.top;
      hit.rect.right = client.left + static_cast<LONG>((x + width) * px);
      hit.rect.bottom = client.bottom;
      hits->push_back(std::move(hit));
    }
    cursor = x - kItemGapDip;
  }

  hr = rt_->EndDraw();
  if (hr == D2DERR_RECREATE_TARGET) {
    rt_.Reset();
    return false;
  }
  return SUCCEEDED(hr);
}

}  // namespace bamti
