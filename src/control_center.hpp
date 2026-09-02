#pragma once

#include "popup_surface.hpp"
#include "status_source.hpp"

#include <bluetoothapis.h>
#include <dwrite.h>
#include <wrl/client.h>

#include <functional>
#include <string>
#include <vector>

namespace bamti {

struct WlanStatus {
  bool radio = false;
  bool connected = false;
  std::wstring name = L"연결 안 됨";
  double ms = 0.0;
};

WlanStatus QueryWlanStatus();

struct ControlCenterLive {
  bool volume_ok = false;
  float volume = 0.0f;
  bool muted = false;
  bool brightness_ok = false;
  float brightness = 0.0f;
  bool wifi_on = false;
  std::wstring wifi_name = L"연결 안 됨";
};

struct ControlCenterHost {
  bool dark = false;
  std::function<void(const StatusEvent&)> dispatch;
  std::function<ControlCenterLive()> live;
};

enum class ControlCenterPage { kHome, kWifi };

class ControlCenterContent : public PopupContent {
 public:
  void Reset(ControlCenterHost host, ControlCenterPage page = ControlCenterPage::kHome);
  void Refresh();
  bool ShowsWifi() const;

  SIZE Measure(UINT dpi) override;
  void Render(ID2D1RenderTarget* target, UINT dpi, int hot_index) override;
  int HitTest(POINT client, UINT dpi) const override;
  void Invoke(int index) override;
  bool StickyRow(int index) const override;
  void StickyInvoke(int index) override;
  bool DragRow(int index) const override;
  void DragTo(int index, POINT client, UINT dpi) override;
  void DragEnd(int index) override;

 private:
  enum HitId {
    kWifi = 0,
    kBluetooth,
    kAirplane,
    kSaver,
    kNight,
    kAccess,
    kVolume,
    kBrightness,
    kSettings,
    kPageBack,
    kPageToggle,
    kPageMore,
    kPageList = 200,
    kPageAction = 400,
  };

  enum class Page { kHome, kWifi, kBluetooth };

  struct Hit {
    RECT rc{};
    int id = -1;
    int extra = -1;
  };

  struct WifiNetwork {
    std::wstring ssid;
    bool connected = false;
    bool secure = true;
    bool has_profile = false;
  };

  struct BtDevice {
    std::wstring name;
    BLUETOOTH_DEVICE_INFO info{};
    bool connected = false;
  };

  void QuerySlowState(bool force);
  void ApplyLive();
  void EnsureFormats(UINT dpi);
  void BuildHits(UINT dpi);
  void RefreshPageLists(bool force);
  void RenderListPage(ID2D1RenderTarget* target, UINT dpi, int hot_id, ID2D1SolidColorBrush* brush);
  RECT ConnectRowRect(UINT dpi, int row) const;
  RECT QuickTileRect(UINT dpi, int col, int row) const;
  RECT SliderTrackRect(UINT dpi, bool brightness) const;
  RECT FooterRect(UINT dpi) const;
  RECT SettingsRect(UINT dpi) const;
  int HeightDip() const;
  int ListCount() const;

  ControlCenterHost host_{};
  std::vector<Hit> hits_;
  int drag_id_ = -1;
  float drag_value_ = 0.0f;
  float volume_ = 0.0f;
  float brightness_ = 0.0f;
  bool volume_ok_ = false;
  bool brightness_ok_ = false;
  bool muted_ = false;
  bool saver_on_ = false;
  bool wifi_on_ = false;
  std::wstring wifi_name_ = L"연결 안 됨";
  bool wifi_radio_on_ = false;
  bool bt_on_ = false;
  bool bt_known_ = false;
  Page page_ = Page::kHome;
  bool show_back_ = true;
  GUID wifi_iface_{};
  bool wifi_iface_ok_ = false;
  std::vector<WifiNetwork> wifi_nets_;
  std::vector<BtDevice> bt_devices_;
  ULONGLONG list_due_ = 0;
  ULONGLONG slow_due_ = 0;
  UINT format_dpi_ = 0;
  Microsoft::WRL::ComPtr<IDWriteFactory> dwrite_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent17_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent15_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> fluent14_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> semibold13_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> regular12_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> regular11_;
  Microsoft::WRL::ComPtr<IDWriteTextFormat> center11_;
};

}  // namespace bamti
