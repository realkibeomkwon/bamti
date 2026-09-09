#include "control_center.hpp"

#include "audio_devices.hpp"
#include "bt_devices.hpp"
#include "clock_renderer.hpp"
#include "corner.hpp"
#include "log.hpp"
#include "night_light.hpp"
#include "panel_style.hpp"
#include "slider_geom.hpp"
#include "status_item.hpp"
#include "theme.hpp"
#include "wifi_password.hpp"

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
#include <new>
#include <string_view>

namespace bamti {
namespace {

constexpr int kPanelPadDip = 14;
constexpr int kCardWDip = 280;
constexpr int kSectionGapDip = 10;
constexpr int kConnectHDip = 114;
constexpr int kConnectRowHDip = 52;
constexpr int kQuickHDip = 52;
constexpr int kQuickTileWDip = 135;
constexpr int kQuickTileHDip = 52;
constexpr int kQuickGapDip = 10;
constexpr int kSliderCardHDip = 60;
constexpr int kFooterHDip = 28;
constexpr ULONGLONG kSlowPeriodMs = 2000;
constexpr ULONGLONG kWifiScanPeriodMs = 15000;
constexpr ULONGLONG kWifiScanWaitMs = 4000;
constexpr ULONGLONG kRadioRefreshMs = 300;
constexpr DWORD kMaxPhyIndex = 64;
constexpr UINT kWlanNotifyMsg = WM_APP + 71;
constexpr wchar_t kWlanNotifyClass[] = L"bamti.WlanNotify";

constexpr wchar_t kFluentFont[] = L"Segoe Fluent Icons";
constexpr wchar_t kUiFont[] = L"Segoe UI";
constexpr wchar_t kBtGlyph[] = L"\xE702";
constexpr wchar_t kSaverGlyph[] = L"\xE8BE";
constexpr wchar_t kNightGlyph[] = L"\xE708";
constexpr wchar_t kBrightGlyph[] = L"\xE706";
constexpr wchar_t kGearGlyph[] = L"\xE713";
constexpr wchar_t kChevronGlyph[] = L"\xE76C";
constexpr wchar_t kBackGlyph[] = L"\xE76B";
constexpr wchar_t kLockGlyph[] = L"\xE72E";
constexpr int kPageHeaderHDip = 48;
constexpr int kPageRowHDip = 52;
constexpr int kBtListMax = 8;
constexpr int kBtScanMax = 8;
constexpr int kAudioListMax = 8;
constexpr int kVolumeSliderRowHDip = 28;
constexpr int kVolumeSliderIconDip = 14;
constexpr int kVolumeSliderGapDip = 8;
constexpr int kVolumeTrackHDip = 6;
constexpr float kVolumeKnobDip = 18.0f;
constexpr wchar_t kVolSmallGlyph[] = L"\xE992";
constexpr wchar_t kVolLoudGlyph[] = L"\xE995";
constexpr int kWifiKnownMax = 6;
constexpr int kWifiOtherMax = 6;
constexpr int kWifiListTotalMax = 10;
constexpr int kPageFooterHDip = 32;
constexpr int kNetEthGapDip = 8;

int DipToPx(int dip, UINT dpi) {
  return MulDiv(dip, static_cast<int>(dpi), 96);
}

float DipToPxF(float dip, UINT dpi) {
  return dip * static_cast<float>(dpi) / 96.0f;
}

Microsoft::WRL::ComPtr<ID2D1StrokeStyle> MakeRoundStroke(ID2D1Factory* factory) {
  Microsoft::WRL::ComPtr<ID2D1StrokeStyle> stroke;
  if (factory == nullptr) {
    return stroke;
  }
  D2D1_STROKE_STYLE_PROPERTIES props{};
  props.startCap = D2D1_CAP_STYLE_ROUND;
  props.endCap = D2D1_CAP_STYLE_ROUND;
  props.dashCap = D2D1_CAP_STYLE_ROUND;
  props.lineJoin = D2D1_LINE_JOIN_ROUND;
  props.miterLimit = 1.0f;
  props.dashStyle = D2D1_DASH_STYLE_SOLID;
  factory->CreateStrokeStyle(props, nullptr, 0, stroke.GetAddressOf());
  return stroke;
}

D2D1_RECT_F WifiIconBox(float cx, float cy, float square) {
  const float icon_w = square;
  const float icon_h = square * (kWifiIconHeightDip / kWifiIconDip);
  return D2D1::RectF(cx - icon_w * 0.5f, cy - icon_h * 0.5f, cx + icon_w * 0.5f, cy + icon_h * 0.5f);
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
      info.quality = static_cast<int>(attrs->wlanAssociationAttributes.wlanSignalQuality);
      if (info.quality < 0) {
        info.quality = 0;
      } else if (info.quality > 100) {
        info.quality = 100;
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

constexpr int kGaugeHDip = 10;

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
                 const D2D1_RECT_F& box, const std::wstring& text,
                 DWRITE_TEXT_ALIGNMENT align = DWRITE_TEXT_ALIGNMENT_LEADING) {
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
  layout->SetTextAlignment(align);
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

struct NetworkPageMetrics {
  int height = 0;
  int header_y = panel::kTopPadDip;
  bool show_toggle = false;
  bool show_eth = false;
  int eth_y = 0;
  int div1_y = 0;
  int known_header_y = -1;
  int known_list_y = -1;
  int known_n = 0;
  int other_header_y = -1;
  int other_list_y = -1;
  int other_n = 0;
  int empty_y = -1;
  const wchar_t* empty_text = nullptr;
  int div2_y = 0;
  int net_settings_y = 0;
  int wifi_settings_y = 0;
};

NetworkPageMetrics MakeNetworkPage(bool eth_on, bool iface_ok, bool radio_on, int known_n, int other_n) {
  NetworkPageMetrics m;
  m.show_toggle = iface_ok;
  m.show_eth = eth_on;
  m.known_n = known_n > 0 ? known_n : 0;
  m.other_n = other_n > 0 ? other_n : 0;
  panel::Stack s;
  m.header_y = s.Take(panel::kHeaderHDip);
  s.Gap(panel::kHeaderGapDip);
  if (m.show_eth) {
    m.eth_y = s.Take(panel::kNoteHDip);
    s.Gap(kNetEthGapDip);
  }
  m.div1_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  if (!iface_ok) {
    m.empty_y = s.Take(panel::kRowHDip);
    m.empty_text = L"무선 어댑터가 없습니다";
  } else if (!radio_on) {
    m.empty_y = s.Take(panel::kRowHDip);
    m.empty_text = L"Wi-Fi가 꺼져 있습니다";
  } else if (m.known_n + m.other_n == 0) {
    m.empty_y = s.Take(panel::kRowHDip);
    m.empty_text = L"사용 가능한 네트워크가 없습니다";
  } else {
    if (m.known_n > 0) {
      m.known_header_y = s.Take(panel::kSectionHDip);
      m.known_list_y = s.Take(panel::kRowHDip * m.known_n);
    }
    if (m.other_n > 0) {
      m.other_header_y = s.Take(panel::kSectionHDip);
      m.other_list_y = s.Take(panel::kRowHDip * m.other_n);
    }
  }
  s.Gap(panel::kDivGapDip);
  m.div2_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  m.net_settings_y = s.Take(panel::kSettingsHDip);
  m.wifi_settings_y = s.Take(panel::kSettingsHDip);
  m.height = s.Finish();
  return m;
}

struct VolumePageMetrics {
  int height = 0;
  int header_y = panel::kTopPadDip;
  int slider_y = 0;
  int div1_y = 0;
  int section_y = -1;
  int list_y = -1;
  int list_n = 0;
  int empty_y = -1;
  const wchar_t* empty_text = nullptr;
  int div2_y = 0;
  int device_settings_y = -1;
  int sound_settings_y = 0;
};

VolumePageMetrics MakeVolumePage(int device_n, bool show_device_settings) {
  VolumePageMetrics m;
  m.list_n = device_n > 0 ? (std::min)(device_n, kAudioListMax) : 0;
  panel::Stack s;
  m.header_y = s.Take(panel::kHeaderHDip);
  s.Gap(panel::kHeaderGapDip);
  m.slider_y = s.Take(kVolumeSliderRowHDip);
  s.Gap(panel::kDivGapDip);
  m.div1_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  if (m.list_n == 0) {
    m.empty_y = s.Take(panel::kRowHDip);
    m.empty_text = L"출력 장치가 없습니다";
  } else {
    m.section_y = s.Take(panel::kSectionHDip);
    m.list_y = s.Take(panel::kRowHDip * m.list_n);
  }
  s.Gap(panel::kDivGapDip);
  m.div2_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  if (show_device_settings) {
    m.device_settings_y = s.Take(panel::kSettingsHDip);
  }
  m.sound_settings_y = s.Take(panel::kSettingsHDip);
  m.height = s.Finish();
  return m;
}

void VolumeSliderTrackDip(int* left, int* right) {
  const int content_l = panel::kInsetDip;
  const int content_r = panel::kWidthDip - panel::kInsetDip;
  if (left != nullptr) {
    *left = content_l + kVolumeSliderIconDip + kVolumeSliderGapDip;
  }
  if (right != nullptr) {
    *right = content_r - kVolumeSliderIconDip - kVolumeSliderGapDip;
  }
}

bool DefaultAudioIsBluetooth(const std::vector<AudioEndpoint>& devices) {
  for (const AudioEndpoint& d : devices) {
    if (d.is_default && d.bluetooth) {
      return true;
    }
  }
  return false;
}

struct BluetoothPageMetrics {
  int height = 0;
  int header_y = panel::kTopPadDip;
  int div1_y = 0;
  int mine_header_y = -1;
  int list_y = -1;
  int list_n = 0;
  int empty_y = -1;
  const wchar_t* empty_text = nullptr;
  int other_header_y = -1;
  int found_y = -1;
  int found_n = 0;
  int scanning_y = -1;
  int div2_y = 0;
  int scan_y = -1;
  const wchar_t* scan_text = nullptr;
  int settings_y = 0;
};

BluetoothPageMetrics MakeBluetoothPage(bool present, bool on, int device_n, int found_n, bool scanning) {
  BluetoothPageMetrics m;
  m.list_n = device_n > 0 ? (std::min)(device_n, kBtListMax) : 0;
  m.found_n = found_n > 0 ? (std::min)(found_n, kBtScanMax) : 0;
  const bool show_other = present && on && (scanning || m.found_n > 0);
  panel::Stack s;
  m.header_y = s.Take(panel::kHeaderHDip);
  s.Gap(panel::kHeaderGapDip);
  m.div1_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  if (!present) {
    m.empty_y = s.Take(panel::kRowHDip);
    m.empty_text = L"Bluetooth 어댑터가 없습니다";
  } else if (!on) {
    m.empty_y = s.Take(panel::kRowHDip);
    m.empty_text = L"Bluetooth가 꺼져 있습니다";
  } else {
    if (m.list_n > 0) {
      if (show_other) {
        m.mine_header_y = s.Take(panel::kSectionHDip);
      }
      m.list_y = s.Take(panel::kRowHDip * m.list_n);
    } else if (!show_other) {
      m.empty_y = s.Take(panel::kRowHDip);
      m.empty_text = L"연결된 장치가 없습니다";
    }
    if (show_other) {
      m.other_header_y = s.Take(panel::kSectionHDip);
      if (m.found_n > 0) {
        m.found_y = s.Take(panel::kRowHDip * m.found_n);
      } else {
        m.scanning_y = s.Take(panel::kRowHDip);
      }
    }
  }
  s.Gap(panel::kDivGapDip);
  m.div2_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  if (present && on) {
    m.scan_y = s.Take(panel::kSettingsHDip);
    if (scanning) {
      m.scan_text = L"검색 중지";
    } else if (m.found_n > 0) {
      m.scan_text = L"다시 검색\u2026";
    } else {
      m.scan_text = L"장치 추가\u2026";
    }
  }
  m.settings_y = s.Take(panel::kSettingsHDip);
  m.height = s.Finish();
  return m;
}

struct BatteryPageMetrics {
  int height = 0;
  int header_y = panel::kTopPadDip;
  int gauge_y = 0;
  int remain_y = -1;
  int div1_y = 0;
  int saver_y = 0;
  int power_y = 0;
  int div2_y = 0;
  int settings_y = 0;
};

BatteryPageMetrics MakeBatteryPage(bool show_remain) {
  BatteryPageMetrics m;
  panel::Stack s;
  m.header_y = s.Take(panel::kHeaderHDip);
  s.Gap(panel::kHeaderGapDip);
  m.gauge_y = s.Take(kGaugeHDip);
  if (show_remain) {
    m.remain_y = s.Take(panel::kNoteHDip);
  }
  s.Gap(panel::kDivGapDip);
  m.div1_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  m.saver_y = s.Take(panel::kRowHDip);
  m.power_y = s.Take(panel::kRowHDip);
  s.Gap(panel::kDivGapDip);
  m.div2_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  m.settings_y = s.Take(panel::kSettingsHDip);
  m.height = s.Finish();
  return m;
}

struct CpuPageMetrics {
  int height = 0;
  int header_y = panel::kTopPadDip;
  int gauge_y = 0;
  int div1_y = 0;
  int user_y = 0;
  int kernel_y = 0;
  int nproc_y = 0;
  int div2_y = 0;
  int settings_y = 0;
};

CpuPageMetrics MakeCpuPage() {
  CpuPageMetrics m;
  panel::Stack s;
  m.header_y = s.Take(panel::kHeaderHDip);
  s.Gap(panel::kHeaderGapDip);
  m.gauge_y = s.Take(kGaugeHDip);
  s.Gap(panel::kDivGapDip);
  m.div1_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  m.user_y = s.Take(panel::kRowHDip);
  m.kernel_y = s.Take(panel::kRowHDip);
  m.nproc_y = s.Take(panel::kRowHDip);
  s.Gap(panel::kDivGapDip);
  m.div2_y = s.Take(panel::kDivHDip);
  s.Gap(panel::kDivGapDip);
  m.settings_y = s.Take(panel::kSettingsHDip);
  m.height = s.Finish();
  return m;
}

void DrawGaugeBar(ID2D1RenderTarget* target, ID2D1SolidColorBrush* brush, int y_dip, UINT dpi, bool dark, float level,
                  uint32_t fill_rgb) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  if (level < 0.0f) {
    level = 0.0f;
  }
  if (level > 1.0f) {
    level = 1.0f;
  }
  const float x = static_cast<float>(DipToPx(panel::kInsetDip, dpi));
  const float y = static_cast<float>(DipToPx(y_dip, dpi));
  const float w = static_cast<float>(DipToPx(panel::kWidthDip - panel::kInsetDip * 2, dpi));
  const float h = static_cast<float>(DipToPx(kGaugeHDip, dpi));
  const float r = corner::PillPx(h);
  brush->SetColor(BadgeOffFill(dark));
  target->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, y, x + w, y + h), r, r}, brush);
  if (level > 0.0f) {
    brush->SetColor(D2D1::ColorF(fill_rgb));
    target->FillRoundedRectangle(D2D1_ROUNDED_RECT{D2D1::RectF(x, y, x + w * level, y + h), r, r}, brush);
  }
}

