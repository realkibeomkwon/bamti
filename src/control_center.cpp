#include "control_center.hpp"

#include "clock_renderer.hpp"
#include "log.hpp"
#include "slider_geom.hpp"
#include "theme.hpp"

#include <bluetoothapis.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wlanapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cwchar>

namespace bamti {
namespace {

constexpr int kPanelPadDip = 14;
constexpr int kCcWidthDip = 340;
constexpr int kCardWDip = 312;
constexpr int kRadiusDip = 12;
constexpr int kSectionGapDip = 10;
constexpr int kConnectHDip = 104;
constexpr int kConnectRowHDip = 52;
constexpr int kQuickHDip = 114;
constexpr int kQuickTileWDip = 151;
constexpr int kQuickTileHDip = 52;
constexpr int kQuickGapDip = 10;
constexpr int kSliderCardHDip = 56;
constexpr int kFooterHDip = 28;
constexpr int kSliderTrackHDip = 24;
constexpr ULONGLONG kSlowPeriodMs = 2000;

constexpr wchar_t kFluentFont[] = L"Segoe Fluent Icons";
constexpr wchar_t kUiFont[] = L"Segoe UI";
constexpr wchar_t kWifiGlyph[] = L"\xE701";
constexpr wchar_t kBtGlyph[] = L"\xE702";
constexpr wchar_t kPlaneGlyph[] = L"\xE709";
constexpr wchar_t kSaverGlyph[] = L"\xE8BE";
constexpr wchar_t kNightGlyph[] = L"\xE708";
constexpr wchar_t kAccessGlyph[] = L"\xE776";
constexpr wchar_t kVolGlyph[] = L"\xE767";
constexpr wchar_t kBrightGlyph[] = L"\xE706";
constexpr wchar_t kGearGlyph[] = L"\xE713";
constexpr wchar_t kChevronGlyph[] = L"\xE76C";

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

double QpcMs(const LARGE_INTEGER& start, const LARGE_INTEGER& end) {
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  if (freq.QuadPart == 0) {
    return 0.0;
  }
  return (end.QuadPart - start.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

void OpenSettingsPage(const wchar_t* uri) {
  ShellExecuteW(nullptr, L"open", uri, nullptr, nullptr, SW_SHOWNORMAL);
}

}  // namespace

WlanStatus QueryWlanStatus() {
  WlanStatus info;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  HANDLE handle = nullptr;
  DWORD negotiated = 0;
  if (WlanOpenHandle(2, nullptr, &negotiated, &handle) != ERROR_SUCCESS || handle == nullptr) {
    QueryPerformanceCounter(&t1);
    static LARGE_INTEGER freq{};
    if (freq.QuadPart == 0) {
      QueryPerformanceFrequency(&freq);
    }
    if (freq.QuadPart != 0) {
      info.ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
    }
    return info;
  }
  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  if (WlanEnumInterfaces(handle, nullptr, &list) == ERROR_SUCCESS && list != nullptr) {
    for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
      if (list->InterfaceInfo[i].isState != wlan_interface_state_connected) {
        continue;
      }
      DWORD size = 0;
      PWLAN_CONNECTION_ATTRIBUTES attrs = nullptr;
      if (WlanQueryInterface(handle, &list->InterfaceInfo[i].InterfaceGuid, wlan_intf_opcode_current_connection,
                             nullptr, &size, reinterpret_cast<PVOID*>(&attrs), nullptr) != ERROR_SUCCESS ||
          attrs == nullptr) {
        continue;
      }
      const DOT11_SSID& ssid = attrs->wlanAssociationAttributes.dot11Ssid;
      if (ssid.uSSIDLength > 0) {
        const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID),
                                          static_cast<int>(ssid.uSSIDLength), nullptr, 0);
        if (n > 0) {
          info.name.assign(static_cast<size_t>(n), L'\0');
          MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID),
                              static_cast<int>(ssid.uSSIDLength), info.name.data(), n);
        }
      }
      info.connected = true;
      WlanFreeMemory(attrs);
      break;
    }
    WlanFreeMemory(list);
  }
  WlanCloseHandle(handle, nullptr);
  QueryPerformanceCounter(&t1);
  static LARGE_INTEGER freq{};
  if (freq.QuadPart == 0) {
    QueryPerformanceFrequency(&freq);
  }
  if (freq.QuadPart != 0) {
    info.ms = (t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
  }
  if (!info.connected) {
    info.name = L"연결 안 됨";
  }
  return info;
}

