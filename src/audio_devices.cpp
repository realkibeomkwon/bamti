#include "audio_devices.hpp"

#include "log.hpp"

#include <initguid.h>
#include <mmdeviceapi.h>
#include <propkey.h>
#include <functiondiscoverykeys_devpkey.h>
#include <propvarutil.h>
#include <wrl/client.h>

#include <algorithm>

namespace bamti {
namespace {

// IPolicyConfig is undocumented. Slot 13 (0-based, including IUnknown) is
// SetDefaultEndpoint. Verify on each Windows build before trusting a call.
constexpr CLSID kClsidPolicyConfigClient = {
    0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
constexpr IID kIidPolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

struct IPolicyConfig : IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, void**) = 0;
  virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, void*, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void*) = 0;
  virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY&, PROPVARIANT*) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR device_id, ERole role) = 0;
  virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};

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

AudioForm FormFromEndpoint(int form) {
  switch (form) {
    case Speakers:
      return AudioForm::kSpeakers;
    case Headphones:
      return AudioForm::kHeadphones;
    case Headset:
      return AudioForm::kHeadset;
    case DigitalAudioDisplayDevice:
      return AudioForm::kDisplay;
    case SPDIF:
    case UnknownDigitalPassthrough:
      return AudioForm::kDigital;
    default:
      return AudioForm::kUnknown;
  }
}

bool EnumeratorLooksBluetooth(const wchar_t* name) {
  if (name == nullptr) {
    return false;
  }
  return _wcsicmp(name, L"BTHENUM") == 0 || _wcsicmp(name, L"BTHHFENUM") == 0;
}

}  // namespace

const wchar_t* AudioFormGlyph(AudioForm form) {
  switch (form) {
    case AudioForm::kSpeakers:
      return L"\xE7F5";
    case AudioForm::kHeadphones:
    case AudioForm::kHeadset:
      return L"\xE7F6";
    case AudioForm::kDisplay:
    case AudioForm::kDigital:
      return L"\xE7F4";
    case AudioForm::kUnknown:
    default:
      return L"\xE767";
  }
}

std::vector<AudioEndpoint> EnumAudioOutputs() {
  std::vector<AudioEndpoint> out;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);

  Microsoft::WRL::ComPtr<IMMDeviceEnumerator> enumerator;
  HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));
  if (FAILED(hr) || !enumerator) {
    QueryPerformanceCounter(&t1);
    Log(L"audio", L"enum enumerator failed hr=0x%08lx took %.2f ms", static_cast<unsigned long>(hr), QpcMs(t0, t1));
    return out;
  }

  std::wstring default_id;
  Microsoft::WRL::ComPtr<IMMDevice> def;
  if (SUCCEEDED(enumerator->GetDefaultAudioEndpoint(eRender, eMultimedia, def.GetAddressOf())) && def) {
    LPWSTR id = nullptr;
    if (SUCCEEDED(def->GetId(&id)) && id != nullptr) {
      default_id = id;
      CoTaskMemFree(id);
    }
  }

  Microsoft::WRL::ComPtr<IMMDeviceCollection> col;
  hr = enumerator->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, col.GetAddressOf());
  if (FAILED(hr) || !col) {
    QueryPerformanceCounter(&t1);
    Log(L"audio", L"enum endpoints failed hr=0x%08lx took %.2f ms", static_cast<unsigned long>(hr), QpcMs(t0, t1));
    return out;
  }

  UINT n = 0;
  col->GetCount(&n);
  out.reserve(n);
  for (UINT i = 0; i < n; ++i) {
    Microsoft::WRL::ComPtr<IMMDevice> device;
    if (FAILED(col->Item(i, device.GetAddressOf())) || !device) {
      continue;
    }
    AudioEndpoint row;
    LPWSTR id = nullptr;
    if (FAILED(device->GetId(&id)) || id == nullptr) {
      continue;
    }
    row.id = id;
    CoTaskMemFree(id);
    row.is_default = !default_id.empty() && row.id == default_id;

    Microsoft::WRL::ComPtr<IPropertyStore> props;
    if (FAILED(device->OpenPropertyStore(STGM_READ, props.GetAddressOf())) || !props) {
      continue;
    }
    PROPVARIANT name{};
    PropVariantInit(&name);
    if (SUCCEEDED(props->GetValue(PKEY_Device_FriendlyName, &name)) && name.vt == VT_LPWSTR &&
        name.pwszVal != nullptr) {
      row.name = name.pwszVal;
    }
    PropVariantClear(&name);
    if (row.name.empty()) {
      continue;
    }

    PROPVARIANT form{};
    PropVariantInit(&form);
    if (SUCCEEDED(props->GetValue(PKEY_AudioEndpoint_FormFactor, &form)) &&
        (form.vt == VT_UI4 || form.vt == VT_UINT)) {
      row.form = FormFromEndpoint(static_cast<int>(form.uintVal));
    }
    PropVariantClear(&form);

    PROPVARIANT enumerator_name{};
    PropVariantInit(&enumerator_name);
    if (SUCCEEDED(props->GetValue(PKEY_Device_EnumeratorName, &enumerator_name)) &&
        enumerator_name.vt == VT_LPWSTR && enumerator_name.pwszVal != nullptr) {
      row.bluetooth = EnumeratorLooksBluetooth(enumerator_name.pwszVal);
    }
    PropVariantClear(&enumerator_name);

    out.push_back(std::move(row));
  }

  std::sort(out.begin(), out.end(), [](const AudioEndpoint& a, const AudioEndpoint& b) { return a.name < b.name; });

  QueryPerformanceCounter(&t1);
  const double ms = QpcMs(t0, t1);
  static bool logged = false;
  if (!logged) {
    logged = true;
    Log(L"audio", L"enum took %.2f ms n=%u", ms, static_cast<unsigned>(out.size()));
    if (ms > 5.0) {
      Log(L"audio", L"enum exceeded 5 ms");
    }
  }
  return out;
}

bool SetDefaultAudioOutput(const std::wstring& device_id) {
  if (device_id.empty()) {
    return false;
  }
  Microsoft::WRL::ComPtr<IPolicyConfig> policy;
  const HRESULT create_hr =
      CoCreateInstance(kClsidPolicyConfigClient, nullptr, CLSCTX_ALL, kIidPolicyConfig,
                       reinterpret_cast<void**>(policy.ReleaseAndGetAddressOf()));
  static bool logged_qi = false;
  if (!logged_qi) {
    logged_qi = true;
    Log(L"audio", L"IPolicyConfig QI hr=0x%08lx vtable_slot=13", static_cast<unsigned long>(create_hr));
  }
  if (FAILED(create_hr) || !policy) {
    static bool logged_create = false;
    if (!logged_create) {
      logged_create = true;
      Log(L"audio", L"IPolicyConfig create failed hr=0x%08lx", static_cast<unsigned long>(create_hr));
    }
    return false;
  }
  const ERole roles[] = {eConsole, eMultimedia, eCommunications};
  for (const ERole role : roles) {
    const HRESULT hr = policy->SetDefaultEndpoint(device_id.c_str(), role);
    if (FAILED(hr)) {
      static bool logged_set = false;
      if (!logged_set) {
        logged_set = true;
        Log(L"audio", L"SetDefaultEndpoint failed hr=0x%08lx role=%d", static_cast<unsigned long>(hr),
            static_cast<int>(role));
      }
      return false;
    }
  }
  return true;
}

}  // namespace bamti