void DrawKvRow(ID2D1RenderTarget* target, IDWriteFactory* dwrite, IDWriteTextFormat* fmt, ID2D1SolidColorBrush* brush,
               int y_dip, UINT dpi, bool dark, const wchar_t* label, const std::wstring& value) {
  if (target == nullptr || brush == nullptr || label == nullptr) {
    return;
  }
  const int inset = DipToPx(panel::kInsetDip, dpi);
  const int width = DipToPx(panel::kWidthDip, dpi);
  const int top = DipToPx(y_dip, dpi);
  const int bottom = top + DipToPx(panel::kRowHDip, dpi);
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite, fmt, brush,
              D2D1::RectF(static_cast<float>(inset), static_cast<float>(top),
                          static_cast<float>(width - inset - DipToPx(56, dpi)), static_cast<float>(bottom)),
              label);
  brush->SetColor(muted);
  DrawTrimmed(target, dwrite, fmt, brush,
              D2D1::RectF(static_cast<float>(inset + DipToPx(80, dpi)), static_cast<float>(top),
                          static_cast<float>(width - inset), static_cast<float>(bottom)),
              value, DWRITE_TEXT_ALIGNMENT_TRAILING);
}

const AudioEndpoint* DefaultAudio(const std::vector<AudioEndpoint>& devices) {
  for (const AudioEndpoint& d : devices) {
    if (d.is_default) {
      return &d;
    }
  }
  return nullptr;
}

void WipeWide(std::wstring* text) {
  if (text == nullptr || text->empty()) {
    return;
  }
  SecureZeroMemory(text->data(), text->size() * sizeof(wchar_t));
  text->clear();
}

std::wstring XmlEscape(std::wstring_view in) {
  std::wstring out;
  out.reserve(in.size());
  for (const wchar_t ch : in) {
    switch (ch) {
      case L'&':
        out += L"&amp;";
        break;
      case L'<':
        out += L"&lt;";
        break;
      case L'>':
        out += L"&gt;";
        break;
      case L'"':
        out += L"&quot;";
        break;
      case L'\'':
        out += L"&apos;";
        break;
      default:
        out.push_back(ch);
        break;
    }
  }
  return out;
}

std::wstring Utf8ToHex(const std::string& utf8) {
  static const wchar_t kDigits[] = L"0123456789ABCDEF";
  std::wstring out;
  out.resize(utf8.size() * 2);
  for (size_t i = 0; i < utf8.size(); ++i) {
    const unsigned char b = static_cast<unsigned char>(utf8[i]);
    out[i * 2] = kDigits[b >> 4];
    out[i * 2 + 1] = kDigits[b & 0x0F];
  }
  return out;
}

std::wstring ReasonText(DWORD reason) {
  wchar_t buf[256]{};
  if (WlanReasonCodeToString(reason, 256, buf, nullptr) == ERROR_SUCCESS && buf[0] != 0) {
    return buf;
  }
  wchar_t fallback[64]{};
  swprintf_s(fallback, L"reason %lu", static_cast<unsigned long>(reason));
  return fallback;
}

bool MapWifiSecurity(DWORD auth, DWORD cipher, const wchar_t** auth_xml, const wchar_t** enc_xml, bool* wep) {
  if (auth_xml == nullptr || enc_xml == nullptr || wep == nullptr) {
    return false;
  }
  *wep = false;
  switch (auth) {
    case DOT11_AUTH_ALGO_RSNA_PSK:
      *auth_xml = L"WPA2PSK";
      break;
    case DOT11_AUTH_ALGO_WPA_PSK:
      *auth_xml = L"WPAPSK";
      break;
    case DOT11_AUTH_ALGO_WPA3_SAE:
      *auth_xml = L"WPA3SAE";
      break;
    case DOT11_AUTH_ALGO_80211_SHARED_KEY:
      *auth_xml = L"open";
      *wep = true;
      *enc_xml = L"WEP";
      return true;
    default:
      return false;
  }
  switch (cipher) {
    case DOT11_CIPHER_ALGO_CCMP:
      *enc_xml = L"AES";
      return true;
    case DOT11_CIPHER_ALGO_TKIP:
      *enc_xml = L"TKIP";
      return true;
    case DOT11_CIPHER_ALGO_WEP:
    case DOT11_CIPHER_ALGO_WEP40:
    case DOT11_CIPHER_ALGO_WEP104:
      *enc_xml = L"WEP";
      return true;
    default:
      return false;
  }
}

WifiKind ClassifyWifiAuth(DWORD auth) {
  switch (auth) {
    case DOT11_AUTH_ALGO_80211_OPEN:
    case DOT11_AUTH_ALGO_OWE:
      return WifiKind::kOpen;
    case DOT11_AUTH_ALGO_WPA_PSK:
    case DOT11_AUTH_ALGO_RSNA_PSK:
    case DOT11_AUTH_ALGO_WPA3_SAE:
    case DOT11_AUTH_ALGO_80211_SHARED_KEY:
      return WifiKind::kPersonal;
    case DOT11_AUTH_ALGO_WPA:
    case DOT11_AUTH_ALGO_RSNA:
    case DOT11_AUTH_ALGO_WPA3:
    case DOT11_AUTH_ALGO_WPA3_ENT:
      return WifiKind::kEnterprise;
    default:
      return WifiKind::kUnknown;
  }
}

const wchar_t* WifiKindName(WifiKind kind) {
  switch (kind) {
    case WifiKind::kOpen:
      return L"open";
    case WifiKind::kPersonal:
      return L"personal";
    case WifiKind::kEnterprise:
      return L"enterprise";
    default:
      return L"unknown";
  }
}

std::wstring BuildWifiProfileXml(const std::wstring& ssid, const std::wstring& password, const wchar_t* auth_xml,
                                 const wchar_t* enc_xml, bool wep) {
  const std::string utf8 = WideToUtf8Bytes(ssid);
  const std::wstring hex = Utf8ToHex(utf8);
  const std::wstring name = XmlEscape(ssid);
  std::wstring key = XmlEscape(password);
  std::wstring xml;
  xml += L"<?xml version=\"1.0\"?>";
  xml += L"<WLANProfile xmlns=\"http://www.microsoft.com/networking/WLAN/profile/v1\">";
  xml += L"<name>";
  xml += name;
  xml += L"</name><SSIDConfig><SSID><hex>";
  xml += hex;
  xml += L"</hex><name>";
  xml += name;
  xml += L"</name></SSID></SSIDConfig>";
  xml += L"<connectionType>ESS</connectionType><connectionMode>manual</connectionMode>";
  xml += L"<MSM><security><authEncryption><authentication>";
  xml += auth_xml != nullptr ? auth_xml : L"WPA2PSK";
  xml += L"</authentication><encryption>";
  xml += enc_xml != nullptr ? enc_xml : L"AES";
  xml += L"</encryption><useOneX>false</useOneX></authEncryption>";
  xml += L"<sharedKey><keyType>";
  xml += wep ? L"networkKey" : L"passPhrase";
  xml += L"</keyType><protected>false</protected><keyMaterial>";
  xml += key;
  xml += L"</keyMaterial></sharedKey></security></MSM></WLANProfile>";
  WipeWide(&key);
  return xml;
}

struct WlanNotifyEvent {
  int kind = 0;
  DWORD reason = 0;
  wchar_t ssid[33]{};
};

ControlCenterContent* NotifySelf(HWND hwnd) {
  if (hwnd == nullptr) {
    return nullptr;
  }
  return reinterpret_cast<ControlCenterContent*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
}

void CALLBACK OnWlanNotify(PWLAN_NOTIFICATION_DATA data, PVOID ctx) {
  const HWND hwnd = static_cast<HWND>(ctx);
  if (hwnd == nullptr || !IsWindow(hwnd) || data == nullptr) {
    return;
  }
  auto* ev = new (std::nothrow) WlanNotifyEvent();
  if (ev == nullptr) {
    return;
  }
  if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_MSM) {
    if (data->NotificationCode == wlan_notification_msm_radio_state_change) {
      ev->kind = 1;
    } else {
      delete ev;
      return;
    }
  } else if (data->NotificationSource == WLAN_NOTIFICATION_SOURCE_ACM) {
    if (data->NotificationCode == wlan_notification_acm_connection_complete) {
      ev->kind = 2;
      if (data->pData != nullptr && data->dwDataSize >= sizeof(WLAN_CONNECTION_NOTIFICATION_DATA)) {
        const auto* conn = static_cast<const WLAN_CONNECTION_NOTIFICATION_DATA*>(data->pData);
        ev->reason = conn->wlanReasonCode;
        const std::wstring ssid = SsidWide(conn->dot11Ssid);
        wcsncpy_s(ev->ssid, ssid.c_str(), _TRUNCATE);
      }
    } else {
      delete ev;
      return;
    }
  } else {
    delete ev;
    return;
  }
  if (PostMessageW(hwnd, kWlanNotifyMsg, 0, reinterpret_cast<LPARAM>(ev)) == FALSE) {
    delete ev;
  }
}

LRESULT CALLBACK NotifyWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
  if (msg == WM_NCCREATE) {
    auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
    return DefWindowProcW(hwnd, msg, wp, lp);
  }
  if (msg == kWlanNotifyMsg) {
    auto* ev = reinterpret_cast<WlanNotifyEvent*>(lp);
    ControlCenterContent* self = NotifySelf(hwnd);
    if (self != nullptr && ev != nullptr) {
      self->HandleWlanNotify(ev->kind, ev->reason, ev->ssid);
    }
    delete ev;
    return 0;
  }
  return DefWindowProcW(hwnd, msg, wp, lp);
}

}  // namespace

ControlCenterContent::ControlCenterContent() = default;

ControlCenterContent::~ControlCenterContent() {
  Dismissed();
  if (notify_hwnd_ != nullptr && IsWindow(notify_hwnd_)) {
    DestroyWindow(notify_hwnd_);
  }
  notify_hwnd_ = nullptr;
}

void ControlCenterContent::PresentHost() {
  if (host_.present) {
    host_.present();
  }
}

void ControlCenterContent::SetAlliedHost(HWND hwnd) {
  if (host_.set_allied) {
    host_.set_allied(hwnd);
  }
}

void ControlCenterContent::EnsureNotifyWindow() {
  if (notify_hwnd_ != nullptr && IsWindow(notify_hwnd_)) {
    return;
  }
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.lpfnWndProc = NotifyWndProc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = kWlanNotifyClass;
  if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
    return;
  }
  notify_hwnd_ = CreateWindowExW(0, kWlanNotifyClass, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this);
}

void ControlCenterContent::EnsureWlanNotify() {
  EnsureNotifyWindow();
  if (wlan_handle_ != nullptr) {
    return;
  }
  wlan_handle_ = OpenWlan();
  if (wlan_handle_ == nullptr || notify_hwnd_ == nullptr) {
    return;
  }
  WlanRegisterNotification(wlan_handle_, WLAN_NOTIFICATION_SOURCE_ACM | WLAN_NOTIFICATION_SOURCE_MSM, TRUE,
                           OnWlanNotify, notify_hwnd_, nullptr, nullptr);
}

void ControlCenterContent::StopWlanNotify() {
  if (wlan_handle_ == nullptr) {
    return;
  }
  WlanRegisterNotification(wlan_handle_, WLAN_NOTIFICATION_SOURCE_NONE, TRUE, nullptr, nullptr, nullptr, nullptr);
  WlanCloseHandle(wlan_handle_, nullptr);
  wlan_handle_ = nullptr;
}

HANDLE ControlCenterContent::WlanHandle() {
  if (wlan_handle_ != nullptr) {
    return wlan_handle_;
  }
  return OpenWlan();
}

void ControlCenterContent::ReleaseWlan(HANDLE handle) {
  if (handle != nullptr && handle != wlan_handle_) {
    WlanCloseHandle(handle, nullptr);
  }
}

void ControlCenterContent::QueryWifiRadio(HANDLE handle) {
  if (handle == nullptr || !wifi_iface_ok_) {
    return;
  }
  DWORD size = 0;
  PWLAN_RADIO_STATE cur = nullptr;
  if (WlanQueryInterface(handle, &wifi_iface_, wlan_intf_opcode_radio_state, nullptr, &size,
                         reinterpret_cast<PVOID*>(&cur), nullptr) != ERROR_SUCCESS ||
      cur == nullptr) {
    return;
  }
  bool hw_on = false;
  bool sw_on = false;
  const DWORD n = cur->dwNumberOfPhys;
  for (DWORD i = 0; i < n && i < kMaxPhyIndex; ++i) {
    if (cur->PhyRadioState[i].dot11HardwareRadioState != dot11_radio_state_off) {
      hw_on = true;
    }
    if (cur->PhyRadioState[i].dot11SoftwareRadioState != dot11_radio_state_off) {
      sw_on = true;
    }
  }
  wifi_hw_radio_on_ = n == 0 ? true : hw_on;
  wifi_radio_on_ = wifi_hw_radio_on_ && sw_on;
  static bool logged_hw = false;
  if (!logged_hw) {
    logged_hw = true;
    const DWORD hw0 = n > 0 ? static_cast<DWORD>(cur->PhyRadioState[0].dot11HardwareRadioState) : 0;
    Log(L"cc", L"wifi hardware radio=%d phys=%lu phy0=%lu", wifi_hw_radio_on_ ? 1 : 0, static_cast<unsigned long>(n),
        static_cast<unsigned long>(hw0));
  }
  WlanFreeMemory(cur);
}

