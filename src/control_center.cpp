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

constexpr int kPanelPadDip = 12;
constexpr int kCcWidthDip = 320;
constexpr int kTileHDip = 56;
constexpr int kTileGapDip = 8;
constexpr int kTileNameHDip = 18;
constexpr int kTileRowGapDip = 12;
constexpr int kSepPadDip = 12;
constexpr int kSliderRowHDip = 36;
constexpr int kSliderIconDip = 16;
constexpr int kFooterHDip = 32;
constexpr int kTileRadiusDip = 8;
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
  target->DrawTextLayout(D2D1::Point2F(box.left, box.top), layout.Get(), brush,
                         D2D1_DRAW_TEXT_OPTIONS_ENABLE_COLOR_FONT | D2D1_DRAW_TEXT_OPTIONS_CLIP);
}

void DrawLabel(ID2D1RenderTarget* target, UINT dpi, const std::wstring& text, const D2D1_RECT_F& box,
               ID2D1Brush* brush) {
  DrawPopupText(target, dpi, text, box, brush);
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
  bt_on_ = false;
  bt_known_ = false;
  if (!logged_bt) {
    logged_bt = true;
    Log(L"cc", L"bluetooth radio=%d connectable=%d", bt.radio ? 1 : 0, bt.connectable ? 1 : 0);
    Log(L"cc", L"bluetooth on/off not distinguished this session; drawing off");
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
  int h = kPanelPadDip * 2;
  h += (kTileHDip + kTileNameHDip) * 2 + kTileRowGapDip;
  h += kSepPadDip * 2 + 1;
  h += kSliderRowHDip;
  if (brightness_ok_) {
    h += kSliderRowHDip;
  }
  h += kSepPadDip * 2 + 1;
  h += kFooterHDip;
  return h;
}

RECT ControlCenterContent::TileRect(UINT dpi, int col, int row) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int gap = DipToPx(kTileGapDip, dpi);
  const int inner = DipToPx(kCcWidthDip, dpi) - pad * 2;
  const int tile_w = (inner - gap * 2) / 3;
  const int tile_h = DipToPx(kTileHDip, dpi);
  const int name_h = DipToPx(kTileNameHDip, dpi);
  const int row_gap = DipToPx(kTileRowGapDip, dpi);
  const int x = pad + col * (tile_w + gap);
  const int y = pad + row * (tile_h + name_h + row_gap);
  return RECT{x, y, x + tile_w, y + tile_h};
}

RECT ControlCenterContent::SliderRect(UINT dpi, bool brightness) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int width = DipToPx(kCcWidthDip, dpi);
  const int tiles = DipToPx((kTileHDip + kTileNameHDip) * 2 + kTileRowGapDip, dpi);
  const int sep = DipToPx(kSepPadDip * 2 + 1, dpi);
  int y = pad + tiles + sep;
  if (brightness) {
    y += DipToPx(kSliderRowHDip, dpi);
  }
  return RECT{pad, y, width - pad, y + DipToPx(kSliderRowHDip, dpi)};
}

RECT ControlCenterContent::FooterRect(UINT dpi) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int width = DipToPx(kCcWidthDip, dpi);
  const int h = DipToPx(kFooterHDip, dpi);
  const int y = DipToPx(HeightDip(), dpi) - pad - h;
  return RECT{pad, y, width - pad, y + h};
}

RECT ControlCenterContent::SettingsRect(UINT dpi) const {
  const RECT foot = FooterRect(dpi);
  const int side = DipToPx(28, dpi);
  return RECT{foot.right - side, foot.top, foot.right, foot.bottom};
}

SIZE ControlCenterContent::Measure(UINT dpi) {
  hits_.clear();
  auto add = [&](int id, RECT rc) {
    Hit hit;
    hit.id = id;
    hit.rc = rc;
    hits_.push_back(hit);
  };
  add(kWifi, TileRect(dpi, 0, 0));
  add(kBluetooth, TileRect(dpi, 1, 0));
  add(kAirplane, TileRect(dpi, 2, 0));
  add(kSaver, TileRect(dpi, 0, 1));
  add(kNight, TileRect(dpi, 1, 1));
  add(kAccess, TileRect(dpi, 2, 1));
  add(kVolume, SliderRect(dpi, false));
  if (brightness_ok_) {
    add(kBrightness, SliderRect(dpi, true));
  }
  add(kSettings, SettingsRect(dpi));
  return SIZE{DipToPx(kCcWidthDip, dpi), DipToPx(HeightDip(), dpi)};
}