namespace {

struct BtInfo {
  bool radio = false;
  bool connectable = false;
};

BtInfo QueryBluetooth() {
  BtInfo info;
  BLUETOOTH_FIND_RADIO_PARAMS params{};
  params.dwSize = sizeof(params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
  if (find == nullptr) {
    return info;
  }
  info.radio = radio != nullptr;
  if (radio != nullptr) {
    info.connectable = BluetoothIsConnectable(radio) != FALSE;
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(find);
  return info;
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

D2D1_COLOR_F ScaleAlpha(D2D1_COLOR_F color, float mul) {
  color.a *= mul;
  return color;
}

bool MakeFormat(IDWriteFactory* dwrite, const wchar_t* family, DWRITE_FONT_WEIGHT weight, float px,
                DWRITE_TEXT_ALIGNMENT align, Microsoft::WRL::ComPtr<IDWriteTextFormat>& out) {
  out.Reset();
  if (dwrite == nullptr) {
    return false;
  }
  if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                      L"ko-KR", out.ReleaseAndGetAddressOf()))) {
    if (FAILED(dwrite->CreateTextFormat(family, nullptr, weight, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, px,
                                        L"en-US", out.ReleaseAndGetAddressOf()))) {
      return false;
    }
  }
  out->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
  out->SetTextAlignment(align);
  out->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
  return true;
}

}  // namespace

void ControlCenterContent::Reset(ControlCenterHost host) {
  host_ = std::move(host);
  drag_id_ = -1;
  slow_due_ = 0;
  QuerySlowState(true);
  ApplyLive();
}

void ControlCenterContent::Refresh() {
  QuerySlowState(false);
  ApplyLive();
}

void ControlCenterContent::QuerySlowState(bool force) {
  const ULONGLONG now = GetTickCount64();
  if (!force && now < slow_due_) {
    return;
  }
  slow_due_ = now + kSlowPeriodMs;

  static bool logged_bt = false;
  const BtInfo bt = QueryBluetooth();
  bt_on_ = bt.radio;
  bt_known_ = true;
  if (!logged_bt) {
    logged_bt = true;
    Log(L"cc", L"bluetooth radio=%d connectable=%d on=%d", bt.radio ? 1 : 0, bt.connectable ? 1 : 0,
        bt_on_ ? 1 : 0);
  }

  SYSTEM_POWER_STATUS power{};
  if (GetSystemPowerStatus(&power) != FALSE) {
    saver_on_ = (power.SystemStatusFlag & 1) != 0;
    battery_present_ = power.BatteryFlag != 128 && power.BatteryLifePercent != 255;
    charging_ = (power.BatteryFlag & 8) != 0;
    battery_ = battery_present_ ? static_cast<float>(power.BatteryLifePercent) / 100.0f : 0.0f;
  }
}

void ControlCenterContent::ApplyLive() {
  if (!host_.live) {
    return;
  }
  const ControlCenterLive live = host_.live();
  if (drag_id_ != kVolume) {
    volume_ok_ = live.volume_ok;
    volume_ = live.volume;
    muted_ = live.muted;
  }
  if (drag_id_ != kBrightness) {
    brightness_ok_ = live.brightness_ok;
    brightness_ = live.brightness;
  }
  wifi_on_ = live.wifi_on;
  wifi_name_ = live.wifi_name.empty() ? std::wstring(L"연결 안 됨") : live.wifi_name;
}

int ControlCenterContent::HeightDip() const {
  int h = kPanelPadDip + kConnectHDip + kSectionGapDip + kQuickHDip + kSectionGapDip;
  if (brightness_ok_) {
    h += kSliderCardHDip + kSectionGapDip;
  }
  h += kSliderCardHDip + kSectionGapDip + kFooterHDip + kPanelPadDip;
  return h;
}