void ControlCenterContent::SetWifiRadio(bool on) {
  HANDLE handle = WlanHandle();
  if (handle == nullptr || !wifi_iface_ok_) {
    ReleaseWlan(handle);
    return;
  }
  DWORD size = 0;
  PWLAN_RADIO_STATE cur = nullptr;
  const DWORD qerr = WlanQueryInterface(handle, &wifi_iface_, wlan_intf_opcode_radio_state, nullptr, &size,
                                        reinterpret_cast<PVOID*>(&cur), nullptr);
  if (qerr != ERROR_SUCCESS || cur == nullptr) {
    Log(L"cc", L"wifi radio query err=%lu", static_cast<unsigned long>(qerr));
    ReleaseWlan(handle);
    return;
  }
  const DWORD n = cur->dwNumberOfPhys;
  bool hw_on = false;
  for (DWORD i = 0; i < n && i < kMaxPhyIndex; ++i) {
    if (cur->PhyRadioState[i].dot11HardwareRadioState != dot11_radio_state_off) {
      hw_on = true;
    }
  }
  wifi_hw_radio_on_ = n == 0 ? true : hw_on;
  if (!wifi_hw_radio_on_) {
    Log(L"cc", L"wifi radio hardware off phys=%lu", static_cast<unsigned long>(n));
    WlanFreeMemory(cur);
    ReleaseWlan(handle);
    return;
  }
  const DOT11_RADIO_STATE want = on ? dot11_radio_state_on : dot11_radio_state_off;
  DWORD last_err = ERROR_SUCCESS;
  DWORD last_idx = 0;
  for (DWORD i = 0; i < n && i < kMaxPhyIndex; ++i) {
    WLAN_PHY_RADIO_STATE phy{};
    phy.dwPhyIndex = cur->PhyRadioState[i].dwPhyIndex;
    phy.dot11SoftwareRadioState = want;
    last_idx = phy.dwPhyIndex;
    last_err = WlanSetInterface(handle, &wifi_iface_, wlan_intf_opcode_radio_state, sizeof(phy), &phy, nullptr);
    Log(L"cc", L"wifi radio set %d phys=%lu idx=%lu err=%lu", on ? 1 : 0, static_cast<unsigned long>(n),
        static_cast<unsigned long>(last_idx), static_cast<unsigned long>(last_err));
  }
  WlanFreeMemory(cur);
  if (last_err == ERROR_SUCCESS) {
    wifi_radio_on_ = on;
  }
  list_due_ = GetTickCount64() + kRadioRefreshMs;
  ReleaseWlan(handle);
}

void ControlCenterContent::ConnectWifiProfile(const std::wstring& ssid) {
  HANDLE handle = WlanHandle();
  if (handle == nullptr || !wifi_iface_ok_) {
    ReleaseWlan(handle);
    return;
  }
  connecting_ssid_ = ssid;
  WLAN_CONNECTION_PARAMETERS params{};
  params.wlanConnectionMode = wlan_connection_mode_profile;
  params.strProfile = ssid.c_str();
  params.dot11BssType = dot11_BSS_type_infrastructure;
  const DWORD err = WlanConnect(handle, &wifi_iface_, &params, nullptr);
  Log(L"cc", L"wifi connect %s err=%lu", ssid.c_str(), static_cast<unsigned long>(err));
  ReleaseWlan(handle);
  list_due_ = GetTickCount64() + kRadioRefreshMs;
}

void ControlCenterContent::ConnectWifi(const WifiNetwork& net) {
  if (net.connected) {
    return;
  }
  if (net.has_profile) {
    ConnectWifiProfile(net.ssid);
    return;
  }
  if (net.kind == WifiKind::kOpen) {
    HANDLE handle = WlanHandle();
    if (handle == nullptr || !wifi_iface_ok_) {
      ReleaseWlan(handle);
      return;
    }
    DOT11_SSID ssid{};
    const std::string utf8 = WideToUtf8Bytes(net.ssid);
    ssid.uSSIDLength = (std::min)(static_cast<ULONG>(utf8.size()), static_cast<ULONG>(DOT11_SSID_MAX_LENGTH));
    if (ssid.uSSIDLength > 0) {
      memcpy(ssid.ucSSID, utf8.data(), ssid.uSSIDLength);
    }
    connecting_ssid_ = net.ssid;
    WLAN_CONNECTION_PARAMETERS params{};
    params.wlanConnectionMode = wlan_connection_mode_discovery_unsecure;
    params.pDot11Ssid = &ssid;
    params.dot11BssType = dot11_BSS_type_infrastructure;
    const DWORD err = WlanConnect(handle, &wifi_iface_, &params, nullptr);
    Log(L"cc", L"wifi connect %s err=%lu", net.ssid.c_str(), static_cast<unsigned long>(err));
    ReleaseWlan(handle);
    list_due_ = GetTickCount64() + kRadioRefreshMs;
    return;
  }
}

void ControlCenterContent::FallbackWifi(const WifiNetwork& net, const wchar_t* why) {
  Log(L"cc", L"wifi fallback ssid=%s why=%s auth=%lu cipher=%lu", net.ssid.c_str(), why != nullptr ? why : L"",
      static_cast<unsigned long>(net.auth), static_cast<unsigned long>(net.cipher));
  OpenSettingsPage(L"ms-availablenetworks:");
}

RECT ControlCenterContent::WifiRowScreen(int index) const {
  RECT row{};
  if (host_.popup_hwnd == nullptr || !IsWindow(host_.popup_hwnd)) {
    return row;
  }
  for (const Hit& hit : hits_) {
    if (hit.id >= kPageList && hit.extra == index) {
      row = hit.rc;
      MapWindowPoints(host_.popup_hwnd, nullptr, reinterpret_cast<POINT*>(&row), 2);
      return row;
    }
  }
  return row;
}

void ControlCenterContent::CloseWifiPassword() {
  if (wifi_prompt_ && wifi_prompt_->visible()) {
    wifi_prompt_->Hide();
  }
  SetAlliedHost(nullptr);
  wifi_prompt_ssid_.clear();
}

