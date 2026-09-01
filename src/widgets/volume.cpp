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

// 우리가 낸 변경임을 알아보기 위한 표식이다. New-Guid: 8b1318eb-4c3e-461b-a46b-44bd420555ea
constexpr GUID kVolumeEventContext = {0x8b1318eb, 0x4c3e, 0x461b, {0xa4, 0x6b, 0x44, 0xbd, 0x42, 0x05, 0x55, 0xea}};

class VolumeNotify final : public IAudioEndpointVolumeCallback {
 public:
  VolumeNotify(HANDLE wake_src, std::shared_ptr<std::atomic<bool>> dirty,
               std::shared_ptr<std::atomic<LONGLONG>> notify_qpc)
      : dirty_(std::move(dirty)), notify_qpc_(std::move(notify_qpc)) {
    if (wake_src == nullptr) {
      return;
    }
    if (!DuplicateHandle(GetCurrentProcess(), wake_src, GetCurrentProcess(), &wake_, 0, FALSE,
                         DUPLICATE_SAME_ACCESS)) {
      dup_err_ = GetLastError();
      wake_ = nullptr;
    }
  }

  VolumeNotify(const VolumeNotify&) = delete;
  VolumeNotify& operator=(const VolumeNotify&) = delete;

  bool has_wake() const { return wake_ != nullptr; }
  DWORD dup_err() const { return dup_err_; }

  ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&ref_)); }

  ULONG STDMETHODCALLTYPE Release() override {
    const LONG n = InterlockedDecrement(&ref_);
    if (n == 0) {
      if (wake_ != nullptr) {
        CloseHandle(wake_);
        wake_ = nullptr;
      }
      delete this;
    }
    return static_cast<ULONG>(n);
  }

  HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppv) override {
    if (ppv == nullptr) {
      return E_POINTER;
    }
    if (riid == IID_IUnknown || riid == __uuidof(IAudioEndpointVolumeCallback)) {
      *ppv = static_cast<IAudioEndpointVolumeCallback*>(this);
      AddRef();
      return S_OK;
    }
    *ppv = nullptr;
    return E_NOINTERFACE;
  }

  HRESULT STDMETHODCALLTYPE OnNotify(PAUDIO_VOLUME_NOTIFICATION_DATA data) override {
    AddRef();
    if (data != nullptr && data->guidEventContext != kVolumeEventContext) {
      LARGE_INTEGER qpc{};
      QueryPerformanceCounter(&qpc);
      if (notify_qpc_) {
        notify_qpc_->store(qpc.QuadPart, std::memory_order_relaxed);
      }
      if (dirty_) {
        dirty_->store(true, std::memory_order_release);
      }
      if (wake_ != nullptr) {
        SetEvent(wake_);
      }
    }
    Release();
    return S_OK;
  }

 private:
  HANDLE wake_ = nullptr;
  DWORD dup_err_ = 0;
  std::shared_ptr<std::atomic<bool>> dirty_;
  std::shared_ptr<std::atomic<LONGLONG>> notify_qpc_;
  LONG ref_ = 1;
};

}  // namespace

VolumeControl::VolumeControl()
    : dirty_(std::make_shared<std::atomic<bool>>(false)),
      notify_qpc_(std::make_shared<std::atomic<LONGLONG>>(0)) {}

VolumeControl::~VolumeControl() {
  Release();
}

void VolumeControl::BindWake(HANDLE wake) {
  wake_event_ = wake;
}

bool VolumeControl::TakeNotifyDirty() {
  if (!dirty_) {
    return false;
  }
  return dirty_->exchange(false, std::memory_order_acq_rel);
}

LONGLONG VolumeControl::TakeNotifyQpc() {
  if (!notify_qpc_) {
    return 0;
  }
  return notify_qpc_->exchange(0, std::memory_order_relaxed);
}

void VolumeControl::Release() {
  if (volume_ && notify_) {
    volume_->UnregisterControlChangeNotify(notify_.Get());
  }
  notify_.Reset();
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
    if (!notify_ && wake_event_ != nullptr && dirty_ && notify_qpc_) {
      auto* cb = new VolumeNotify(wake_event_, dirty_, notify_qpc_);
      if (!cb->has_wake()) {
        const DWORD dup_err = cb->dup_err();
        cb->Release();
        if (!logged_notify_fail_) {
          logged_notify_fail_ = true;
          Log(L"widget", L"volume notify duplicate wake failed err=%lu", dup_err);
        }
      } else {
        hr = volume_->RegisterControlChangeNotify(cb);
        if (FAILED(hr)) {
          cb->Release();
          if (!logged_notify_fail_) {
            logged_notify_fail_ = true;
            Log(L"widget", L"volume notify register failed hr=0x%08lx", static_cast<unsigned long>(hr));
          }
        } else {
          notify_ = cb;
          cb->Release();
          logged_notify_fail_ = false;
        }
      }
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
  const HRESULT hr = volume_->SetMasterVolumeLevelScalar(want, &kVolumeEventContext);
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
  const HRESULT hr = volume_->SetMute(muted ? TRUE : FALSE, &kVolumeEventContext);
  if (FAILED(hr)) {
    Release();
    return false;
  }
  return true;
}

}  // namespace bamti
