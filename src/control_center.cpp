#include "control_center.hpp"

#include "log.hpp"
#include "slider_geom.hpp"
#include "status_item.hpp"
#include "theme.hpp"

#include <bluetoothapis.h>
#include <d2d1helper.h>
#include <dwrite.h>
#include <shellapi.h>
#include <wlanapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cwchar>

namespace bamti {
namespace {

constexpr int kPanelPadDip = 14;
constexpr int kCcWidthDip = 340;
constexpr int kCardWDip = 312;
constexpr int kRadiusDip = kCornerRadiusDip;
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
constexpr wchar_t kBackGlyph[] = L"\xE76B";
constexpr wchar_t kLockGlyph[] = L"\xE72E";
constexpr int kPageHeaderHDip = 48;
constexpr int kPageRowHDip = 52;
constexpr int kPageListMax = 6;
constexpr int kPageFooterHDip = 32;

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
      const WLAN_INTERFACE_STATE state = list->InterfaceInfo[i].isState;
      if (state != wlan_interface_state_not_ready) {
        info.radio = true;
      }
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

std::wstring SsidWide(const DOT11_SSID& ssid) {
  if (ssid.uSSIDLength == 0) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID),
                                    static_cast<int>(ssid.uSSIDLength), nullptr, 0);
  if (n <= 0) {
    return {};
  }
  std::wstring out(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, 0, reinterpret_cast<const char*>(ssid.ucSSID), static_cast<int>(ssid.uSSIDLength),
                      out.data(), n);
  return out;
}

bool FirstWlanIface(HANDLE handle, GUID* guid, bool* radio) {
  if (handle == nullptr || guid == nullptr) {
    return false;
  }
  PWLAN_INTERFACE_INFO_LIST list = nullptr;
  if (WlanEnumInterfaces(handle, nullptr, &list) != ERROR_SUCCESS || list == nullptr) {
    return false;
  }
  bool found = false;
  for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
    const WLAN_INTERFACE_STATE state = list->InterfaceInfo[i].isState;
    if (radio != nullptr && state != wlan_interface_state_not_ready) {
      *radio = true;
    }
    if (!found) {
      *guid = list->InterfaceInfo[i].InterfaceGuid;
      found = true;
    }
  }
  WlanFreeMemory(list);
  return found;
}