void ControlCenterContent::OpenWifiPassword(int index, const std::wstring& error) {
  if (index < 0 || index >= static_cast<int>(wifi_nets_.size())) {
    return;
  }
  const RECT row = WifiRowScreen(index);
  if (row.right <= row.left) {
    return;
  }
  if (!wifi_prompt_) {
    wifi_prompt_ = std::make_unique<WifiPasswordPrompt>();
    wifi_prompt_->SetCallbacks([this](std::wstring password) { OnPasswordSubmit(std::move(password)); },
                               [this]() { CloseWifiPassword(); });
  }
  wifi_prompt_ssid_ = wifi_nets_[static_cast<size_t>(index)].ssid;
  UINT dpi = 96;
  if (host_.popup_hwnd != nullptr) {
    dpi = GetDpiForWindow(host_.popup_hwnd);
  }
  if (dpi == 0) {
    dpi = 96;
  }
  if (!wifi_prompt_->Show(host_.popup_hwnd, row, dpi, host_.dark, error)) {
    wifi_prompt_ssid_.clear();
    return;
  }
  SetAlliedHost(wifi_prompt_->hwnd());
  ShowWindow(wifi_prompt_->hwnd(), SW_SHOW);
  SetWindowPos(wifi_prompt_->hwnd(), HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
  wifi_prompt_->FocusEdit();
}

void ControlCenterContent::OnPasswordSubmit(std::wstring password) {
  WifiNetwork net{};
  bool found = false;
  for (const WifiNetwork& row : wifi_nets_) {
    if (row.ssid == wifi_prompt_ssid_) {
      net = row;
      found = true;
      break;
    }
  }
  CloseWifiPassword();
  if (!found) {
    WipeWide(&password);
    return;
  }
  const wchar_t* auth_xml = nullptr;
  const wchar_t* enc_xml = nullptr;
  bool wep = false;
  if (!MapWifiSecurity(net.auth, net.cipher, &auth_xml, &enc_xml, &wep)) {
    WipeWide(&password);
    FallbackWifi(net, L"cipher");
    return;
  }
  HANDLE handle = WlanHandle();
  if (handle == nullptr || !wifi_iface_ok_) {
    WipeWide(&password);
    ReleaseWlan(handle);
    return;
  }
  std::wstring xml = BuildWifiProfileXml(net.ssid, password, auth_xml, enc_xml, wep);
  WipeWide(&password);
  DWORD reason = 0;
  const DWORD set_err = WlanSetProfile(handle, &wifi_iface_, WLAN_PROFILE_USER, xml.c_str(), nullptr, TRUE, nullptr,
                                       &reason);
  WipeWide(&xml);
  if (set_err != ERROR_SUCCESS || reason != WLAN_REASON_CODE_SUCCESS) {
    const std::wstring text = ReasonText(reason != 0 ? reason : set_err);
    Log(L"cc", L"wifi profile %s err=%lu reason=%s code=%lu", net.ssid.c_str(), static_cast<unsigned long>(set_err),
        text.c_str(), static_cast<unsigned long>(reason));
    ReleaseWlan(handle);
    if (net.auth == DOT11_AUTH_ALGO_WPA3_SAE) {
      FallbackWifi(net, L"wpa3sae");
      return;
    }
    int idx = -1;
    for (int i = 0; i < static_cast<int>(wifi_nets_.size()); ++i) {
      if (wifi_nets_[static_cast<size_t>(i)].ssid == net.ssid) {
        idx = i;
        break;
      }
    }
    if (idx >= 0) {
      OpenWifiPassword(idx, text);
    }
    return;
  }
  Log(L"cc", L"wifi profile %s err=0", net.ssid.c_str());
  ReleaseWlan(handle);
  ConnectWifiProfile(net.ssid);
}

void ControlCenterContent::HandleWlanNotify(int kind, DWORD reason, const std::wstring& ssid) {
  if (kind == 1) {
    HANDLE handle = WlanHandle();
    QueryWifiRadio(handle);
    ReleaseWlan(handle);
    list_due_ = 0;
    RefreshPageLists(true);
    PresentHost();
    return;
  }
  if (kind != 2) {
    return;
  }
  const std::wstring text = ReasonText(reason);
  Log(L"cc", L"wifi acm complete ssid=%s reason=%s code=%lu", ssid.c_str(), text.c_str(),
      static_cast<unsigned long>(reason));
  list_due_ = 0;
  RefreshPageLists(true);
  if (reason != WLAN_REASON_CODE_SUCCESS) {
    std::wstring target = ssid.empty() ? connecting_ssid_ : ssid;
    int idx = -1;
    for (int i = 0; i < static_cast<int>(wifi_nets_.size()); ++i) {
      if (wifi_nets_[static_cast<size_t>(i)].ssid == target) {
        idx = i;
        break;
      }
    }
    if (idx >= 0 && wifi_nets_[static_cast<size_t>(idx)].kind == WifiKind::kPersonal) {
      OpenWifiPassword(idx, text.empty() ? std::wstring(L"연결에 실패했습니다") : text);
    }
  } else {
    CloseWifiPassword();
  }
  connecting_ssid_.clear();
  PresentHost();
}

void ControlCenterContent::Dismissed() {
  CloseWifiPassword();
  StopWlanNotify();
  connecting_ssid_.clear();
  bt_found_.clear();
  bt_scan_rev_ = 0;
  bt_scanning_ = false;
  bt_connecting_addr_.clear();
  bt_fail_armed_ = false;
  if (host_.dispatch) {
    StatusEvent ev;
    ev.id = "bamti.widget/bluetooth";
    ev.event = "toggle";
    ev.row_id = "bt_scan";
    ev.on = false;
    host_.dispatch(ev);
  }
}

void ControlCenterContent::Reset(ControlCenterHost host, ControlCenterPage page) {
  CloseWifiPassword();
  StopWlanNotify();
  host_ = std::move(host);
  drag_id_ = -1;
  switch (page) {
    case ControlCenterPage::kWifi:
      page_ = Page::kWifi;
      show_back_ = false;
      break;
    case ControlCenterPage::kVolume:
      page_ = Page::kVolume;
      show_back_ = false;
      break;
    case ControlCenterPage::kBluetooth:
      page_ = Page::kBluetooth;
      show_back_ = false;
      break;
    case ControlCenterPage::kBattery:
      page_ = Page::kBattery;
      show_back_ = false;
      break;
    case ControlCenterPage::kCpu:
      page_ = Page::kCpu;
      show_back_ = false;
      break;
    default:
      page_ = Page::kHome;
      show_back_ = true;
      break;
  }
  slow_due_ = 0;
  list_due_ = 0;
  wifi_scan_due_ = 0;
  wifi_scan_wait_until_ = 0;
  connecting_ssid_.clear();
  bt_fail_armed_ = false;
  QuerySlowState(true);
  ApplyLive();
  if (page_ != Page::kHome) {
    if (page_ == Page::kWifi) {
      EnsureWlanNotify();
    }
    RefreshPageLists(true);
  }
}

ControlCenterPage ControlCenterContent::CurrentPage() const {
  switch (page_) {
    case Page::kWifi:
      return ControlCenterPage::kWifi;
    case Page::kVolume:
      return ControlCenterPage::kVolume;
    case Page::kBluetooth:
      return ControlCenterPage::kBluetooth;
    case Page::kBattery:
      return ControlCenterPage::kBattery;
    case Page::kCpu:
      return ControlCenterPage::kCpu;
    default:
      return ControlCenterPage::kHome;
  }
}

int ControlCenterContent::CornerDip() const {
  return (page_ == Page::kWifi || page_ == Page::kVolume || page_ == Page::kBluetooth || page_ == Page::kBattery ||
          page_ == Page::kCpu)
             ? corner::kHeroDip
             : corner::kOverlayDip;
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
  const NightLightState night = QueryNightLight();
  night_known_ = night.known;
  night_on_ = night.on;
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
  eth_on_ = live.eth_on;
  eth_name_ = live.eth_name;
  bt_present_ = live.bt_present;
  bt_on_ = live.bt_on;
  bt_can_toggle_ = live.bt_can_toggle;
  bt_scanning_ = live.bt_scanning;
  if (live.bt_connecting != bt_connecting_addr_) {
    bt_connecting_addr_ = live.bt_connecting;
    bt_connecting_since_ = GetTickCount64();
  }
  if (live.bt_list_rev != bt_list_rev_) {
    const bool refresh = bt_list_rev_ != 0;
    bt_list_rev_ = live.bt_list_rev;
    if (refresh && page_ == Page::kBluetooth) {
      list_due_ = 0;
      RefreshPageLists(true);
    }
  }
  if (live.bt_connect_fail_rev != bt_connect_fail_rev_) {
    if (bt_fail_armed_) {
      OpenSettingsPage(L"ms-settings:bluetooth");
    }
    bt_connect_fail_rev_ = live.bt_connect_fail_rev;
  }
  bt_fail_armed_ = true;
  if (live.bt_scan_rev != bt_scan_rev_) {
    bt_found_.clear();
    if (host_.bt_scan_result) {
      const std::vector<BtDeviceInfo> found = host_.bt_scan_result();
      for (const BtDeviceInfo& d : found) {
        BtDevice row;
        row.name = d.name;
        row.info = d.raw;
        row.connected = d.connected;
        row.paired = d.paired;
        row.battery = d.battery;
        row.address = d.address;
        bt_found_.push_back(std::move(row));
      }
    }
    bt_scan_rev_ = live.bt_scan_rev;
  }
  bt_known_ = true;
  battery_ok_ = live.battery_ok;
  battery_level_ = live.battery_level;
  battery_ac_ = live.battery_ac;
  battery_charging_ = live.battery_charging;
  battery_remain_text_ = live.battery_remain_text;
  saver_on_ = live.battery_saver_on;
  battery_saver_toggle_ok_ = live.battery_saver_toggle_ok;
  cpu_ok_ = live.cpu_ok;
  cpu_usage_ = live.cpu_usage;
  cpu_user_ = live.cpu_user;
  cpu_kernel_ = live.cpu_kernel;
  cpu_nproc_ = live.cpu_nproc;
  if (page_ == Page::kHome) {
    wifi_radio_on_ = wifi_on_;
  }
}

int ControlCenterContent::ListCount() const {
  if (page_ == Page::kBluetooth) {
    return (std::min)(static_cast<int>(bt_devices_.size()), kBtListMax);
  }
  return 0;
}

void ControlCenterContent::RefreshPageLists(bool force) {
  const ULONGLONG now = GetTickCount64();
  if (!force && now < list_due_) {
    return;
  }
  if (page_ == Page::kVolume) {
    audio_outs_ = EnumAudioOutputs();
    if (audio_outs_.size() > static_cast<size_t>(kAudioListMax)) {
      audio_outs_.resize(static_cast<size_t>(kAudioListMax));
    }
    list_due_ = now + kSlowPeriodMs;
    return;
  }
  if (page_ == Page::kWifi) {
    const auto commit = [this](std::vector<WifiNetwork> nets) {
      std::vector<WifiNetwork> known;
      std::vector<WifiNetwork> other;
      for (const WifiNetwork& n : nets) {
        if (n.has_profile) {
          known.push_back(n);
        } else {
          other.push_back(n);
        }
      }
      std::stable_partition(known.begin(), known.end(), [](const WifiNetwork& n) { return n.connected; });
      std::stable_partition(other.begin(), other.end(), [](const WifiNetwork& n) { return n.connected; });
      if (known.size() > static_cast<size_t>(kWifiKnownMax)) {
        known.resize(static_cast<size_t>(kWifiKnownMax));
      }
      if (other.size() > static_cast<size_t>(kWifiOtherMax)) {
        other.resize(static_cast<size_t>(kWifiOtherMax));
      }
      while (known.size() + other.size() > static_cast<size_t>(kWifiListTotalMax)) {
        if (!other.empty()) {
          other.pop_back();
        } else if (!known.empty()) {
          known.pop_back();
        } else {
          break;
        }
      }
      wifi_known_n_ = static_cast<int>(known.size());
      wifi_nets_.clear();
      wifi_nets_.insert(wifi_nets_.end(), known.begin(), known.end());
      wifi_nets_.insert(wifi_nets_.end(), other.begin(), other.end());
    };

    HANDLE handle = OpenWlan();
    if (handle == nullptr) {
      wifi_iface_ok_ = false;
      wifi_radio_on_ = false;
      wifi_known_n_ = 0;
      wifi_nets_.clear();
      list_due_ = now + kSlowPeriodMs;
      return;
    }
    bool radio = false;
    wifi_iface_ok_ = FirstWlanIface(handle, &wifi_iface_, &radio);
    wifi_radio_on_ = radio;
    QueryWifiRadio(handle);
    if (!wifi_iface_ok_ || !wifi_radio_on_) {
      wifi_known_n_ = 0;
      wifi_nets_.clear();
      CloseWifiPassword();
      WlanCloseHandle(handle, nullptr);
      list_due_ = now + kSlowPeriodMs;
      return;
    }

    if (force || now >= wifi_scan_due_) {
      const DWORD scan_err = WlanScan(handle, &wifi_iface_, nullptr, nullptr, nullptr);
      static bool logged_scan = false;
      if (!logged_scan) {
        logged_scan = true;
        Log(L"cc", L"wifi scan err=%lu", static_cast<unsigned long>(scan_err));
      }
      if (scan_err == ERROR_SUCCESS || scan_err == ERROR_BUSY) {
        wifi_scan_due_ = now + kWifiScanPeriodMs;
        wifi_scan_wait_until_ = now + kWifiScanWaitMs;
      }
    }

    std::vector<WifiNetwork> fresh;
    PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
    const DWORD list_err = WlanGetAvailableNetworkList(handle, &wifi_iface_, 0, nullptr, &list);
    static bool logged_list = false;
    if (!logged_list && list_err != ERROR_SUCCESS) {
      logged_list = true;
      Log(L"cc", L"wifi list err=%lu", static_cast<unsigned long>(list_err));
    }
    if (list_err == ERROR_SUCCESS && list != nullptr) {
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
        row.auth = static_cast<DWORD>(net.dot11DefaultAuthAlgorithm);
        row.cipher = static_cast<DWORD>(net.dot11DefaultCipherAlgorithm);
        row.kind = ClassifyWifiAuth(row.auth);
        bool seen = false;
        for (WifiNetwork& exist : fresh) {
          if (exist.ssid == row.ssid) {
            exist.connected = exist.connected || row.connected;
            exist.has_profile = exist.has_profile || row.has_profile;
            exist.secure = exist.secure || row.secure;
            if (row.has_profile || row.connected || exist.auth == 0) {
              exist.auth = row.auth;
              exist.cipher = row.cipher;
              exist.kind = row.kind;
            }
            seen = true;
            break;
          }
        }
        if (!seen) {
          fresh.push_back(std::move(row));
        }
      }
      WlanFreeMemory(list);
    }
    WlanCloseHandle(handle, nullptr);

    const bool scan_pending = now < wifi_scan_wait_until_;
    if (scan_pending && !wifi_nets_.empty() && fresh.size() < wifi_nets_.size()) {
      std::vector<WifiNetwork> merged = wifi_nets_;
      for (WifiNetwork& exist : merged) {
        exist.connected = false;
      }
      for (const WifiNetwork& row : fresh) {
        bool seen = false;
        for (WifiNetwork& exist : merged) {
          if (exist.ssid == row.ssid) {
            exist.connected = row.connected;
            exist.has_profile = exist.has_profile || row.has_profile;
            exist.secure = row.secure;
            exist.auth = row.auth;
            exist.cipher = row.cipher;
            exist.kind = row.kind;
            seen = true;
            break;
          }
        }
        if (!seen) {
          merged.push_back(row);
        }
      }
      commit(std::move(merged));
    } else {
      commit(std::move(fresh));
    }
    static std::vector<std::wstring> logged_kinds;
    for (const WifiNetwork& n : wifi_nets_) {
      bool seen = false;
      for (const std::wstring& s : logged_kinds) {
        if (s == n.ssid) {
          seen = true;
          break;
        }
      }
      if (seen) {
        continue;
      }
      logged_kinds.push_back(n.ssid);
      Log(L"cc", L"wifi kind ssid=%s auth=%lu kind=%s", n.ssid.c_str(), static_cast<unsigned long>(n.auth),
          WifiKindName(n.kind));
    }
    static int logged_n = -1;
    if (logged_n != static_cast<int>(wifi_nets_.size())) {
      logged_n = static_cast<int>(wifi_nets_.size());
      Log(L"cc", L"wifi list n=%d known=%d pending=%d", logged_n, wifi_known_n_, scan_pending ? 1 : 0);
    }
    list_due_ = scan_pending ? now : now + kSlowPeriodMs;
    return;
  }
  list_due_ = now + kSlowPeriodMs;
  if (page_ != Page::kBluetooth) {
    return;
  }
  bt_devices_.clear();
  const std::vector<BtDeviceInfo> listed = EnumBtDevices();
  for (const BtDeviceInfo& d : listed) {
    BtDevice row;
    row.name = d.name;
    row.info = d.raw;
    row.connected = d.connected;
    row.paired = d.paired;
    row.battery = d.battery;
    row.address = d.address;
    bt_devices_.push_back(std::move(row));
  }
}

int ControlCenterContent::WidthDip() const {
  return panel::kWidthDip;
}

int ControlCenterContent::HeightDip() const {
  if (page_ == Page::kWifi) {
    const int other_n = (std::max)(0, static_cast<int>(wifi_nets_.size()) - wifi_known_n_);
    return MakeNetworkPage(eth_on_, wifi_iface_ok_, wifi_radio_on_, wifi_known_n_, other_n).height;
  }
  if (page_ == Page::kVolume) {
    return MakeVolumePage(static_cast<int>(audio_outs_.size()), DefaultAudioIsBluetooth(audio_outs_)).height;
  }
  if (page_ == Page::kBluetooth) {
    return MakeBluetoothPage(bt_present_, bt_on_, static_cast<int>(bt_devices_.size()),
                             static_cast<int>(bt_found_.size()), bt_scanning_)
        .height;
  }
  if (page_ == Page::kBattery) {
    return MakeBatteryPage(!battery_ac_ && !battery_remain_text_.empty()).height;
  }
  if (page_ == Page::kCpu) {
    return MakeCpuPage().height;
  }
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
  if (dwrite_ && format_dpi_ == dpi && fluent17_ && fluent14_ && semibold14_ && semibold13_ && regular14_ &&
      regular13_ && regular12_ && regular11_) {
    return;
  }
  fluent17_.Reset();
  fluent14_.Reset();
  semibold14_.Reset();
  semibold13_.Reset();
  regular14_.Reset();
  regular13_.Reset();
  regular12_.Reset();
  regular11_.Reset();
  center11_.Reset();
  format_dpi_ = dpi;
  if (!dwrite_) {
    return;
  }
  const float s = static_cast<float>(dpi) / 96.0f;
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 17.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent17_);
  MakeFormat(dwrite_.Get(), kFluentFont, DWRITE_FONT_WEIGHT_NORMAL, 14.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, fluent14_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, 14.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, semibold14_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_SEMI_BOLD, 13.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, semibold13_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 14.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular14_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 13.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular13_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 12.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular12_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 11.0f * s, DWRITE_TEXT_ALIGNMENT_LEADING, regular11_);
  MakeFormat(dwrite_.Get(), kUiFont, DWRITE_FONT_WEIGHT_NORMAL, 11.0f * s, DWRITE_TEXT_ALIGNMENT_CENTER, center11_);
}

RECT ControlCenterContent::ConnectRowRect(UINT dpi, int row) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int y = pad + row * DipToPx(kConnectRowHDip + kQuickGapDip, dpi);
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

RECT ControlCenterContent::SliderCardRect(UINT dpi, bool brightness) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  int y = pad + DipToPx(kConnectHDip + kSectionGapDip + kQuickHDip + kSectionGapDip, dpi);
  if (!brightness && brightness_ok_) {
    y += DipToPx(kSliderCardHDip + kSectionGapDip, dpi);
  }
  return RECT{pad, y, pad + DipToPx(kCardWDip, dpi), y + DipToPx(kSliderCardHDip, dpi)};
}

RECT ControlCenterContent::SliderTrackRect(UINT dpi, bool brightness) const {
  const RECT card = SliderCardRect(dpi, brightness);
  const int side = DipToPx(12 + kVolumeSliderIconDip + kVolumeSliderGapDip, dpi);
  const int row_top = card.top + DipToPx(28, dpi);
  const int y = row_top + (DipToPx(kVolumeSliderRowHDip, dpi) - DipToPx(kVolumeTrackHDip, dpi)) / 2;
  return RECT{card.left + side, y, card.right - side, y + DipToPx(kVolumeTrackHDip, dpi)};
}