void ControlCenterContent::EnsureFormats(UINT dpi) {
  if (!dwrite_) {
    DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                        reinterpret_cast<IUnknown**>(dwrite_.ReleaseAndGetAddressOf()));
  }
  if (dwrite_ && format_dpi_ == dpi && fluent17_ && fluent15_ && fluent14_ && semibold13_ && regular12_ &&
      regular11_) {
    return;
  }
  fluent17_.Reset();
  fluent15_.Reset();
  fluent14_.Reset();
  semibold13_.Reset();
  regular12_.Reset();
  regular11_.Reset();
  format_dpi_ = dpi;
  if (!dwrite_) {
    return;
  }
  const float s = static_cast<float>(dpi) / 96.0f;
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 17.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent17_);
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 15.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent15_);
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 14.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent14_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, semibold13_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 12.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular12_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 11.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular11_);
}

RECT ControlCenterContent::ConnectRowRect(UINT dpi, int row) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int y = pad + DipToPx(kConnectRowHDip, dpi) * row;
  return RECT{pad, y, pad + DipToPx(kCardWDip, dpi), y + DipToPx(kConnectRowHDip, dpi)};
}

RECT ControlCenterContent::QuickTileRect(UINT dpi, int col, int row) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int gap = DipToPx(kQuickGapDip, dpi);
  const int w = DipToPx(kQuickTileWDip, dpi);
  const int h = DipToPx(kQuickTileHDip, dpi);
  const int y0 = pad + DipToPx(kConnectHDip + kSectionGapDip, dpi);
  const int x = pad + col * (w + gap);
  const int y = y0 + row * (h + gap);
  return RECT{x, y, x + w, y + h};
}

RECT ControlCenterContent::SliderTrackRect(UINT dpi, bool brightness) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int inset = DipToPx(12, dpi);
  int y = pad + DipToPx(kConnectHDip + kSectionGapDip + kQuickHDip + kSectionGapDip, dpi);
  if (!brightness && brightness_ok_) {
    y += DipToPx(kSliderCardHDip + kSectionGapDip, dpi);
  }
  y += DipToPx(26, dpi);
  const int left = pad + inset;
  const int right = pad + DipToPx(kCardWDip, dpi) - inset;
  return RECT{left, y, right, y + DipToPx(kSliderTrackHDip, dpi)};
}

RECT ControlCenterContent::FooterRect(UINT dpi) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int width = DipToPx(kCcWidthDip, dpi);
  const int h = DipToPx(kFooterHDip, dpi);
  const int y = DipToPx(HeightDip() - kPanelPadDip - kFooterHDip, dpi);
  return RECT{pad, y, width - pad, y + h};
}

RECT ControlCenterContent::SettingsRect(UINT dpi) const {
  const RECT foot = FooterRect(dpi);
  const int side = DipToPx(28, dpi);
  return RECT{foot.right - side, foot.top, foot.right, foot.bottom};
}

SIZE ControlCenterContent::Measure(UINT dpi) {
  EnsureFormats(dpi);
  hits_.clear();
  auto add = [&](int id, RECT rc) {
    Hit hit;
    hit.id = id;
    hit.rc = rc;
    hits_.push_back(hit);
  };
  add(kWifi, ConnectRowRect(dpi, 0));
  add(kBluetooth, ConnectRowRect(dpi, 1));
  add(kAirplane, QuickTileRect(dpi, 0, 0));
  add(kSaver, QuickTileRect(dpi, 1, 0));
  add(kNight, QuickTileRect(dpi, 0, 1));
  add(kAccess, QuickTileRect(dpi, 1, 1));
  if (brightness_ok_) {
    const RECT track = SliderTrackRect(dpi, true);
    add(kBrightness, RECT{track.left - DipToPx(12, dpi), track.top - DipToPx(26, dpi), track.right + DipToPx(12, dpi),
                          track.top - DipToPx(26, dpi) + DipToPx(kSliderCardHDip, dpi)});
  }
  {
    const RECT track = SliderTrackRect(dpi, false);
    add(kVolume, RECT{track.left - DipToPx(12, dpi), track.top - DipToPx(26, dpi), track.right + DipToPx(12, dpi),
                      track.top - DipToPx(26, dpi) + DipToPx(kSliderCardHDip, dpi)});
  }
  add(kSettings, SettingsRect(dpi));
  return SIZE{DipToPx(kCcWidthDip, dpi), DipToPx(HeightDip(), dpi)};
}

void ControlCenterContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr) {
    return;
  }
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  EnsureFormats(dpi);
  const bool dark = host_.dark;
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(ClockTextColor(dark), brush.GetAddressOf());
  if (!brush) {
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d", QpcMs(t0, t1), static_cast<int>(hits_.size()));
    return;
  }

  const int hot_id = (hot_index >= 0 && hot_index < static_cast<int>(hits_.size()))
                         ? hits_[static_cast<size_t>(hot_index)].id
                         : -1;
  const float radius = static_cast<float>(DipToPx(kRadiusDip, dpi));
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  auto fill_round = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               radius, radius};
    target->FillRoundedRectangle(rr, brush.Get());
  };
  auto draw_badge = [&](float cx, float cy, float diameter, bool on, IDWriteTextFormat* format, const wchar_t* glyph) {
    const float r = diameter * 0.5f;
    brush->SetColor(on ? AccentFillColor(dark) : BadgeOffFill(dark));
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush.Get());
    brush->SetColor(on ? AccentOnColor(dark) : fg);
    if (dwrite_ && format) {
      DrawGlyph(target, dwrite_.Get(), format, brush.Get(), D2D1::RectF(cx - r, cy - r, cx + r, cy + r), glyph);
    }
  };

  const RECT connect0 = ConnectRowRect(dpi, 0);
  const RECT connect1 = ConnectRowRect(dpi, 1);
  fill_round(RECT{connect0.left, connect0.top, connect1.right, connect1.bottom}, CardFillColor(dark));
  struct ConnectRow {
    int id;
    RECT rc;
    const wchar_t* glyph;
    const wchar_t* title;
    std::wstring sub;
    bool on;
  };
  const wchar_t* bt_sub = !bt_known_ ? L"알 수 없음" : (bt_on_ ? L"켜짐" : L"꺼짐");
  const ConnectRow connects[] = {
      {kWifi, connect0, kWifiGlyph, L"Wi-Fi", wifi_on_ ? wifi_name_ : std::wstring(L"연결 안 됨"), wifi_on_},
      {kBluetooth, connect1, kBtGlyph, L"Bluetooth", std::wstring(bt_sub), bt_on_},
  };
  for (const ConnectRow& row : connects) {
    if (hot_id == row.id) {
      fill_round(row.rc, MenuItemHoverFill(dark, false));
    }
    const float cy = static_cast<float>(row.rc.top + row.rc.bottom) * 0.5f;
    const float cx = static_cast<float>(row.rc.left) + static_cast<float>(DipToPx(12 + 17, dpi));
    draw_badge(cx, cy, static_cast<float>(DipToPx(34, dpi)), row.on, fluent17_.Get(), row.glyph);
    const float text_x = static_cast<float>(row.rc.left + DipToPx(58, dpi));
    const float chevron_l = static_cast<float>(row.rc.right - DipToPx(24, dpi));
    const float text_r = chevron_l - static_cast<float>(DipToPx(8, dpi));
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), semibold13_.Get(), brush.Get(),
                D2D1::RectF(text_x, static_cast<float>(row.rc.top + DipToPx(6, dpi)), text_r, cy), row.title);
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular11_.Get(), brush.Get(),
                D2D1::RectF(text_x, cy, text_r, static_cast<float>(row.rc.bottom - DipToPx(6, dpi))), row.sub);
    brush->SetColor(ScaleAlpha(fg, 0.45f));
    if (dwrite_ && fluent14_) {
      DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush.Get(),
                D2D1::RectF(chevron_l, cy - static_cast<float>(DipToPx(8, dpi)),
                            static_cast<float>(row.rc.right - DipToPx(12, dpi)), cy + static_cast<float>(DipToPx(8, dpi))),
                kChevronGlyph);
    }
  }

  struct Quick {
    int id;
    int col;
    int row;
    const wchar_t* glyph;
    const wchar_t* name;
    bool on;
  };
  const Quick quick[] = {
      {kAirplane, 0, 0, kPlaneGlyph, L"비행기 모드", false},
      {kSaver, 1, 0, kSaverGlyph, L"절전 모드", saver_on_},
      {kNight, 0, 1, kNightGlyph, L"야간 모드", false},
      {kAccess, 1, 1, kAccessGlyph, L"접근성", false},
  };
  for (const Quick& tile : quick) {
    const RECT rc = QuickTileRect(dpi, tile.col, tile.row);
    fill_round(rc, CardFillColor(dark));
    if (hot_id == tile.id) {
      fill_round(rc, MenuItemHoverFill(dark, false));
    }
    const float cy = static_cast<float>(rc.top + rc.bottom) * 0.5f;
    const float cx = static_cast<float>(rc.left) + static_cast<float>(DipToPx(10 + 15, dpi));
    draw_badge(cx, cy, static_cast<float>(DipToPx(30, dpi)), tile.on, fluent15_.Get(), tile.glyph);
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(rc.left + DipToPx(50, dpi)), static_cast<float>(rc.top),
                            static_cast<float>(rc.right - DipToPx(10, dpi)), static_cast<float>(rc.bottom)),
                tile.name);
  }

  auto draw_slider_card = [&](bool brightness, const wchar_t* title, float value, const wchar_t* glyph, int id) {
    const RECT track = SliderTrackRect(dpi, brightness);
    const RECT card{track.left - DipToPx(12, dpi), track.top - DipToPx(26, dpi), track.right + DipToPx(12, dpi),
                    track.top - DipToPx(26, dpi) + DipToPx(kSliderCardHDip, dpi)};
    fill_round(card, CardFillColor(dark));
    if (hot_id == id) {
      fill_round(card, MenuItemHoverFill(dark, false));
    }
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular11_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(card.left + DipToPx(12, dpi)), static_cast<float>(card.top + DipToPx(8, dpi)),
                            static_cast<float>(card.right - DipToPx(12, dpi)),
                            static_cast<float>(card.top + DipToPx(22, dpi))),
                title);
    const float left = static_cast<float>(track.left);
    const float right = static_cast<float>(track.right);
    const float top = static_cast<float>(track.top);
    const float bottom = static_cast<float>(track.bottom);
    const float h = bottom - top;
    const SliderGeometry geom = SliderGeomThick(left, right, h);
    const float v = ClampUnit(value);
    const float x = geom.lo + (geom.hi - geom.lo) * v;
    brush->SetColor(ScaleAlpha(fg, 0.12f));
    target->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(left, top, right, bottom), h * 0.5f, h * 0.5f},
                                 brush.Get());
    brush->SetColor(AccentFillColor(dark));
    target->FillRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(left, top, x + h * 0.5f, bottom), h * 0.5f, h * 0.5f}, brush.Get());
    const float glyph_l = left + static_cast<float>(DipToPx(8, dpi));
    const float glyph_size = static_cast<float>(DipToPx(14, dpi));
    const bool covered = x + h * 0.5f >= glyph_l + glyph_size + 2.0f;
    brush->SetColor(covered ? AccentOnColor(dark) : ScaleAlpha(fg, 0.7f));
    if (dwrite_ && fluent14_) {
      const float gy = top + (h - glyph_size) * 0.5f;
      DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush.Get(),
                D2D1::RectF(glyph_l, gy, glyph_l + glyph_size, gy + glyph_size), glyph);
    }
  };
  if (brightness_ok_) {
    draw_slider_card(true, L"디스플레이", brightness_, kBrightGlyph, kBrightness);
  }
  draw_slider_card(false, L"사운드", muted_ ? 0.0f : volume_, kVolGlyph, kVolume);

  const RECT foot = FooterRect(dpi);
  if (battery_present_) {
    const float icon = static_cast<float>(DipToPx(16, dpi));
    const float iy = static_cast<float>(foot.top + foot.bottom) * 0.5f - icon * 0.5f;
    DrawBatteryIcon(target, brush.Get(),
                    D2D1::RectF(static_cast<float>(foot.left), iy, static_cast<float>(foot.left) + icon, iy + icon), dark,
                    battery_, charging_);
    wchar_t pct[16]{};
    swprintf_s(pct, L"%d%%", static_cast<int>(battery_ * 100.0f + 0.5f));
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(foot.left + DipToPx(22, dpi)), static_cast<float>(foot.top),
                            static_cast<float>(foot.left + DipToPx(80, dpi)), static_cast<float>(foot.bottom)),
                pct);
  }
  const RECT gear = SettingsRect(dpi);
  brush->SetColor(hot_id == kSettings ? fg : ScaleAlpha(fg, 0.8f));
  if (dwrite_ && fluent17_) {
    DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(gear.left), static_cast<float>(gear.top), static_cast<float>(gear.right),
                          static_cast<float>(gear.bottom)),
              kGearGlyph);
  }
  QueryPerformanceCounter(&t1);
  Log(L"cc", L"render %.2fms rows=%d", QpcMs(t0, t1), static_cast<int>(hits_.size()));
}

