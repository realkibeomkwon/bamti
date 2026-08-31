#pragma once

#include <string>
#include <windows.h>
#include <endpointvolume.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

namespace bamti {

struct VolumeState {
  bool ok = false;
  float level = 0.0f;  // 0.0 ~ 1.0
  bool muted = false;
  std::wstring device;  // 기본 출력 장치의 표시 이름
};

class VolumeControl {
 public:
  VolumeControl() = default;
  VolumeControl(const VolumeControl&) = delete;
  VolumeControl& operator=(const VolumeControl&) = delete;
  ~VolumeControl();

  VolumeState Read();
  bool SetLevel(float level);
  bool SetMute(bool muted);
  void Release();

 private:
  bool Ensure();

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume_;
  std::wstring device_;
  bool logged_cost_ = false;
  bool logged_fail_ = false;
  bool logged_read_ = false;
};

}  // namespace bamti