RECT ControlCenterContent::FooterRect(UINT dpi) const {
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int width = DipToPx(panel::kWidthDip, dpi);
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
  if (page_ == Page::kWifi) {
    const int other_n = (std::max)(0, static_cast<int>(wifi_nets_.size()) - wifi_known_n_);
    const NetworkPageMetrics m = MakeNetworkPage(eth_on_, wifi_iface_ok_, wifi_radio_on_, wifi_known_n_, other_n);
    const int width = DipToPx(panel::kWidthDip, dpi);
    const int inset = DipToPx(panel::kInsetDip, dpi);
    if (show_back_) {
      add(kPageBack, RECT{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                          DipToPx(m.header_y + panel::kHeaderHDip, dpi)});
    }
    if (m.show_toggle) {
      add(kPageToggle, RECT{width - inset - DipToPx(panel::kToggleWDip, dpi), DipToPx(m.header_y, dpi), width - inset,
                            DipToPx(m.header_y + panel::kToggleHDip, dpi)});
    }
    auto add_rows = [&](int y0, int n, int extra0) {
      for (int i = 0; i < n; ++i) {
        const int top = DipToPx(y0 + i * panel::kRowHDip, dpi);
        add(kPageList + extra0 + i,
            RECT{inset, top, width - inset, top + DipToPx(panel::kRowHDip, dpi)}, extra0 + i);
      }
    };
    add_rows(m.known_list_y, m.known_n, 0);
    add_rows(m.other_list_y, m.other_n, m.known_n);
    add(kPageNetworkSettings,
        RECT{inset, DipToPx(m.net_settings_y, dpi), width - inset,
             DipToPx(m.net_settings_y + panel::kSettingsHDip, dpi)});
    add(kPageWifiSettings,
        RECT{inset, DipToPx(m.wifi_settings_y, dpi), width - inset,
             DipToPx(m.wifi_settings_y + panel::kSettingsHDip, dpi)});
    return;
  }
  if (page_ == Page::kVolume) {
    const VolumePageMetrics m =
        MakeVolumePage(static_cast<int>(audio_outs_.size()), DefaultAudioIsBluetooth(audio_outs_));
    const int width = DipToPx(panel::kWidthDip, dpi);
    const int inset = DipToPx(panel::kInsetDip, dpi);
    if (show_back_) {
      add(kPageBack, RECT{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                          DipToPx(m.header_y + panel::kHeaderHDip, dpi)});
    }
    add(kVolume, RECT{inset, DipToPx(m.slider_y, dpi), width - inset,
                      DipToPx(m.slider_y + kVolumeSliderRowHDip, dpi)});
    for (int i = 0; i < m.list_n; ++i) {
      const int top = DipToPx(m.list_y + i * panel::kRowHDip, dpi);
      add(kPageList + i, RECT{inset, top, width - inset, top + DipToPx(panel::kRowHDip, dpi)}, i);
    }
    if (m.device_settings_y >= 0) {
      add(kPageDeviceSettings,
          RECT{inset, DipToPx(m.device_settings_y, dpi), width - inset,
               DipToPx(m.device_settings_y + panel::kSettingsHDip, dpi)});
    }
    add(kPageSoundSettings,
        RECT{inset, DipToPx(m.sound_settings_y, dpi), width - inset,
             DipToPx(m.sound_settings_y + panel::kSettingsHDip, dpi)});
    return;
  }
  if (page_ == Page::kBluetooth) {
    const BluetoothPageMetrics m =
        MakeBluetoothPage(bt_present_, bt_on_, static_cast<int>(bt_devices_.size()),
                          static_cast<int>(bt_found_.size()), bt_scanning_);
    const int width = DipToPx(panel::kWidthDip, dpi);
    const int inset = DipToPx(panel::kInsetDip, dpi);
    if (show_back_) {
      add(kPageBack, RECT{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                          DipToPx(m.header_y + panel::kHeaderHDip, dpi)});
    }
    if (bt_present_) {
      add(kPageToggle, RECT{width - inset - DipToPx(panel::kToggleWDip, dpi), DipToPx(m.header_y, dpi),
                            width - inset, DipToPx(m.header_y + panel::kToggleHDip, dpi)});
    }
    for (int i = 0; i < m.list_n; ++i) {
      const int top = DipToPx(m.list_y + i * panel::kRowHDip, dpi);
      add(kPageList + i, RECT{inset, top, width - inset, top + DipToPx(panel::kRowHDip, dpi)}, i);
    }
    for (int i = 0; i < m.found_n; ++i) {
      const int top = DipToPx(m.found_y + i * panel::kRowHDip, dpi);
      add(kPageList + kBtListMax + i, RECT{inset, top, width - inset, top + DipToPx(panel::kRowHDip, dpi)},
          kBtListMax + i);
    }
    if (m.scan_y >= 0) {
      add(kPageScan, RECT{inset, DipToPx(m.scan_y, dpi), width - inset,
                          DipToPx(m.scan_y + panel::kSettingsHDip, dpi)});
    }
    add(kPageBtSettings,
        RECT{inset, DipToPx(m.settings_y, dpi), width - inset,
             DipToPx(m.settings_y + panel::kSettingsHDip, dpi)});
    return;
  }
  if (page_ == Page::kBattery) {
    const BatteryPageMetrics m = MakeBatteryPage(!battery_ac_ && !battery_remain_text_.empty());
    const int width = DipToPx(panel::kWidthDip, dpi);
    const int inset = DipToPx(panel::kInsetDip, dpi);
    if (show_back_) {
      add(kPageBack, RECT{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                          DipToPx(m.header_y + panel::kHeaderHDip, dpi)});
    }
    add(kPageToggle, RECT{width - inset - DipToPx(panel::kToggleWDip, dpi), DipToPx(m.saver_y, dpi), width - inset,
                          DipToPx(m.saver_y + panel::kToggleHDip, dpi)});
    add(kPagePowerSettings, RECT{inset, DipToPx(m.settings_y, dpi), width - inset,
                                 DipToPx(m.settings_y + panel::kSettingsHDip, dpi)});
    return;
  }
  if (page_ == Page::kCpu) {
    const CpuPageMetrics m = MakeCpuPage();
    const int width = DipToPx(panel::kWidthDip, dpi);
    const int inset = DipToPx(panel::kInsetDip, dpi);
    if (show_back_) {
      add(kPageBack, RECT{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                          DipToPx(m.header_y + panel::kHeaderHDip, dpi)});
    }
    add(kPageTaskManager, RECT{inset, DipToPx(m.settings_y, dpi), width - inset,
                               DipToPx(m.settings_y + panel::kSettingsHDip, dpi)});
    return;
  }
  add(kWifi, ConnectRowRect(dpi, 0));
  add(kBluetooth, ConnectRowRect(dpi, 1));
  add(kSaver, QuickTileRect(dpi, 0, 0));
  add(kNight, QuickTileRect(dpi, 1, 0));
  if (brightness_ok_) {
    add(kBrightness, SliderCardRect(dpi, true));
  }
  add(kVolume, SliderCardRect(dpi, false));
  add(kSettings, SettingsRect(dpi));
}

SIZE ControlCenterContent::Measure(UINT dpi) {
  EnsureFormats(dpi);
  BuildHits(dpi);
  return SIZE{DipToPx(WidthDip(), dpi), DipToPx(HeightDip(), dpi)};
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
  if (page_ == Page::kWifi) {
    RenderNetworkPage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=1", QpcMs(t0, t1), static_cast<int>(hits_.size()));
    return;
  }
  if (page_ == Page::kVolume) {
    RenderVolumePage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=3", QpcMs(t0, t1), static_cast<int>(hits_.size()));
    return;
  }
  if (page_ == Page::kBluetooth) {
    RenderBluetoothPage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=2", QpcMs(t0, t1), static_cast<int>(hits_.size()));
    return;
  }
  if (page_ == Page::kBattery) {
    RenderBatteryPage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=4", QpcMs(t0, t1), static_cast<int>(hits_.size()));
    return;
  }
  if (page_ == Page::kCpu) {
    RenderCpuPage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=5", QpcMs(t0, t1), static_cast<int>(hits_.size()));
    return;
  }
  if (page_ != Page::kHome) {
    RenderListPage(target, dpi, hot_id, brush.Get());
    QueryPerformanceCounter(&t1);
    Log(L"cc", L"render %.2fms rows=%d page=%d", QpcMs(t0, t1), static_cast<int>(hits_.size()),
        page_ == Page::kWifi ? 1 : 2);
    return;
  }
  const float radius = corner::ToPx(corner::kOverlayDip, dpi);
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  ID2D1Factory* d2d = nullptr;
  target->GetFactory(&d2d);
  const Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke = MakeRoundStroke(d2d);
  auto fill_round = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               radius, radius};
    target->FillRoundedRectangle(rr, brush.Get());
  };
  auto draw_badge = [&](float cx, float cy, float diameter, bool on, IDWriteTextFormat* format, const wchar_t* glyph,
                        float alpha = 1.0f) {
    const float r = diameter * 0.5f;
    brush->SetColor(ScaleAlpha(on ? AccentFillColor(dark) : BadgeOffFill(dark), alpha));
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(cx, cy), r, r), brush.Get());
    brush->SetColor(ScaleAlpha(on ? AccentOnColor(dark) : fg, alpha));
    if (dwrite_ && format) {
      DrawGlyph(target, dwrite_.Get(), format, brush.Get(), D2D1::RectF(cx - r, cy - r, cx + r, cy + r), glyph);
    }
  };

  const RECT connect0 = ConnectRowRect(dpi, 0);
  const RECT connect1 = ConnectRowRect(dpi, 1);
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
      {kWifi, connect0, nullptr, L"Wi-Fi", wifi_on_ ? wifi_name_ : std::wstring(L"연결 안 됨"), wifi_on_},
      {kBluetooth, connect1, kBtGlyph, L"Bluetooth", std::wstring(bt_sub), bt_on_},
  };
  for (const ConnectRow& row : connects) {
    fill_round(row.rc, CardFillColor(dark));
    if (hot_id == row.id) {
      fill_round(row.rc, MenuItemHoverFill(dark, false));
    }
    const float cy = static_cast<float>(row.rc.top + row.rc.bottom) * 0.5f;
    const float cx = static_cast<float>(row.rc.left) + static_cast<float>(DipToPx(12 + 17, dpi));
    const float diameter = static_cast<float>(DipToPx(34, dpi));
    draw_badge(cx, cy, diameter, row.on, fluent17_.Get(), row.glyph);
    if (row.id == kWifi && d2d != nullptr && round_stroke) {
      const D2D1_COLOR_F icon = row.on ? AccentOnColor(dark) : fg;
      DrawWifiIcon(target, d2d, brush.Get(), round_stroke.Get(), WifiIconBox(cx, cy, diameter * 0.55f), icon, 2);
    }
    const float text_x = static_cast<float>(row.rc.left + DipToPx(55, dpi));
    const float chevron_l = static_cast<float>(row.rc.right - DipToPx(24, dpi));
    const float text_r = chevron_l - static_cast<float>(DipToPx(8, dpi));
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), semibold14_.Get(), brush.Get(),
                D2D1::RectF(text_x, static_cast<float>(row.rc.top + DipToPx(6, dpi)), text_r, cy), row.title);
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush.Get(),
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
    bool enabled;
  };
  const Quick quick[] = {
      {kSaver, 0, 0, kSaverGlyph, L"절전 모드", saver_on_, !battery_ac_},
      {kNight, 1, 0, kNightGlyph, L"야간 모드", night_on_, true},
  };
  for (const Quick& tile : quick) {
    const RECT rc = QuickTileRect(dpi, tile.col, tile.row);
    fill_round(rc, CardFillColor(dark));
    if (tile.enabled && hot_id == tile.id) {
      fill_round(rc, MenuItemHoverFill(dark, false));
    }
    const float alpha = tile.enabled ? 1.0f : 0.40f;
    const float cy = static_cast<float>(rc.top + rc.bottom) * 0.5f;
    const float cx = static_cast<float>(rc.left) + static_cast<float>(DipToPx(10 + panel::kCircleDip / 2, dpi));
    draw_badge(cx, cy, static_cast<float>(DipToPx(panel::kCircleDip, dpi)), tile.on, fluent14_.Get(), tile.glyph, alpha);
    brush->SetColor(ScaleAlpha(fg, alpha));
    DrawTrimmed(target, dwrite_.Get(), regular14_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(rc.left + DipToPx(45, dpi)), static_cast<float>(rc.top),
                            static_cast<float>(rc.right - DipToPx(10, dpi)), static_cast<float>(rc.bottom)),
                tile.name);
  }

  auto draw_slider_card = [&](bool brightness, const wchar_t* title, float value, int id) {
    const RECT card = SliderCardRect(dpi, brightness);
    fill_round(card, CardFillColor(dark));
    if (hot_id == id) {
      fill_round(card, MenuItemHoverFill(dark, false));
    }
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush.Get(),
                D2D1::RectF(static_cast<float>(card.left + DipToPx(12, dpi)),
                            static_cast<float>(card.top + DipToPx(8, dpi)),
                            static_cast<float>(card.right - DipToPx(12, dpi)),
                            static_cast<float>(card.top + DipToPx(8 + panel::kSectionHDip, dpi))),
                title);
    const int row_top = card.top + DipToPx(28, dpi);
    const float row_cy = static_cast<float>(row_top) + DipToPxF(static_cast<float>(kVolumeSliderRowHDip), dpi) * 0.5f;
    const float icon = DipToPxF(static_cast<float>(kVolumeSliderIconDip), dpi);
    const float left_icon_d = brightness ? DipToPxF(11.0f, dpi) : icon;
    const float inset = DipToPxF(12.0f, dpi);
    const D2D1_RECT_F left_icon{static_cast<float>(card.left) + inset, row_cy - left_icon_d * 0.5f,
                                static_cast<float>(card.left) + inset + left_icon_d, row_cy + left_icon_d * 0.5f};
    const D2D1_RECT_F right_icon{static_cast<float>(card.right) - inset - icon, row_cy - icon * 0.5f,
                                 static_cast<float>(card.right) - inset, row_cy + icon * 0.5f};
    brush->SetColor(fg);
    if (dwrite_ && fluent14_) {
      DrawGlyphInked(target, dwrite_.Get(), fluent14_.Get(), brush.Get(), left_icon,
                     brightness ? kBrightGlyph : kVolSmallGlyph);
      DrawGlyphInked(target, dwrite_.Get(), fluent14_.Get(), brush.Get(), right_icon,
                     brightness ? kBrightGlyph : kVolLoudGlyph);
    }
    const RECT track = SliderTrackRect(dpi, brightness);
    const float track_l = static_cast<float>(track.left);
    const float track_r = static_cast<float>(track.right);
    const float track_t = static_cast<float>(track.top);
    const float track_b = static_cast<float>(track.bottom);
    const float track_h = track_b - track_t;
    const float pill = corner::PillPx(track_h);
    const float knob = DipToPxF(kVolumeKnobDip, dpi);
    const float knob_r = knob * 0.5f;
    const float lo = track_l + knob_r;
    const float hi = track_r - knob_r;
    const float v = ClampUnit(value);
    const float knob_x = lo + (hi - lo) * v;
    D2D1_COLOR_F fill = AccentFillColor(dark);
    D2D1_COLOR_F knob_color = AccentOnColor(dark);
    if (!brightness && muted_) {
      fill = ScaleAlpha(fill, 0.40f);
      knob_color = ScaleAlpha(knob_color, 0.40f);
    }
    brush->SetColor(BadgeOffFill(dark));
    target->FillRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(track_l, track_t, track_r, track_b), pill, pill}, brush.Get());
    brush->SetColor(fill);
    target->FillRoundedRectangle(
        D2D1_ROUNDED_RECT{D2D1::RectF(track_l, track_t, knob_x, track_b), pill, pill}, brush.Get());
    brush->SetColor(knob_color);
    target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, row_cy), knob_r, knob_r), brush.Get());
  };
  if (brightness_ok_) {
    draw_slider_card(true, L"디스플레이", brightness_, kBrightness);
  }
  draw_slider_card(false, L"사운드", volume_, kVolume);

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