HANDLE OpenWlan() {
  HANDLE handle = nullptr;
  DWORD negotiated = 0;
  if (WlanOpenHandle(2, nullptr, &negotiated, &handle) != ERROR_SUCCESS) {
    return nullptr;
  }
  return handle;
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

void DrawGlyphInked(ID2D1RenderTarget* target, IDWriteFactory* dwrite, IDWriteTextFormat* format, ID2D1Brush* brush,
                    const D2D1_RECT_F& box, const wchar_t* glyph) {
  if (target == nullptr || dwrite == nullptr || format == nullptr || brush == nullptr || glyph == nullptr) {
    return;
  }
  const float layout_w = box.right - box.left;
  const float layout_h = box.bottom - box.top;
  Microsoft::WRL::ComPtr<IDWriteTextLayout> layout;
  if (FAILED(dwrite->CreateTextLayout(glyph, 1, format, layout_w, layout_h, layout.GetAddressOf()))) {
    return;
  }
  DWRITE_OVERHANG_METRICS oh{};
  DWRITE_TEXT_METRICS tm{};
  float dx = 0.0f;
  float dy = 0.0f;
  if (SUCCEEDED(layout->GetOverhangMetrics(&oh))) {
    const float ink_l = -oh.left;
    const float ink_t = -oh.top;
    const float ink_r = layout_w + oh.right;
    const float ink_b = layout_h + oh.bottom;
    if (ink_r > ink_l && ink_b > ink_t) {
      dx = layout_w * 0.5f - (ink_l + ink_r) * 0.5f;
      dy = layout_h * 0.5f - (ink_t + ink_b) * 0.5f;
    }
  }
  if (dx == 0.0f && dy == 0.0f && SUCCEEDED(layout->GetMetrics(&tm)) && tm.width > 0.0f && tm.height > 0.0f) {
    dx = (layout_w - tm.width) * 0.5f - tm.left;
    dy = (layout_h - tm.height) * 0.5f - tm.top;
  }
  target->DrawTextLayout(D2D1::Point2F(box.left + dx, box.top + dy), layout.Get(), brush,
                         D2D1_DRAW_TEXT_OPTIONS_NO_SNAP);
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

void ControlCenterContent::Reset(ControlCenterHost host, ControlCenterPage page) {
  host_ = std::move(host);
  drag_id_ = -1;
  page_ = page == ControlCenterPage::kWifi ? Page::kWifi : Page::kHome;
  show_back_ = page != ControlCenterPage::kWifi;
  slow_due_ = 0;
  list_due_ = 0;
  QuerySlowState(true);
  ApplyLive();
  if (page_ != Page::kHome) {
    RefreshPageLists(true);
  }
}

bool ControlCenterContent::ShowsWifi() const {
  return page_ == Page::kWifi;
}

void ControlCenterContent::Refresh() {
  QuerySlowState(false);
  ApplyLive();
  if (page_ != Page::kHome) {
    RefreshPageLists(false);
  }
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
  if (page_ == Page::kHome) {
    wifi_radio_on_ = wifi_on_;
  }
}

int ControlCenterContent::ListCount() const {
  if (page_ == Page::kWifi) {
    return (std::min)(static_cast<int>(wifi_nets_.size()), kPageListMax);
  }
  if (page_ == Page::kBluetooth) {
    return (std::min)(static_cast<int>(bt_devices_.size()), kPageListMax);
  }
  return 0;
}

void ControlCenterContent::RefreshPageLists(bool force) {
  const ULONGLONG now = GetTickCount64();
  if (!force && now < list_due_) {
    return;
  }
  list_due_ = now + kSlowPeriodMs;
  if (page_ == Page::kWifi) {
    wifi_nets_.clear();
    wifi_iface_ok_ = false;
    HANDLE handle = OpenWlan();
    if (handle == nullptr) {
      wifi_radio_on_ = false;
      return;
    }
    bool radio = false;
    wifi_iface_ok_ = FirstWlanIface(handle, &wifi_iface_, &radio);
    wifi_radio_on_ = radio;
    if (wifi_iface_ok_) {
      PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
      if (WlanGetAvailableNetworkList(handle, &wifi_iface_, 0, nullptr, &list) == ERROR_SUCCESS && list != nullptr) {
        for (DWORD i = 0; i < list->dwNumberOfItems; ++i) {
          const WLAN_AVAILABLE_NETWORK& net = list->Network[i];
          WifiNetwork row;
          row.ssid = SsidWide(net.dot11Ssid);
          if (row.ssid.empty()) {
            continue;
          }
          row.connected = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
          row.has_profile = (net.dwFlags & WLAN_AVAILABLE_NETWORK_HAS_PROFILE) != 0;
          row.secure = net.bSecurityEnabled != FALSE;
          bool seen = false;
          for (WifiNetwork& exist : wifi_nets_) {
            if (exist.ssid == row.ssid) {
              exist.connected = exist.connected || row.connected;
              exist.has_profile = exist.has_profile || row.has_profile;
              seen = true;
              break;
            }
          }
          if (!seen) {
            wifi_nets_.push_back(std::move(row));
          }
        }
        WlanFreeMemory(list);
      }
    }
    WlanCloseHandle(handle, nullptr);
    std::stable_partition(wifi_nets_.begin(), wifi_nets_.end(), [](const WifiNetwork& n) { return n.connected; });
    if (wifi_nets_.size() > static_cast<size_t>(kPageListMax)) {
      wifi_nets_.resize(static_cast<size_t>(kPageListMax));
    }
    return;
  }
  if (page_ != Page::kBluetooth) {
    return;
  }
  bt_devices_.clear();
  BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
  radio_params.dwSize = sizeof(radio_params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio);
  if (radio_find == nullptr) {
    bt_on_ = false;
    return;
  }
  bt_on_ = radio != nullptr;
  BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
  search.dwSize = sizeof(search);
  search.fReturnAuthenticated = TRUE;
  search.fReturnRemembered = TRUE;
  search.fReturnUnknown = FALSE;
  search.fReturnConnected = TRUE;
  search.fIssueInquiry = FALSE;
  search.hRadio = radio;
  BLUETOOTH_DEVICE_INFO info{};
  info.dwSize = sizeof(info);
  const HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, &info);
  if (find != nullptr) {
    do {
      BtDevice row;
      row.info = info;
      row.name = info.szName;
      row.connected = info.fConnected != FALSE;
      if (!row.name.empty()) {
        bt_devices_.push_back(std::move(row));
      }
      info = {};
      info.dwSize = sizeof(info);
    } while (BluetoothFindNextDevice(find, &info));
    BluetoothFindDeviceClose(find);
  }
  if (radio != nullptr) {
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(radio_find);
  std::stable_partition(bt_devices_.begin(), bt_devices_.end(), [](const BtDevice& d) { return d.connected; });
  if (bt_devices_.size() > static_cast<size_t>(kPageListMax)) {
    bt_devices_.resize(static_cast<size_t>(kPageListMax));
  }
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
  center11_.Reset();
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
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 11.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, center11_);
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

void ControlCenterContent::BuildHits(UINT dpi) {
  hits_.clear();
  auto add = [&](int id, RECT rc, int extra = -1) {
    Hit hit;
    hit.id = id;
    hit.rc = rc;
    hit.extra = extra;
    hits_.push_back(hit);
  };
  if (page_ != Page::kHome) {
    const int pad = DipToPx(kPanelPadDip, dpi);
    const int width = DipToPx(kCcWidthDip, dpi);
    if (show_back_) {
      add(kPageBack, RECT{pad, pad, pad + DipToPx(32, dpi), pad + DipToPx(kPageHeaderHDip, dpi)});
    }
    add(kPageToggle, RECT{width - pad - DipToPx(44, dpi), pad + DipToPx(12, dpi), width - pad,
                          pad + DipToPx(12 + 24, dpi)});
    const int list_top = pad + DipToPx(kPageHeaderHDip + 8, dpi);
    const int row_h = DipToPx(kPageRowHDip, dpi);
    const int n = ListCount();
    for (int i = 0; i < n; ++i) {
      const RECT row{pad, list_top + i * row_h, width - pad, list_top + (i + 1) * row_h};
      add(kPageList + i, row, i);
      if (page_ == Page::kWifi && i < static_cast<int>(wifi_nets_.size()) && wifi_nets_[static_cast<size_t>(i)].connected) {
        add(kPageAction + i,
            RECT{row.right - DipToPx(88, dpi), row.bottom - DipToPx(28, dpi), row.right - DipToPx(8, dpi),
                 row.bottom - DipToPx(6, dpi)},
            i);
      }
    }
    add(kPageMore, RECT{pad, DipToPx(HeightDip() - kPanelPadDip - kPageFooterHDip, dpi), width - pad,
                        DipToPx(HeightDip() - kPanelPadDip, dpi)});
    return;
  }
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
}

SIZE ControlCenterContent::Measure(UINT dpi) {
  EnsureFormats(dpi);
  BuildHits(dpi);
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

  BuildHits(dpi);
  const int hot_id = (hot_index >= 0 && hot_index < static_cast<int>(hits_.size()))
                         ? hits_[static_cast<size_t>(hot_index)].id
                         : -1;
  if (page_ != Page::kHome) {
    RenderListPage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=%d", QpcMs(t0, t1), static_cast<int>(hits_.size()),
        page_ == Page::kWifi ? 1 : 2);
    return;
  }
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
    const D2D1_RECT_F cap = D2D1::RectF(left, top, left + h, top + h);
    const bool covered = x + h * 0.5f >= left + h - 2.0f;
    brush->SetColor(covered ? AccentOnColor(dark) : ScaleAlpha(fg, 0.7f));
    if (dwrite_ && fluent14_) {
      DrawGlyphInked(target, dwrite_.Get(), fluent14_.Get(), brush.Get(), cap, glyph);
    }
  };
  if (brightness_ok_) {
    draw_slider_card(true, L"디스플레이", brightness_, kBrightGlyph, kBrightness);
  }
  draw_slider_card(false, L"사운드", muted_ ? 0.0f : volume_, kVolGlyph, kVolume);

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

void ControlCenterContent::RenderListPage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                          ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  const float radius = static_cast<float>(DipToPx(kRadiusDip, dpi));
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int width = DipToPx(kCcWidthDip, dpi);
  auto fill_round = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               radius, radius};
    target->FillRoundedRectangle(rr, brush);
  };

  const bool on = page_ == Page::kWifi ? wifi_radio_on_ : bt_on_;
  const wchar_t* title = page_ == Page::kWifi ? L"Wi-Fi" : L"Bluetooth";
  if (show_back_) {
    if (hot_id == kPageBack) {
      fill_round(RECT{pad, pad, pad + DipToPx(32, dpi), pad + DipToPx(kPageHeaderHDip, dpi)},
                 MenuItemHoverFill(dark, false));
    }
    brush->SetColor(fg);
    if (dwrite_ && fluent17_) {
      DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush,
                D2D1::RectF(static_cast<float>(pad), static_cast<float>(pad + DipToPx(8, dpi)),
                            static_cast<float>(pad + DipToPx(32, dpi)),
                            static_cast<float>(pad + DipToPx(40, dpi))),
                kBackGlyph);
    }
  }
  brush->SetColor(fg);
  const float title_l = static_cast<float>(pad + (show_back_ ? DipToPx(36, dpi) : 0));
  DrawTrimmed(target, dwrite_.Get(), semibold13_.Get(), brush,
              D2D1::RectF(title_l, static_cast<float>(pad),
                          static_cast<float>(width - pad - DipToPx(56, dpi)),
                          static_cast<float>(pad + DipToPx(kPageHeaderHDip, dpi))),
              title);

  const RECT toggle{width - pad - DipToPx(44, dpi), pad + DipToPx(12, dpi), width - pad,
                    pad + DipToPx(12 + 24, dpi)};
  const float th = static_cast<float>(toggle.bottom - toggle.top);
  brush->SetColor(on ? AccentFillColor(dark) : BadgeOffFill(dark));
  target->FillRoundedRectangle(
      D2D1_ROUNDED_RECT{D2D1::RectF(static_cast<float>(toggle.left), static_cast<float>(toggle.top),
                                    static_cast<float>(toggle.right), static_cast<float>(toggle.bottom)),
                        th * 0.5f, th * 0.5f},
      brush);
  const float knob = th - 6.0f;
  const float knob_x = on ? static_cast<float>(toggle.right) - 3.0f - knob : static_cast<float>(toggle.left) + 3.0f;
  brush->SetColor(AccentOnColor(dark));
  target->FillEllipse(
      D2D1::Ellipse(D2D1::Point2F(knob_x + knob * 0.5f, static_cast<float>(toggle.top) + th * 0.5f), knob * 0.5f,
                    knob * 0.5f),
      brush);

  const int n = ListCount();
  const int list_top = pad + DipToPx(kPageHeaderHDip + 8, dpi);
  const int row_h = DipToPx(kPageRowHDip, dpi);
  const RECT list_card{pad, list_top, width - pad, list_top + n * row_h};
  if (n > 0) {
    fill_round(list_card, CardFillColor(dark));
  } else {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush,
                D2D1::RectF(static_cast<float>(pad), static_cast<float>(list_top), static_cast<float>(width - pad),
                            static_cast<float>(list_top + row_h)),
                page_ == Page::kWifi ? L"사용 가능한 네트워크가 없습니다" : L"장치가 없습니다");
  }
  for (int i = 0; i < n; ++i) {
    const RECT row{pad, list_top + i * row_h, width - pad, list_top + (i + 1) * row_h};
    if (hot_id == kPageList + i || hot_id == kPageAction + i) {
      fill_round(row, MenuItemHoverFill(dark, false));
    }
    std::wstring name;
    std::wstring sub;
    bool connected = false;
    bool secure = false;
    if (page_ == Page::kWifi) {
      const WifiNetwork& net = wifi_nets_[static_cast<size_t>(i)];
      name = net.ssid;
      connected = net.connected;
      secure = net.secure;
      sub = connected ? (secure ? L"연결됨, 보안" : L"연결됨") : (secure ? L"보안" : L"개방");
    } else {
      const BtDevice& dev = bt_devices_[static_cast<size_t>(i)];
      name = dev.name;
      connected = dev.connected;
      sub = connected ? L"연결됨" : L"연결 안 됨";
    }
    if (connected) {
      brush->SetColor(AccentFillColor(dark));
      target->FillRectangle(D2D1::RectF(static_cast<float>(row.left), static_cast<float>(row.top + DipToPx(8, dpi)),
                                        static_cast<float>(row.left + DipToPx(3, dpi)),
                                        static_cast<float>(row.bottom - DipToPx(8, dpi))),
                            brush);
    }
    const float glyph_box = static_cast<float>(DipToPx(20, dpi));
    const float gy = static_cast<float>(row.top + row.bottom) * 0.5f - glyph_box * 0.5f;
    const float gx = static_cast<float>(row.left + DipToPx(14, dpi));
    brush->SetColor(fg);
    if (dwrite_ && fluent14_) {
      DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush,
                D2D1::RectF(gx, gy, gx + glyph_box, gy + glyph_box),
                page_ == Page::kWifi ? kWifiGlyph : kBtGlyph);
    }
    if (page_ == Page::kWifi && secure && dwrite_ && fluent14_) {
      brush->SetColor(muted);
      DrawGlyph(target, dwrite_.Get(), fluent14_.Get(), brush,
                D2D1::RectF(gx + glyph_box - static_cast<float>(DipToPx(6, dpi)),
                            gy + glyph_box - static_cast<float>(DipToPx(8, dpi)), gx + glyph_box + static_cast<float>(DipToPx(6, dpi)),
                            gy + glyph_box + static_cast<float>(DipToPx(4, dpi))),
                kLockGlyph);
    }
    const float text_l = static_cast<float>(row.left + DipToPx(44, dpi));
    const float text_r = static_cast<float>(row.right - DipToPx(connected && page_ == Page::kWifi ? 96 : 12, dpi));
    const float mid = static_cast<float>(row.top + row.bottom) * 0.5f;
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), semibold13_.Get(), brush,
                D2D1::RectF(text_l, static_cast<float>(row.top + DipToPx(6, dpi)), text_r, mid), name);
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular11_.Get(), brush,
                D2D1::RectF(text_l, mid, text_r, static_cast<float>(row.bottom - DipToPx(6, dpi))), sub);
    if (connected && page_ == Page::kWifi) {
      const RECT action{row.right - DipToPx(88, dpi), row.bottom - DipToPx(28, dpi), row.right - DipToPx(8, dpi),
                        row.bottom - DipToPx(6, dpi)};
      fill_round(action, BadgeOffFill(dark));
      brush->SetColor(fg);
      DrawTrimmed(target, dwrite_.Get(), center11_ ? center11_.Get() : regular11_.Get(), brush,
                  D2D1::RectF(static_cast<float>(action.left), static_cast<float>(action.top),
                              static_cast<float>(action.right), static_cast<float>(action.bottom)),
                  L"연결 끊기");
    }
  }

  const RECT more{pad, DipToPx(HeightDip() - kPanelPadDip - kPageFooterHDip, dpi), width - pad,
                  DipToPx(HeightDip() - kPanelPadDip, dpi)};
  if (hot_id == kPageMore) {
    fill_round(more, MenuItemHoverFill(dark, false));
  }
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush,
              D2D1::RectF(static_cast<float>(more.left + DipToPx(4, dpi)), static_cast<float>(more.top),
                          static_cast<float>(more.right), static_cast<float>(more.bottom)),
              page_ == Page::kWifi ? L"추가 Wi-Fi 설정" : L"추가 Bluetooth 설정");
}

