#pragma once

#include "popup_surface.hpp"
#include "status_source.hpp"

#include <functional>
#include <string>
#include <vector>

namespace bamti {

struct WlanStatus {
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

class ControlCenterContent : public PopupContent {
 public:
  void Reset(ControlCenterHost host);
  void Refresh();

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
  };

  struct Hit {
    RECT rc{};
    int id = -1;
  };

  void QuerySlowState(bool force);
  void ApplyLive();
  RECT TileRect(UINT dpi, int col, int row) const;
  RECT SliderRect(UINT dpi, bool brightness) const;
  RECT FooterRect(UINT dpi) const;
  RECT SettingsRect(UINT dpi) const;
  int HeightDip() const;

  ControlCenterHost host_{};
  std::vector<Hit> hits_;
  int drag_id_ = -1;
  float drag_value_ = 0.0f;
  float volume_ = 0.0f;
  float brightness_ = 0.0f;
  bool volume_ok_ = false;
  bool brightness_ok_ = false;
  bool muted_ = false;
  bool battery_present_ = false;
  bool charging_ = false;
  float battery_ = 0.0f;
  bool saver_on_ = false;
  bool wifi_on_ = false;
  std::wstring wifi_name_ = L"연결 안 됨";
  bool bt_on_ = false;
  bool bt_known_ = false;
  ULONGLONG slow_due_ = 0;
};

}  // namespace bamti