void ControlCenterContent::RenderNetworkPage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                             ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  ID2D1Factory* d2d = nullptr;
  target->GetFactory(&d2d);
  const Microsoft::WRL::ComPtr<ID2D1StrokeStyle> round_stroke = MakeRoundStroke(d2d);
  const int other_n = (std::max)(0, static_cast<int>(wifi_nets_.size()) - wifi_known_n_);
  const NetworkPageMetrics m = MakeNetworkPage(eth_on_, wifi_iface_ok_, wifi_radio_on_, wifi_known_n_, other_n);
  const int width = DipToPx(panel::kWidthDip, dpi);
  const int inset = DipToPx(panel::kInsetDip, dpi);
  auto text_rect = [&](int y, int h, int left, int right) {
    return D2D1::RectF(static_cast<float>(left), static_cast<float>(DipToPx(y, dpi)), static_cast<float>(right),
                       static_cast<float>(DipToPx(y + h, dpi)));
  };

  if (show_back_) {
    const RECT back{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                    DipToPx(m.header_y + panel::kHeaderHDip, dpi)};
    if (hot_id == kPageBack) {
      panel::FillHover(target, brush, back, dpi, dark);
    }
    brush->SetColor(fg);
    if (dwrite_ && fluent17_) {
      DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush,
                D2D1::RectF(static_cast<float>(back.left), static_cast<float>(back.top),
                            static_cast<float>(back.right), static_cast<float>(back.bottom)),
                kBackGlyph);
    }
  }

  const float title_l = static_cast<float>(inset + (show_back_ ? DipToPx(panel::kBackWDip + 4, dpi) : 0));
  const float title_r = static_cast<float>(width - inset - (m.show_toggle ? DipToPx(panel::kToggleWDip + 8, dpi) : 0));
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), semibold14_ ? semibold14_.Get() : semibold13_.Get(), brush,
              D2D1::RectF(title_l, static_cast<float>(DipToPx(m.header_y, dpi)), title_r,
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              L"Wi-Fi");

  if (m.show_toggle) {
    const RECT toggle{width - inset - DipToPx(panel::kToggleWDip, dpi), DipToPx(m.header_y, dpi), width - inset,
                      DipToPx(m.header_y + panel::kToggleHDip, dpi)};
    panel::DrawToggle(target, brush, toggle, dpi, wifi_radio_on_, wifi_hw_radio_on_, dark);
  }

  if (m.show_eth) {
    std::wstring eth = L"이더넷: ";
    eth += eth_name_.empty() ? std::wstring(L"연결됨") : eth_name_;
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush,
                text_rect(m.eth_y, panel::kNoteHDip, inset, width - inset), eth);
  }

  panel::DrawDivider(target, brush, m.div1_y, dpi, dark);

  if (m.empty_text != nullptr) {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                text_rect(m.empty_y, panel::kRowHDip, inset, width - inset), m.empty_text);
  }

  auto draw_section = [&](int header_y, const wchar_t* title, int list_y, int n, int extra0) {
    if (header_y >= 0) {
      panel::DrawSectionHeader(target, dwrite_.Get(), regular12_.Get(), brush, header_y, dpi, dark, title);
    }
    const float circle = DipToPxF(static_cast<float>(panel::kCircleDip), dpi);
    const float lock_w = DipToPxF(9.0f, dpi);
    const float lock_h = DipToPxF(12.5f, dpi);
    const float lock_gap = DipToPxF(2.0f, dpi);
    for (int i = 0; i < n; ++i) {
      const int idx = extra0 + i;
      if (idx < 0 || idx >= static_cast<int>(wifi_nets_.size())) {
        continue;
      }
      const WifiNetwork& net = wifi_nets_[static_cast<size_t>(idx)];
      const RECT row{inset, DipToPx(list_y + i * panel::kRowHDip, dpi), width - inset,
                     DipToPx(list_y + (i + 1) * panel::kRowHDip, dpi)};
      if (hot_id == kPageList + idx) {
        panel::FillHover(target, brush, row, dpi, dark);
      }
      const float cy = static_cast<float>(row.top + row.bottom) * 0.5f;
      const float cx = static_cast<float>(row.left) + circle * 0.5f;
      panel::DrawRowCircle(target, dwrite_.Get(), fluent14_.Get(), brush, cx, cy, dpi, dark, net.connected,
                           nullptr);
      if (d2d != nullptr && round_stroke) {
        const D2D1_COLOR_F icon = net.connected ? AccentOnColor(dark) : ClockTextColor(dark);
        DrawWifiIcon(target, d2d, brush, round_stroke.Get(), WifiIconBox(cx, cy, circle * 0.55f), icon, 2);
      }
      const float text_l = static_cast<float>(DipToPx(panel::kTextLeftDip, dpi));
      float text_r = static_cast<float>(row.right);
      if (net.secure) {
        const float lock_r = static_cast<float>(row.right) - lock_gap;
        const float lock_l = lock_r - lock_w;
        brush->SetColor(muted);
        if (dwrite_ && fluent14_) {
          DrawGlyphInked(target, dwrite_.Get(), fluent14_.Get(), brush,
                         D2D1::RectF(lock_l, cy - lock_h * 0.5f, lock_r, cy + lock_h * 0.5f), kLockGlyph);
        }
        text_r = lock_l - DipToPxF(8.0f, dpi);
      }
      brush->SetColor(fg);
      DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                  D2D1::RectF(text_l, static_cast<float>(row.top), text_r, static_cast<float>(row.bottom)), net.ssid);
    }
  };
  draw_section(m.known_header_y, L"알려진 네트워크", m.known_list_y, m.known_n, 0);
  draw_section(m.other_header_y, L"다른 네트워크", m.other_list_y, m.other_n, m.known_n);

  panel::DrawDivider(target, brush, m.div2_y, dpi, dark);

  IDWriteTextFormat* settings_fmt = regular13_ ? regular13_.Get() : regular12_.Get();
  panel::DrawSettingsRow(target, dwrite_.Get(), settings_fmt, brush, m.net_settings_y, dpi, dark,
                         hot_id == kPageNetworkSettings, L"네트워크 설정\u2026");
  panel::DrawSettingsRow(target, dwrite_.Get(), settings_fmt, brush, m.wifi_settings_y, dpi, dark,
                         hot_id == kPageWifiSettings, L"Wi-Fi 설정\u2026");
}

RECT ControlCenterContent::VolumeSliderTrackRect(UINT dpi) const {
  const VolumePageMetrics m =
      MakeVolumePage(static_cast<int>(audio_outs_.size()), DefaultAudioIsBluetooth(audio_outs_));
  int track_l = 0;
  int track_r = 0;
  VolumeSliderTrackDip(&track_l, &track_r);
  const int y = DipToPx(m.slider_y, dpi) +
                (DipToPx(kVolumeSliderRowHDip, dpi) - DipToPx(kVolumeTrackHDip, dpi)) / 2;
  return RECT{DipToPx(track_l, dpi), y, DipToPx(track_r, dpi), y + DipToPx(kVolumeTrackHDip, dpi)};
}

void ControlCenterContent::RenderVolumePage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                            ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  const VolumePageMetrics m =
      MakeVolumePage(static_cast<int>(audio_outs_.size()), DefaultAudioIsBluetooth(audio_outs_));
  const int width = DipToPx(panel::kWidthDip, dpi);
  const int inset = DipToPx(panel::kInsetDip, dpi);

  if (show_back_) {
    const RECT back{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                    DipToPx(m.header_y + panel::kHeaderHDip, dpi)};
    if (hot_id == kPageBack) {
      panel::FillHover(target, brush, back, dpi, dark);
    }
    brush->SetColor(fg);
    if (dwrite_ && fluent17_) {
      DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush,
                D2D1::RectF(static_cast<float>(back.left), static_cast<float>(back.top),
                            static_cast<float>(back.right), static_cast<float>(back.bottom)),
                kBackGlyph);
    }
  }

  const float title_l = static_cast<float>(inset + (show_back_ ? DipToPx(panel::kBackWDip + 4, dpi) : 0));
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), semibold14_ ? semibold14_.Get() : semibold13_.Get(), brush,
              D2D1::RectF(title_l, static_cast<float>(DipToPx(m.header_y, dpi)),
                          static_cast<float>(width - inset),
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              L"사운드");

  const RECT slider_row{inset, DipToPx(m.slider_y, dpi), width - inset,
                        DipToPx(m.slider_y + kVolumeSliderRowHDip, dpi)};
  const float icon = DipToPxF(static_cast<float>(kVolumeSliderIconDip), dpi);
  const float row_cy = static_cast<float>(slider_row.top + slider_row.bottom) * 0.5f;
  const D2D1_RECT_F left_icon{static_cast<float>(slider_row.left), row_cy - icon * 0.5f,
                              static_cast<float>(slider_row.left) + icon, row_cy + icon * 0.5f};
  const D2D1_RECT_F right_icon{static_cast<float>(slider_row.right) - icon, row_cy - icon * 0.5f,
                               static_cast<float>(slider_row.right), row_cy + icon * 0.5f};
  brush->SetColor(fg);
  if (dwrite_ && fluent14_) {
    DrawGlyphInked(target, dwrite_.Get(), fluent14_.Get(), brush, left_icon, kVolSmallGlyph);
    DrawGlyphInked(target, dwrite_.Get(), fluent14_.Get(), brush, right_icon, kVolLoudGlyph);
  }

  const RECT track = VolumeSliderTrackRect(dpi);
  const float track_l = static_cast<float>(track.left);
  const float track_r = static_cast<float>(track.right);
  const float track_t = static_cast<float>(track.top);
  const float track_b = static_cast<float>(track.bottom);
  const float track_h = track_b - track_t;
  const float pill = corner::PillPx(track_h);
  const float knob = DipToPxF(kVolumeKnobDip, dpi);
  const float knob_r = knob * 0.5f;
  const float lo = track_l + knob_r;
  const float hi = track_r - knob_r;
  const float v = ClampUnit(volume_);
  const float knob_x = lo + (hi - lo) * v;
  D2D1_COLOR_F fill = AccentFillColor(dark);
  D2D1_COLOR_F knob_color = AccentOnColor(dark);
  if (muted_) {
    fill = ScaleAlpha(fill, 0.40f);
    knob_color = ScaleAlpha(knob_color, 0.40f);
  }
  brush->SetColor(BadgeOffFill(dark));
  target->FillRoundedRectangle(
      D2D1_ROUNDED_RECT{D2D1::RectF(track_l, track_t, track_r, track_b), pill, pill}, brush);
  brush->SetColor(fill);
  target->FillRoundedRectangle(
      D2D1_ROUNDED_RECT{D2D1::RectF(track_l, track_t, knob_x, track_b), pill, pill}, brush);
  brush->SetColor(knob_color);
  target->FillEllipse(D2D1::Ellipse(D2D1::Point2F(knob_x, row_cy), knob_r, knob_r), brush);

  panel::DrawDivider(target, brush, m.div1_y, dpi, dark);

  if (m.empty_text != nullptr) {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                D2D1::RectF(static_cast<float>(inset), static_cast<float>(DipToPx(m.empty_y, dpi)),
                            static_cast<float>(width - inset),
                            static_cast<float>(DipToPx(m.empty_y + panel::kRowHDip, dpi))),
                m.empty_text);
  } else if (m.section_y >= 0) {
    panel::DrawSectionHeader(target, dwrite_.Get(), regular12_.Get(), brush, m.section_y, dpi, dark, L"출력");
    const float circle = DipToPxF(static_cast<float>(panel::kCircleDip), dpi);
    for (int i = 0; i < m.list_n; ++i) {
      if (i < 0 || i >= static_cast<int>(audio_outs_.size())) {
        continue;
      }
      const AudioEndpoint& dev = audio_outs_[static_cast<size_t>(i)];
      const RECT row{inset, DipToPx(m.list_y + i * panel::kRowHDip, dpi), width - inset,
                     DipToPx(m.list_y + (i + 1) * panel::kRowHDip, dpi)};
      if (hot_id == kPageList + i) {
        panel::FillHover(target, brush, row, dpi, dark);
      }
      const float cy = static_cast<float>(row.top + row.bottom) * 0.5f;
      const float cx = static_cast<float>(row.left) + circle * 0.5f;
      panel::DrawRowCircle(target, dwrite_.Get(), fluent14_.Get(), brush, cx, cy, dpi, dark, dev.is_default,
                           AudioFormGlyph(dev.form));
      brush->SetColor(fg);
      DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                  D2D1::RectF(static_cast<float>(DipToPx(panel::kTextLeftDip, dpi)), static_cast<float>(row.top),
                              static_cast<float>(row.right), static_cast<float>(row.bottom)),
                  dev.name);
    }
  }

  panel::DrawDivider(target, brush, m.div2_y, dpi, dark);

  IDWriteTextFormat* settings_fmt = regular13_ ? regular13_.Get() : regular12_.Get();
  if (m.device_settings_y >= 0) {
    const AudioEndpoint* def = DefaultAudio(audio_outs_);
    std::wstring label = def != nullptr ? def->name : std::wstring();
    label += L" 설정\u2026";
    panel::DrawSettingsRow(target, dwrite_.Get(), settings_fmt, brush, m.device_settings_y, dpi, dark,
                           hot_id == kPageDeviceSettings, label.c_str());
  }
  panel::DrawSettingsRow(target, dwrite_.Get(), settings_fmt, brush, m.sound_settings_y, dpi, dark,
                         hot_id == kPageSoundSettings, L"사운드 설정\u2026");
}