int ControlCenterContent::HitTest(POINT client, UINT dpi) const {
  (void)dpi;
  for (int i = 0; i < static_cast<int>(hits_.size()); ++i) {
    if (PtInRect(&hits_[static_cast<size_t>(i)].rc, client)) {
      return i;
    }
  }
  return -1;
}

void ControlCenterContent::Invoke(int index) {
  if (index < 0 || index >= static_cast<int>(hits_.size())) {
    return;
  }
  switch (hits_[static_cast<size_t>(index)].id) {
    case kWifi:
      OpenSettingsPage(L"ms-settings:network-wifi");
      break;
    case kBluetooth:
      OpenSettingsPage(L"ms-settings:bluetooth");
      break;
    case kAirplane:
      OpenSettingsPage(L"ms-settings:network-airplanemode");
      break;
    case kSaver:
      OpenSettingsPage(L"ms-settings:batterysaver");
      break;
    case kNight:
      OpenSettingsPage(L"ms-settings:night-light");
      break;
    case kAccess:
      OpenSettingsPage(L"ms-settings:easeofaccess");
      break;
    case kSettings:
      OpenSettingsPage(L"ms-settings:");
      break;
    default:
      break;
  }
}

bool ControlCenterContent::StickyRow(int index) const {
  (void)index;
  return false;
}