int ControlCenterContent::HitTest(POINT client, UINT dpi) const {
  (void)dpi;
  for (int i = static_cast<int>(hits_.size()) - 1; i >= 0; --i) {
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
  const Hit& hit = hits_[static_cast<size_t>(index)];
  switch (hit.id) {
    case kWifi:
      page_ = Page::kWifi;
      list_due_ = 0;
      RefreshPageLists(true);
      break;
    case kBluetooth:
      page_ = Page::kBluetooth;
      list_due_ = 0;
      RefreshPageLists(true);
      break;
    case kPageBack:
      page_ = Page::kHome;
      break;
    case kPageToggle:
      if (page_ == Page::kWifi) {
        HANDLE handle = OpenWlan();
        if (handle != nullptr && wifi_iface_ok_) {
          WLAN_PHY_RADIO_STATE phy{};
          phy.dot11SoftwareRadioState = wifi_radio_on_ ? dot11_radio_state_off : dot11_radio_state_on;
          WLAN_RADIO_STATE state{};
          state.dwNumberOfPhys = 1;
          state.PhyRadioState[0] = phy;
          const DWORD err = WlanSetInterface(handle, &wifi_iface_, wlan_intf_opcode_radio_state, sizeof(state),
                                             &state, nullptr);
          Log(L"cc", L"wifi radio set %d err=%lu", wifi_radio_on_ ? 0 : 1, static_cast<unsigned long>(err));
          WlanCloseHandle(handle, nullptr);
        }
        list_due_ = 0;
        RefreshPageLists(true);
      } else if (page_ == Page::kBluetooth) {
        BLUETOOTH_FIND_RADIO_PARAMS params{};
        params.dwSize = sizeof(params);
        HANDLE radio = nullptr;
        const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
        if (find != nullptr) {
          const BOOL next = bt_on_ ? FALSE : TRUE;
          if (radio != nullptr) {
            BluetoothEnableDiscovery(radio, next);
            BluetoothEnableIncomingConnections(radio, next);
            CloseHandle(radio);
          }
          BluetoothFindRadioClose(find);
          bt_on_ = next != FALSE;
        }
        list_due_ = 0;
        RefreshPageLists(true);
      }
      break;
    case kPageMore:
      OpenSettingsPage(page_ == Page::kWifi ? L"ms-settings:network-wifi" : L"ms-settings:bluetooth");
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
      if (hit.id >= kPageAction && hit.id < kPageAction + kPageListMax && page_ == Page::kWifi) {
        const int i = hit.extra;
        if (i >= 0 && i < static_cast<int>(wifi_nets_.size())) {
          HANDLE handle = OpenWlan();
          if (handle != nullptr && wifi_iface_ok_) {
            const DWORD err = WlanDisconnect(handle, &wifi_iface_, nullptr);
            Log(L"cc", L"wifi disconnect err=%lu", static_cast<unsigned long>(err));
            WlanCloseHandle(handle, nullptr);
          }
          list_due_ = 0;
          RefreshPageLists(true);
        }
      } else if (hit.id >= kPageList && hit.id < kPageList + kPageListMax) {
        const int i = hit.extra;
        if (page_ == Page::kWifi && i >= 0 && i < static_cast<int>(wifi_nets_.size())) {
          const WifiNetwork& net = wifi_nets_[static_cast<size_t>(i)];
          if (!net.connected) {
            HANDLE handle = OpenWlan();
            if (handle != nullptr && wifi_iface_ok_) {
              DOT11_SSID ssid{};
              const std::string utf8 = WideToUtf8Bytes(net.ssid);
              ssid.uSSIDLength = (std::min)(static_cast<ULONG>(utf8.size()), static_cast<ULONG>(DOT11_SSID_MAX_LENGTH));
              if (ssid.uSSIDLength > 0) {
                memcpy(ssid.ucSSID, utf8.data(), ssid.uSSIDLength);
              }
              WLAN_CONNECTION_PARAMETERS params{};
              params.wlanConnectionMode =
                  net.has_profile ? wlan_connection_mode_profile
                                  : (net.secure ? wlan_connection_mode_discovery_secure
                                                : wlan_connection_mode_discovery_unsecure);
              params.strProfile = net.has_profile ? net.ssid.c_str() : nullptr;
              params.pDot11Ssid = &ssid;
              params.dot11BssType = dot11_BSS_type_infrastructure;
              const DWORD err = WlanConnect(handle, &wifi_iface_, &params, nullptr);
              Log(L"cc", L"wifi connect %s err=%lu", net.ssid.c_str(), static_cast<unsigned long>(err));
              WlanCloseHandle(handle, nullptr);
            }
            list_due_ = 0;
            RefreshPageLists(true);
          }
        } else if (page_ == Page::kBluetooth && i >= 0 && i < static_cast<int>(bt_devices_.size())) {
          BtDevice& dev = bt_devices_[static_cast<size_t>(i)];
          BLUETOOTH_FIND_RADIO_PARAMS params{};
          params.dwSize = sizeof(params);
          HANDLE radio = nullptr;
          const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
          if (find != nullptr) {
            if (radio != nullptr) {
              const DWORD err = BluetoothAuthenticateDeviceEx(nullptr, radio, &dev.info, nullptr,
                                                              MITMProtectionNotRequired);
              Log(L"cc", L"bt auth %s err=%lu", dev.name.c_str(), static_cast<unsigned long>(err));
              CloseHandle(radio);
            }
            BluetoothFindRadioClose(find);
          }
          list_due_ = 0;
          RefreshPageLists(true);
        }
      }
      break;
  }
}

bool ControlCenterContent::StickyRow(int index) const {
  if (index < 0 || index >= static_cast<int>(hits_.size())) {
    return false;
  }
  const int id = hits_[static_cast<size_t>(index)].id;
  if (id == kWifi || id == kBluetooth || id == kPageBack || id == kPageToggle) {
    return true;
  }
  return id >= kPageList && id < kPageAction + kPageListMax;
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