void ControlCenterContent::RenderBluetoothPage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                               ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  const BluetoothPageMetrics m =
      MakeBluetoothPage(bt_present_, bt_on_, static_cast<int>(bt_devices_.size()),
                        static_cast<int>(bt_found_.size()), bt_scanning_);
  const int width = DipToPx(panel::kWidthDip, dpi);
  const int inset = DipToPx(panel::kInsetDip, dpi);

  if (show_back_) {
    const RECT back{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                    DipToPx(m.header_y + panel::kHeaderHDip, dpi)};
    if (hot_id == kPageBack) {
      panel::FillHover(target, brush, back, dpi, dark);
    }
    brush->SetColor(fg);
    if (dwrite_ && fluent17_) {
      DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush,
                D2D1::RectF(static_cast<float>(back.left), static_cast<float>(back.top),
                            static_cast<float>(back.right), static_cast<float>(back.bottom)),
                kBackGlyph);
    }
  }

  const float title_l = static_cast<float>(inset + (show_back_ ? DipToPx(panel::kBackWDip + 4, dpi) : 0));
  const float title_r = static_cast<float>(width - inset - (bt_present_ ? DipToPx(panel::kToggleWDip + 8, dpi) : 0));
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), semibold14_ ? semibold14_.Get() : semibold13_.Get(), brush,
              D2D1::RectF(title_l, static_cast<float>(DipToPx(m.header_y, dpi)), title_r,
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              L"Bluetooth");

  if (bt_present_) {
    const RECT toggle{width - inset - DipToPx(panel::kToggleWDip, dpi), DipToPx(m.header_y, dpi), width - inset,
                      DipToPx(m.header_y + panel::kToggleHDip, dpi)};
    panel::DrawToggle(target, brush, toggle, dpi, bt_on_, bt_can_toggle_, dark);
  }

  panel::DrawDivider(target, brush, m.div1_y, dpi, dark);

  if (m.mine_header_y >= 0) {
    panel::DrawSectionHeader(target, dwrite_.Get(), regular12_.Get(), brush, m.mine_header_y, dpi, dark,
                             L"내 장치");
  }

  if (m.empty_text != nullptr) {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                D2D1::RectF(static_cast<float>(inset), static_cast<float>(DipToPx(m.empty_y, dpi)),
                            static_cast<float>(width - inset),
                            static_cast<float>(DipToPx(m.empty_y + panel::kRowHDip, dpi))),
                m.empty_text);
  } else if (m.list_y >= 0) {
    const float circle = DipToPxF(static_cast<float>(panel::kCircleDip), dpi);
    for (int i = 0; i < m.list_n; ++i) {
      if (i < 0 || i >= static_cast<int>(bt_devices_.size())) {
        continue;
      }
      const BtDevice& dev = bt_devices_[static_cast<size_t>(i)];
      const RECT row{inset, DipToPx(m.list_y + i * panel::kRowHDip, dpi), width - inset,
                     DipToPx(m.list_y + (i + 1) * panel::kRowHDip, dpi)};
      if (hot_id == kPageList + i) {
        panel::FillHover(target, brush, row, dpi, dark);
      }
      const float cy = static_cast<float>(row.top + row.bottom) * 0.5f;
      const float cx = static_cast<float>(row.left) + circle * 0.5f;
      panel::DrawRowCircle(target, dwrite_.Get(), fluent14_.Get(), brush, cx, cy, dpi, dark, dev.connected,
                           BtClassGlyph(dev.info.ulClassofDevice));
      float text_r = static_cast<float>(row.right);
      const bool busy = !bt_connecting_addr_.empty() && bt_connecting_addr_ == dev.address &&
                        (GetTickCount64() - bt_connecting_since_ < 60000);
      if (busy) {
        const float status_w = DipToPxF(92.0f, dpi);
        brush->SetColor(muted);
        DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush,
                    D2D1::RectF(static_cast<float>(row.right) - status_w, static_cast<float>(row.top),
                                static_cast<float>(row.right), static_cast<float>(row.bottom)),
                    dev.connected ? L"연결 끊는 중\u2026" : L"연결 중\u2026");
        text_r -= status_w + DipToPxF(8.0f, dpi);
      } else if (dev.battery >= 0) {
        wchar_t pct[16]{};
        swprintf_s(pct, L"%d%%", dev.battery);
        const float bat_w = DipToPxF(36.0f, dpi);
        brush->SetColor(muted);
        DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush,
                    D2D1::RectF(static_cast<float>(row.right) - bat_w, static_cast<float>(row.top),
                                static_cast<float>(row.right), static_cast<float>(row.bottom)),
                    pct);
        text_r -= bat_w + DipToPxF(8.0f, dpi);
      }
      brush->SetColor(dev.connected ? fg : muted);
      DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                  D2D1::RectF(static_cast<float>(DipToPx(panel::kTextLeftDip, dpi)), static_cast<float>(row.top),
                              text_r, static_cast<float>(row.bottom)),
                  dev.name);
    }
  }

  if (m.other_header_y >= 0) {
    panel::DrawSectionHeader(target, dwrite_.Get(), regular12_.Get(), brush, m.other_header_y, dpi, dark,
                             L"다른 장치");
  }
  if (m.scanning_y >= 0) {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                D2D1::RectF(static_cast<float>(inset), static_cast<float>(DipToPx(m.scanning_y, dpi)),
                            static_cast<float>(width - inset),
                            static_cast<float>(DipToPx(m.scanning_y + panel::kRowHDip, dpi))),
                L"검색 중\u2026");
  } else if (m.found_y >= 0) {
    const float circle = DipToPxF(static_cast<float>(panel::kCircleDip), dpi);
    for (int i = 0; i < m.found_n; ++i) {
      if (i < 0 || i >= static_cast<int>(bt_found_.size())) {
        continue;
      }
      const BtDevice& dev = bt_found_[static_cast<size_t>(i)];
      const RECT row{inset, DipToPx(m.found_y + i * panel::kRowHDip, dpi), width - inset,
                     DipToPx(m.found_y + (i + 1) * panel::kRowHDip, dpi)};
      if (hot_id == kPageList + kBtListMax + i) {
        panel::FillHover(target, brush, row, dpi, dark);
      }
      const float cy = static_cast<float>(row.top + row.bottom) * 0.5f;
      const float cx = static_cast<float>(row.left) + circle * 0.5f;
      panel::DrawRowCircle(target, dwrite_.Get(), fluent14_.Get(), brush, cx, cy, dpi, dark, false,
                           BtClassGlyph(dev.info.ulClassofDevice));
      const wchar_t* name = dev.name.empty() ? L"알 수 없는 장치" : dev.name.c_str();
      brush->SetColor(muted);
      DrawTrimmed(target, dwrite_.Get(), regular14_ ? regular14_.Get() : regular12_.Get(), brush,
                  D2D1::RectF(static_cast<float>(DipToPx(panel::kTextLeftDip, dpi)), static_cast<float>(row.top),
                              static_cast<float>(row.right), static_cast<float>(row.bottom)),
                  name);
    }
  }

  panel::DrawDivider(target, brush, m.div2_y, dpi, dark);
  IDWriteTextFormat* settings_fmt = regular13_ ? regular13_.Get() : regular12_.Get();
  if (m.scan_y >= 0 && m.scan_text != nullptr) {
    panel::DrawSettingsRow(target, dwrite_.Get(), settings_fmt, brush, m.scan_y, dpi, dark, hot_id == kPageScan,
                           m.scan_text);
  }
  panel::DrawSettingsRow(target, dwrite_.Get(), settings_fmt, brush, m.settings_y, dpi, dark,
                         hot_id == kPageBtSettings, L"Bluetooth 설정\u2026");
}

void ControlCenterContent::RenderBatteryPage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                             ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  const bool show_remain = !battery_ac_ && !battery_remain_text_.empty();
  const BatteryPageMetrics m = MakeBatteryPage(show_remain);
  const int width = DipToPx(panel::kWidthDip, dpi);
  const int inset = DipToPx(panel::kInsetDip, dpi);
  IDWriteTextFormat* title_fmt = semibold14_ ? semibold14_.Get() : semibold13_.Get();
  IDWriteTextFormat* body_fmt = regular14_ ? regular14_.Get() : regular12_.Get();

  if (show_back_) {
    const RECT back{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                    DipToPx(m.header_y + panel::kHeaderHDip, dpi)};
    if (hot_id == kPageBack) {
      panel::FillHover(target, brush, back, dpi, dark);
    }
    brush->SetColor(fg);
    if (dwrite_ && fluent17_) {
      DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush,
                D2D1::RectF(static_cast<float>(back.left), static_cast<float>(back.top),
                            static_cast<float>(back.right), static_cast<float>(back.bottom)),
                kBackGlyph);
    }
  }

  const float title_l = static_cast<float>(inset + (show_back_ ? DipToPx(panel::kBackWDip + 4, dpi) : 0));
  const float pct_w = DipToPxF(48.0f, dpi);
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), title_fmt, brush,
              D2D1::RectF(title_l, static_cast<float>(DipToPx(m.header_y, dpi)),
                          static_cast<float>(width - inset) - pct_w,
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              L"배터리");
  wchar_t pct[16]{};
  swprintf_s(pct, L"%d%%", static_cast<int>(battery_level_ * 100.0f + 0.5f));
  DrawTrimmed(target, dwrite_.Get(), title_fmt, brush,
              D2D1::RectF(static_cast<float>(width - inset) - pct_w, static_cast<float>(DipToPx(m.header_y, dpi)),
                          static_cast<float>(width - inset),
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              pct, DWRITE_TEXT_ALIGNMENT_TRAILING);

  DrawGaugeBar(target, brush, m.gauge_y, dpi, dark, battery_level_,
               BatteryFillRgb(dark, battery_level_, battery_ac_));
  if (show_remain) {
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular12_.Get(), brush,
                D2D1::RectF(static_cast<float>(inset), static_cast<float>(DipToPx(m.remain_y, dpi)),
                            static_cast<float>(width - inset),
                            static_cast<float>(DipToPx(m.remain_y + panel::kNoteHDip, dpi))),
                battery_remain_text_);
  }

  panel::DrawDivider(target, brush, m.div1_y, dpi, dark);

  const RECT saver{inset, DipToPx(m.saver_y, dpi), width - inset, DipToPx(m.saver_y + panel::kRowHDip, dpi)};
  if (hot_id == kPageToggle || hot_id == kPageSaverSettings) {
    panel::FillHover(target, brush, saver, dpi, dark);
  }
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), body_fmt, brush,
              D2D1::RectF(static_cast<float>(inset), static_cast<float>(saver.top),
                          static_cast<float>(width - inset - DipToPx(panel::kToggleWDip + 8, dpi)),
                          static_cast<float>(saver.bottom)),
              L"절전 모드");
  const RECT toggle{width - inset - DipToPx(panel::kToggleWDip, dpi),
                    DipToPx(m.saver_y, dpi) + (DipToPx(panel::kRowHDip, dpi) - DipToPx(panel::kToggleHDip, dpi)) / 2,
                    width - inset,
                    DipToPx(m.saver_y, dpi) + (DipToPx(panel::kRowHDip, dpi) - DipToPx(panel::kToggleHDip, dpi)) / 2 +
                        DipToPx(panel::kToggleHDip, dpi)};
  panel::DrawToggle(target, brush, toggle, dpi, saver_on_, battery_saver_toggle_ok_ && !battery_ac_, dark);

  const wchar_t* power = L"알 수 없음";
  if (battery_ok_) {
    power = battery_ac_ ? L"연결됨" : L"배터리 사용 중";
  }
  DrawKvRow(target, dwrite_.Get(), body_fmt, brush, m.power_y, dpi, dark, L"전원", power);

  panel::DrawDivider(target, brush, m.div2_y, dpi, dark);
  panel::DrawSettingsRow(target, dwrite_.Get(), regular13_ ? regular13_.Get() : regular12_.Get(), brush,
                         m.settings_y, dpi, dark, hot_id == kPagePowerSettings, L"전원 설정\u2026");
}