void ControlCenterContent::StickyInvoke(int index) {
  Invoke(index);
}

bool ControlCenterContent::DragRow(int index) const {
  if (index < 0 || index >= static_cast<int>(hits_.size())) {
    return false;
  }
  const int id = hits_[static_cast<size_t>(index)].id;
  return id == kVolume || id == kBrightness;
}

void ControlCenterContent::DragTo(int index, POINT client, UINT dpi) {
  if (index < 0 || index >= static_cast<int>(hits_.size()) || !host_.dispatch) {
    return;
  }
  const Hit& hit = hits_[static_cast<size_t>(index)];
  if (hit.id != kVolume && hit.id != kBrightness) {
    return;
  }
  const RECT track = SliderTrackRect(dpi, hit.id == kBrightness);
  const float left = static_cast<float>(track.left);
  const float right = static_cast<float>(track.right);
  const float thickness = static_cast<float>(track.bottom - track.top);
  const SliderGeometry geom = SliderGeomThick(left, right, thickness);
  float v = geom.hi > geom.lo ? (static_cast<float>(client.x) - geom.lo) / (geom.hi - geom.lo) : 0.0f;
  v = ClampUnit(v);
  v = std::round(v / 0.02f) * 0.02f;
  v = ClampUnit(v);
  if (drag_id_ == hit.id && v == drag_value_) {
    return;
  }
  drag_id_ = hit.id;
  drag_value_ = v;
  if (hit.id == kVolume) {
    volume_ = v;
    muted_ = false;
    StatusEvent ev;
    ev.id = "bamti.widget/volume";
    ev.event = "slide";
    ev.row_id = "volume_level";
    ev.value = v;
    host_.dispatch(ev);
  } else {
    brightness_ = v;
    StatusEvent ev;
    ev.id = "bamti.control_center";
    ev.event = "slide";
    ev.row_id = "brightness";
    ev.value = v;
    host_.dispatch(ev);
  }
}

void ControlCenterContent::DragEnd(int index) {
  (void)index;
  drag_id_ = -1;
}

}  // namespace bamti