void ControlCenterContent::Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) {
  if (target == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite;
  DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                      reinterpret_cast<IUnknown**>(dwrite.GetAddressOf()));
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> small_ui;
  if (dwrite) {
    dwrite->CreateTextFormat(kFluentFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 16.0f, L"en-us", fluent.GetAddressOf());
    dwrite->CreateTextFormat(kUiFont, nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL,
                             DWRITE_FONT_STRETCH_NORMAL, 11.0f, L"ko-kr", small_ui.GetAddressOf());
    if (fluent) {
      fluent->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      fluent->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    if (small_ui) {
      small_ui->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
      small_ui->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
  }
  Microsoft::WRL::ComPtr<ID2D1SolidColorBrush> brush;
  target->CreateSolidColorBrush(ClockTextColor(dark), brush.GetAddressOf());
  if (!brush) {
    return;
  }

  struct Tile {
    int id;
    const wchar_t* glyph;
    std::wstring name;
    bool on;
  };
  const Tile tiles[] = {
      {kWifi, kWifiGlyph, wifi_name_, wifi_on_},
      {kBluetooth, kBtGlyph, L"Bluetooth", bt_on_},
      {kAirplane, kPlaneGlyph, L"비행기 모드", false},
      {kSaver, kSaverGlyph, L"절전 모드", saver_on_},
      {kNight, kNightGlyph, L"야간 모드", false},
      {kAccess, kAccessGlyph, L"접근성", false},
  };
  const int radius = DipToPx(kTileRadiusDip, dpi);
  const int name_h = DipToPx(kTileNameHDip, dpi);
  for (int i = 0; i < 6; ++i) {
    const RECT rc = TileRect(dpi, i % 3, i / 3);
    const bool hot = hot_index >= 0 && i < static_cast<int>(hits_.size()) && hits_[static_cast<size_t>(i)].id == tiles[i].id &&
                     hot_index == i;
    (void)hot;
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               static_cast<float>(radius), static_cast<float>(radius)};
    if (tiles[i].on) {
      brush->SetColor(DockIndicatorColor(dark));
    } else {
      brush->SetColor(MenuItemHoverFill(dark, false));
    }
    target->FillRoundedRectangle(rr, brush.Get());
    const D2D1_COLOR_F fg = tiles[i].on ? D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f) : ClockTextColor(dark);
    brush->SetColor(fg);
    if (dwrite && fluent) {
      DrawGlyph(target, dwrite.Get(), fluent.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top), static_cast<float>(rc.right),
                            static_cast<float>(rc.bottom - DipToPx(14, dpi))),
                tiles[i].glyph);
      DrawGlyph(target, dwrite.Get(), fluent.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(rc.right - DipToPx(18, dpi)), static_cast<float>(rc.top + DipToPx(6, dpi)),
                            static_cast<float>(rc.right - DipToPx(4, dpi)), static_cast<float>(rc.top + DipToPx(22, dpi))),
                kChevronGlyph);
    }
    brush->SetColor(ClockTextColor(dark));
    const D2D1_RECT_F name{static_cast<float>(rc.left), static_cast<float>(rc.bottom), static_cast<float>(rc.right),
                           static_cast<float>(rc.bottom + name_h)};
    DrawLabel(target, dpi, tiles[i].name, name, brush.Get());
  }

  auto draw_slider = [&](const RECT& rc, float value, const wchar_t* glyph) {
    const int icon = DipToPx(kSliderIconDip, dpi);
    const float icon_y = static_cast<float>(rc.top + (rc.bottom - rc.top - icon) / 2);
    brush->SetColor(ClockTextColor(dark));
    if (dwrite && fluent) {
      DrawGlyph(target, dwrite.Get(), fluent.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(rc.left), icon_y, static_cast<float>(rc.left + icon), icon_y + icon), glyph);
    }
    const float left = static_cast<float>(rc.left + icon + DipToPx(10, dpi));
    const float right = static_cast<float>(rc.right);
    const float cy = static_cast<float>(rc.top + rc.bottom) * 0.5f;
    const SliderGeometry geom = SliderGeom(left, right, dpi);
    const float v = ClampUnit(value);
    const float x = geom.lo + (geom.hi - geom.lo) * v;
    const float track_h = static_cast<float>(DipToPx(6, dpi));
    D2D1_COLOR_F track = ClockTextColor(dark);
    track.a *= 0.2f;
    brush->SetColor(track);
    target->FillRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(geom.lo, cy - track_h * 0.5f, geom.hi, cy + track_h * 0.5f), track_h, track_h},
        brush.Get());
    brush->SetColor(DockIndicatorColor(dark));
    target->FillRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(geom.lo, cy - track_h * 0.5f, x, cy + track_h * 0.5f), track_h, track_h}, brush.Get());
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(x, cy), geom.thumb_r, geom.thumb_r), brush.Get());
  };
  draw_slider(SliderRect(dpi, false), muted_ ? 0.0f : volume_, kVolGlyph);
  if (brightness_ok_) {
    draw_slider(SliderRect(dpi, true), brightness_, kBrightGlyph);
  }

  const RECT foot = FooterRect(dpi);
  if (battery_present_) {
    const float icon = static_cast<float>(DipToPx(16, dpi));
    const float iy = static_cast<float>(foot.top + (foot.bottom - foot.top)) * 0.5f - icon * 0.5f;
    DrawBatteryIcon(target, brush.Get(), D2D1::RectF(static_cast<float>(foot.left), iy, static_cast<float>(foot.left) + icon,
                                                     iy + icon),
                    dark, battery_, charging_);
    wchar_t pct[16]{};
    swprintf_s(pct, L"%d%%", static_cast<int>(battery_ * 100.0f + 0.5f));
    brush->SetColor(ClockTextColor(dark));
    DrawLabel(target, dpi, pct,
              D2D1::RectF(static_cast<float>(foot.left + DipToPx(22, dpi)), static_cast<float>(foot.top),
                          static_cast<float>(foot.left + DipToPx(80, dpi)), static_cast<float>(foot.bottom)),
              brush.Get());
  }
  const RECT gear = SettingsRect(dpi);
  brush->SetColor(ClockTextColor(dark));
  if (dwrite && fluent) {
    DrawGlyph(target, dwrite.Get(), fluent.Get(), brush.Get(),
              D2D1::RectF(static_cast<float>(gear.left), static_cast<float>(gear.top), static_cast<float>(gear.right),
                          static_cast<float>(gear.bottom)),
              kGearGlyph);
  }
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
  const int icon = DipToPx(kSliderIconDip, dpi);
  const float left = static_cast<float>(hit.rc.left + icon + DipToPx(10, dpi));
  const float right = static_cast<float>(hit.rc.right);
  const SliderGeometry geom = SliderGeom(left, right, dpi);
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