void ControlCenterContent::RenderCpuPage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                         ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const CpuPageMetrics m = MakeCpuPage();
  const int width = DipToPx(panel::kWidthDip, dpi);
  const int inset = DipToPx(panel::kInsetDip, dpi);
  IDWriteTextFormat* title_fmt = semibold14_ ? semibold14_.Get() : semibold13_.Get();
  IDWriteTextFormat* body_fmt = regular14_ ? regular14_.Get() : regular12_.Get();

  if (show_back_) {
    const RECT back{inset, DipToPx(m.header_y, dpi), inset + DipToPx(panel::kBackWDip, dpi),
                    DipToPx(m.header_y + panel::kHeaderHDip, dpi)};
    if (hot_id == kPageBack) {
      panel::FillHover(target, brush, back, dpi, dark);
    }
    brush->SetColor(fg);
    if (dwrite_ && fluent17_) {
      DrawGlyph(target, dwrite_.Get(), fluent17_.Get(), brush,
                D2D1::RectF(static_cast<float>(back.left), static_cast<float>(back.top),
                            static_cast<float>(back.right), static_cast<float>(back.bottom)),
                kBackGlyph);
    }
  }

  const float title_l = static_cast<float>(inset + (show_back_ ? DipToPx(panel::kBackWDip + 4, dpi) : 0));
  const float pct_w = DipToPxF(48.0f, dpi);
  brush->SetColor(fg);
  DrawTrimmed(target, dwrite_.Get(), title_fmt, brush,
              D2D1::RectF(title_l, static_cast<float>(DipToPx(m.header_y, dpi)),
                          static_cast<float>(width - inset) - pct_w,
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              L"CPU");
  wchar_t pct[16]{};
  swprintf_s(pct, L"%d%%", static_cast<int>(cpu_usage_ * 100.0f + 0.5f));
  DrawTrimmed(target, dwrite_.Get(), title_fmt, brush,
              D2D1::RectF(static_cast<float>(width - inset) - pct_w, static_cast<float>(DipToPx(m.header_y, dpi)),
                          static_cast<float>(width - inset),
                          static_cast<float>(DipToPx(m.header_y + panel::kHeaderHDip, dpi))),
              pct, DWRITE_TEXT_ALIGNMENT_TRAILING);

  DrawGaugeBar(target, brush, m.gauge_y, dpi, dark, cpu_usage_, CpuFillRgb(dark, cpu_usage_));
  panel::DrawDivider(target, brush, m.div1_y, dpi, dark);

  wchar_t user[16]{};
  wchar_t kernel[16]{};
  wchar_t nproc[16]{};
  swprintf_s(user, L"%d%%", static_cast<int>(cpu_user_ * 100.0f + 0.5f));
  swprintf_s(kernel, L"%d%%", static_cast<int>(cpu_kernel_ * 100.0f + 0.5f));
  swprintf_s(nproc, L"%u", cpu_nproc_);
  DrawKvRow(target, dwrite_.Get(), body_fmt, brush, m.user_y, dpi, dark, L"사용자", user);
  DrawKvRow(target, dwrite_.Get(), body_fmt, brush, m.kernel_y, dpi, dark, L"커널", kernel);
  DrawKvRow(target, dwrite_.Get(), body_fmt, brush, m.nproc_y, dpi, dark, L"논리 프로세서", nproc);

  panel::DrawDivider(target, brush, m.div2_y, dpi, dark);
  panel::DrawSettingsRow(target, dwrite_.Get(), regular13_ ? regular13_.Get() : regular12_.Get(), brush,
                         m.settings_y, dpi, dark, hot_id == kPageTaskManager, L"작업 관리자\u2026");
}

void ControlCenterContent::RenderListPage(ID2D1RenderTarget* target, UINT dpi, int hot_id,
                                          ID2D1SolidColorBrush* brush) {
  if (target == nullptr || brush == nullptr) {
    return;
  }
  const bool dark = host_.dark;
  const D2D1_COLOR_F fg = ClockTextColor(dark);
  const D2D1_COLOR_F muted = ScaleAlpha(fg, 0.55f);
  const float radius = corner::ToPx(corner::kOverlayDip, dpi);
  const int pad = DipToPx(kPanelPadDip, dpi);
  const int width = DipToPx(panel::kWidthDip, dpi);
  auto fill_round = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               radius, radius};
    target->FillRoundedRectangle(rr, brush);
  };
  auto fill_hover = [&](const RECT& rc, D2D1_COLOR_F color) {
    brush->SetColor(color);
    const float hover_r = corner::HoverPx(static_cast<float>(rc.bottom - rc.top), dpi);
    const D2D1_ROUNDED_RECT rr{D2D1::RectF(static_cast<float>(rc.left), static_cast<float>(rc.top),
                                           static_cast<float>(rc.right), static_cast<float>(rc.bottom)),
                               hover_r, hover_r};
    target->FillRoundedRectangle(rr, brush);
  };

  const bool on = bt_on_;
  const wchar_t* title = L"Bluetooth";
  if (show_back_) {
    if (hot_id == kPageBack) {
      fill_hover(RECT{pad, pad, pad + DipToPx(32, dpi), pad + DipToPx(kPageHeaderHDip, dpi)},
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
  const float toggle_pill = corner::PillPx(th);
  brush->SetColor(on ? AccentFillColor(dark) : BadgeOffFill(dark));
  target->FillRoundedRectangle(
      D2D1_ROUNDED_RECT{D2D1::RectF(static_cast<float>(toggle.left), static_cast<float>(toggle.top),
                                    static_cast<float>(toggle.right), static_cast<float>(toggle.bottom)),
                        toggle_pill, toggle_pill},
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
                L"장치가 없습니다");
  }
  for (int i = 0; i < n; ++i) {
    const RECT row{pad, list_top + i * row_h, width - pad, list_top + (i + 1) * row_h};
    if (hot_id == kPageList + i) {
      fill_round(row, MenuItemHoverFill(dark, false));
    }
    std::wstring name;
    std::wstring sub;
    bool connected = false;
    if (i < static_cast<int>(bt_devices_.size())) {
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
                kBtGlyph);
    }
    const float text_l = static_cast<float>(row.left + DipToPx(44, dpi));
    const float text_r = static_cast<float>(row.right - DipToPx(12, dpi));
    const float mid = static_cast<float>(row.top + row.bottom) * 0.5f;
    brush->SetColor(fg);
    DrawTrimmed(target, dwrite_.Get(), semibold13_.Get(), brush,
                D2D1::RectF(text_l, static_cast<float>(row.top + DipToPx(6, dpi)), text_r, mid), name);
    brush->SetColor(muted);
    DrawTrimmed(target, dwrite_.Get(), regular11_.Get(), brush,
                D2D1::RectF(text_l, mid, text_r, static_cast<float>(row.bottom - DipToPx(6, dpi))), sub);
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
              L"추가 Bluetooth 설정");
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
      EnsureWlanNotify();
      RefreshPageLists(true);
      break;
    case kBluetooth:
      page_ = Page::kBluetooth;
      list_due_ = 0;
      CloseWifiPassword();
      StopWlanNotify();
      RefreshPageLists(true);
      break;
    case kPageBack:
      CloseWifiPassword();
      StopWlanNotify();
      page_ = Page::kHome;
      break;
    case kPageToggle:
      if (page_ == Page::kWifi) {
        if (!wifi_hw_radio_on_) {
          break;
        }
        SetWifiRadio(!wifi_radio_on_);
      } else if (page_ == Page::kBluetooth) {
        if (!bt_can_toggle_) {
          OpenSettingsPage(L"ms-settings:bluetooth");
          break;
        }
        if (host_.dispatch) {
          StatusEvent ev;
          ev.id = "bamti.widget/bluetooth";
          ev.event = "toggle";
          ev.row_id = "radio";
          ev.on = !bt_on_;
          host_.dispatch(ev);
        }
        list_due_ = 0;
        RefreshPageLists(true);
      } else if (page_ == Page::kBattery) {
        if (!battery_saver_toggle_ok_) {
          OpenSettingsPage(L"ms-settings:batterysaver");
          break;
        }
        if (battery_ac_) {
          break;
        }
        if (host_.dispatch) {
          StatusEvent ev;
          ev.id = "bamti.widget/battery";
          ev.event = "toggle";
          ev.row_id = "saver";
          ev.on = !saver_on_;
          host_.dispatch(ev);
        }
      }
      break;
    case kPageScan: {
      const bool start = !bt_scanning_;
      if (start) {
        bt_scanning_ = true;
      } else {
        bt_scanning_ = false;
        bt_found_.clear();
      }
      if (host_.dispatch) {
        StatusEvent ev;
        ev.id = "bamti.widget/bluetooth";
        ev.event = "toggle";
        ev.row_id = "bt_scan";
        ev.on = start;
        host_.dispatch(ev);
      }
      break;
    }
    case kPageMore:
    case kPageBtSettings:
      OpenSettingsPage(L"ms-settings:bluetooth");
      break;
    case kPageNetworkSettings:
      OpenSettingsPage(L"ms-settings:network");
      break;
    case kPageWifiSettings:
      OpenSettingsPage(L"ms-settings:network-wifi");
      break;
    case kPageSoundSettings:
      OpenSettingsPage(L"ms-settings:sound");
      break;
    case kPageDeviceSettings:
      OpenSettingsPage(L"ms-settings:bluetooth");
      break;
    case kPagePowerSettings:
      if (host_.dispatch) {
        StatusEvent ev;
        ev.id = "bamti.widget/battery";
        ev.event = "invoke";
        ev.row_id = "power_settings";
        host_.dispatch(ev);
      }
      break;
    case kPageTaskManager:
      if (host_.dispatch) {
        StatusEvent ev;
        ev.id = "bamti.widget/cpu";
        ev.event = "invoke";
        ev.row_id = "task_manager";
        host_.dispatch(ev);
      }
      break;
    case kPageSaverSettings:
      OpenSettingsPage(L"ms-settings:batterysaver");
      break;
    case kSaver:
      if (battery_ac_) {
        break;
      }
      if (!battery_saver_toggle_ok_) {
        OpenSettingsPage(L"ms-settings:batterysaver");
        break;
      }
      if (host_.dispatch) {
        StatusEvent ev;
        ev.id = "bamti.widget/battery";
        ev.event = "toggle";
        ev.row_id = "saver";
        ev.on = !saver_on_;
        host_.dispatch(ev);
      }
      saver_on_ = !saver_on_;
      PresentHost();
      break;
    case kNight:
      if (!night_known_ || !SetNightLight(!night_on_)) {
        OpenSettingsPage(L"ms-settings:night-light");
        break;
      }
      night_on_ = !night_on_;
      PresentHost();
      break;
    case kSettings:
      OpenSettingsPage(L"ms-settings:");
      break;
    default:
      if (hit.id >= kPageList && hit.id < kPageList + kBtListMax + kBtScanMax) {
        const int i = hit.extra;
        if (page_ == Page::kWifi && i >= 0 && i < static_cast<int>(wifi_nets_.size())) {
          const WifiNetwork& net = wifi_nets_[static_cast<size_t>(i)];
          if (!net.connected) {
            Log(L"cc", L"wifi click ssid=%s auth=%lu kind=%s", net.ssid.c_str(),
                static_cast<unsigned long>(net.auth), WifiKindName(net.kind));
            if (net.has_profile) {
              ConnectWifiProfile(net.ssid);
            } else if (net.kind == WifiKind::kOpen) {
              ConnectWifi(net);
            } else if (net.kind == WifiKind::kPersonal) {
              const wchar_t* auth_xml = nullptr;
              const wchar_t* enc_xml = nullptr;
              bool wep = false;
              if (!MapWifiSecurity(net.auth, net.cipher, &auth_xml, &enc_xml, &wep)) {
                FallbackWifi(net, L"cipher");
              } else {
                OpenWifiPassword(i, {});
              }
            } else {
              FallbackWifi(net, WifiKindName(net.kind));
            }
          }
        } else if (page_ == Page::kVolume && i >= 0 && i < static_cast<int>(audio_outs_.size())) {
          const AudioEndpoint& dev = audio_outs_[static_cast<size_t>(i)];
          if (!dev.is_default && SetDefaultAudioOutput(dev.id)) {
            audio_outs_ = EnumAudioOutputs();
            if (audio_outs_.size() > static_cast<size_t>(kAudioListMax)) {
              audio_outs_.resize(static_cast<size_t>(kAudioListMax));
            }
            if (host_.dispatch) {
              StatusEvent ev;
              ev.id = "bamti.widget/volume";
              ev.event = "change";
              ev.row_id = "volume_device";
              host_.dispatch(ev);
            }
            PresentHost();
          }
        } else if (page_ == Page::kBluetooth && i >= 0 && i < static_cast<int>(bt_devices_.size())) {
          BtDevice& dev = bt_devices_[static_cast<size_t>(i)];
          if (dev.paired) {
            if (!bt_connecting_addr_.empty() && bt_connecting_addr_ == dev.address &&
                (GetTickCount64() - bt_connecting_since_ < 60000)) {
              break;
            }
            if (host_.bt_connect) {
              bt_connecting_addr_ = dev.address;
              bt_connecting_since_ = GetTickCount64();
              host_.bt_connect(dev.address, !dev.connected);
            } else {
              OpenSettingsPage(L"ms-settings:bluetooth");
            }
          } else {
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
        } else if (page_ == Page::kBluetooth && i >= kBtListMax) {
          const int fi = i - kBtListMax;
          if (fi >= 0 && fi < static_cast<int>(bt_found_.size())) {
            BtDevice& dev = bt_found_[static_cast<size_t>(fi)];
            BLUETOOTH_FIND_RADIO_PARAMS params{};
            params.dwSize = sizeof(params);
            HANDLE radio = nullptr;
            const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
            DWORD err = static_cast<DWORD>(-1);
            if (find != nullptr) {
              if (radio != nullptr) {
                err = BluetoothAuthenticateDeviceEx(nullptr, radio, &dev.info, nullptr, MITMProtectionNotRequired);
                Log(L"cc", L"bt auth %s err=%lu",
                    dev.name.empty() ? L"알 수 없는 장치" : dev.name.c_str(), static_cast<unsigned long>(err));
                CloseHandle(radio);
              }
              BluetoothFindRadioClose(find);
            }
            if (err == ERROR_SUCCESS) {
              bt_found_.erase(bt_found_.begin() + fi);
            }
            list_due_ = 0;
            RefreshPageLists(true);
          }
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
  if (id == kWifi || id == kBluetooth || id == kPageBack || id == kPageToggle || id == kPageScan ||
      (id == kSaver && battery_ac_)) {
    return true;
  }
  return id >= kPageList && id < kPageList + kBtListMax + kBtScanMax;
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
  const RECT track = (page_ == Page::kVolume && hit.id == kVolume) ? VolumeSliderTrackRect(dpi)
                                                                 : SliderTrackRect(dpi, hit.id == kBrightness);
  const float knob_r = DipToPxF(kVolumeKnobDip, dpi) * 0.5f;
  const SliderGeometry geom{static_cast<float>(track.left) + knob_r, static_cast<float>(track.right) - knob_r, knob_r};
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
