#pragma once

#include <atomic>
#include <memory>
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
  VolumeControl();
  VolumeControl(const VolumeControl&) = delete;
  VolumeControl& operator=(const VolumeControl&) = delete;
  ~VolumeControl();

  void BindWake(HANDLE wake);
  bool TakeNotifyDirty();
  LONGLONG TakeNotifyQpc();

  VolumeState Read();
  bool SetLevel(float level);
  bool SetMute(bool muted);
  void Release();
  void Invalidate();

 private:
  bool Ensure();

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator_;
  Microsoft::WRL::ComPtr<IAudioEndpointVolume> volume_;
  Microsoft::WRL::ComPtr<IAudioEndpointVolumeCallback> notify_;
  std::shared_ptr<std::atomic<bool>> dirty_;
  std::shared_ptr<std::atomic<LONGLONG>> notify_qpc_;
  HANDLE wake_event_ = nullptr;
  std::wstring device_;
  bool logged_cost_ = false;
  bool logged_fail_ = false;
  bool logged_read_ = false;
  bool logged_notify_fail_ = false;
};

}  // namespace bamti
