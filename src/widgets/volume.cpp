#include "widgets/volume.hpp"

#include "log.hpp"

#include <initguid.h>
#include <audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <objbase.h>
#include <propkey.h>
#include <propvarutil.h>

#include <cmath>

namespace bamti {
namespace {

float ClampUnit(float value) {
  if (!std::isfinite(value) || value < 0.0f) {
    return 0.0f;
  }
  if (value > 1.0f) {
    return 1.0f;
  }
  return value;
}

double ElapsedMs(const LARGE_INTEGER& freq, const LARGE_INTEGER& t0, const LARGE_INTEGER& t1) {
  if (freq.QuadPart == 0) {
    return 0.0;
  }
  return static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(freq.QuadPart);
}

}  // namespace

VolumeControl::~VolumeControl() {
  Release();
}

void VolumeControl::Release() {
  volume_.Reset();
  enumerator_.Reset();
  device_.clear();
}

bool VolumeControl::Ensure() {
  if (enumerator_ && volume_) {
    logged_fail_ = false;
    return true;
  }

  LARGE_INTEGER freq{};
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);

  if (!enumerator_) {
    const HRESULT hr =
        CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator_));
    if (FAILED(hr) || !enumerator_) {
      Release();
      if (!logged_fail_) {
        logged_fail_ = true;
        Log(L"widget", L"volume enumerator failed hr=0x%08lx", static_cast<unsigned long>(hr));
      }
      return false;
    }
  }

  if (!volume_) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    HRESULT hr = enumerator_->GetDefaultAudioEndpoint(eRender, eMultimedia, device.GetAddressOf());
    if (FAILED(hr) || !device) {
      Release();
      if (!logged_fail_) {
        logged_fail_ = true;
        Log(L"widget", L"volume endpoint missing hr=0x%08lx", static_cast<unsigned long>(hr));
      }
      return false;
    }
    hr = device->Activate(__uuidof(IAudioEndpointVolume), CLSCTX_ALL, nullptr,
                          reinterpret_cast<void**>(volume_.ReleaseAndGetAddressOf()));
    if (FAILED(hr) || !volume_) {
      Release();
      if (!logged_fail_) {
        logged_fail_ = true;
        Log(L"widget", L"volume activate failed hr=0x%08lx", static_cast<unsigned long>(hr));
      }
      return false;
    }
    device_.clear();
    Microsoft::WRL::ComPtr<IPropertyStore> props;
    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf())) && props) {
      PROPVARIANT name{};
      PropVariantInit(&name);
      if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
          name.pwszVal != nullptr) {
        device_ = name.pwszVal;
      }
      PropVariantClear(&name);
    }
    QueryPerformanceCounter(&t1);
    if (!logged_cost_) {
      logged_cost_ = true;
      Log(L"widget", L"volume endpoint acquire took %.2f ms", ElapsedMs(freq, t0, t1));
    }
  }

  logged_fail_ = false;
  return true;
}

VolumeState VolumeControl::Read() {
  VolumeState state;
  if (!Ensure() || !volume_) {
    return state;
  }
  LARGE_INTEGER freq{};
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceFrequency(&freq);
  QueryPerformanceCounter(&t0);
  float level = 0.0f;
  BOOL muted = FALSE;
  const HRESULT level_hr = volume_->GetMasterVolumeLevelScalar(&level);
  const HRESULT mute_hr = volume_->GetMute(&muted);
  QueryPerformanceCounter(&t1);
  if (FAILED(level_hr) || FAILED(mute_hr)) {
    Release();
    return state;
  }
  if (!logged_read_) {
    logged_read_ = true;
    Log(L"widget", L"volume read took %.2f ms", ElapsedMs(freq, t0, t1));
  }
  state.ok = true;
  state.level = ClampUnit(level);
  state.muted = muted != FALSE;
  state.device = device_;
  return state;
}

bool VolumeControl::SetLevel(float level) {
  if (!Ensure() || !volume_) {
    return false;
  }
  const float want = ClampUnit(level);
  const HRESULT hr = volume_->SetMasterVolumeLevelScalar(want, nullptr);
  if (FAILED(hr)) {
    Release();
    return false;
  }
  if (want <= 0.02f || want >= 0.98f) {
    float got = 0.0f;
    if (SUCCEEDED(volume_->GetMasterVolumeLevelScalar(&got))) {
      Log(L"widget", L"volume set %.4f -> read %.4f", want, got);
    }
  }
  return true;
}

bool VolumeControl::SetMute(bool muted) {
  if (!Ensure() || !volume_) {
    return false;
  }
  const HRESULT hr = volume_->SetMute(muted ? TRUE : FALSE, nullptr);
  if (FAILED(hr)) {
    Release();
    return false;
  }
  return true;
}

}  // namespace bamti
