#include <windows.h>
#include <oleauto.h>
#include <wbemidl.h>
#include <wrl/client.h>

#include "widgets/brightness.hpp"

namespace bamti {
namespace {

class Bstr {
 public:
  explicit Bstr(const wchar_t* text) : p_(SysAllocString(text)) {}
  ~Bstr() { SysFreeString(p_); }
  Bstr(const Bstr&) = delete;
  Bstr& operator=(const Bstr&) = delete;
  operator BSTR() const { return p_; }

 private:
  BSTR p_ = nullptr;
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

}  // namespace

BrightnessSample ProbeWmiBrightness() {
  BrightnessSample out;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);
  Microsoft::WRL::ComPtr<IWbemLocator> locator;
  if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))) || !locator) {
    QueryPerformanceCounter(&t1);
    out.ms = QpcMs(t0, t1);
    return out;
  }
  Microsoft::WRL::ComPtr<IWbemServices> svc;
  if (FAILED(locator->ConnectServer(Bstr(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                                    svc.GetAddressOf())) ||
      !svc) {
    QueryPerformanceCounter(&t1);
    out.ms = QpcMs(t0, t1);
    return out;
  }
  CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  Microsoft::WRL::ComPtr<IEnumWbemClassObject> en;
  if (FAILED(svc->ExecQuery(Bstr(L"WQL"), Bstr(L"SELECT CurrentBrightness FROM WmiMonitorBrightness"),
                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.GetAddressOf())) ||
      !en) {
    QueryPerformanceCounter(&t1);
    out.ms = QpcMs(t0, t1);
    return out;
  }
  Microsoft::WRL::ComPtr<IWbemClassObject> obj;
  ULONG got = 0;
  while (SUCCEEDED(en->Next(2000, 1, obj.ReleaseAndGetAddressOf(), &got)) && got == 1 && obj) {
    VARIANT v;
    VariantInit(&v);
    if (SUCCEEDED(obj->Get(L"CurrentBrightness", 0, &v, nullptr, nullptr))) {
      if (v.vt == VT_UI1) {
        out.ok = true;
        out.value = v.bVal;
        out.backend = BrightnessBackend::kWmi;
      } else if (v.vt == VT_I4) {
        out.ok = true;
        out.value = static_cast<unsigned>(v.lVal);
        out.backend = BrightnessBackend::kWmi;
      }
      VariantClear(&v);
      if (out.ok) {
        break;
      }
    }
    VariantClear(&v);
  }
  QueryPerformanceCounter(&t1);
  out.ms = QpcMs(t0, t1);
  return out;
}

bool SetWmiBrightness(unsigned percent) {
  if (percent > 100) {
    percent = 100;
  }
  Microsoft::WRL::ComPtr<IWbemLocator> locator;
  if (FAILED(CoCreateInstance(CLSID_WbemLocator, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&locator))) || !locator) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWbemServices> svc;
  if (FAILED(locator->ConnectServer(Bstr(L"ROOT\\WMI"), nullptr, nullptr, nullptr, 0, nullptr, nullptr,
                                    svc.GetAddressOf())) ||
      !svc) {
    return false;
  }
  CoSetProxyBlanket(svc.Get(), RPC_C_AUTHN_WINNT, RPC_C_AUTHZ_NONE, nullptr, RPC_C_AUTHN_LEVEL_CALL,
                    RPC_C_IMP_LEVEL_IMPERSONATE, nullptr, EOAC_NONE);
  Microsoft::WRL::ComPtr<IEnumWbemClassObject> en;
  if (FAILED(svc->ExecQuery(Bstr(L"WQL"), Bstr(L"SELECT * FROM WmiMonitorBrightnessMethods"),
                            WBEM_FLAG_FORWARD_ONLY | WBEM_FLAG_RETURN_IMMEDIATELY, nullptr, en.GetAddressOf())) ||
      !en) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWbemClassObject> obj;
  ULONG got = 0;
  if (FAILED(en->Next(2000, 1, obj.GetAddressOf(), &got)) || got != 1 || !obj) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWbemClassObject> cls;
  if (FAILED(svc->GetObject(Bstr(L"WmiMonitorBrightnessMethods"), 0, nullptr, cls.GetAddressOf(), nullptr)) || !cls) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWbemClassObject> in_sig;
  if (FAILED(cls->GetMethod(L"WmiSetBrightness", 0, in_sig.GetAddressOf(), nullptr)) || !in_sig) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWbemClassObject> in_args;
  if (FAILED(in_sig->SpawnInstance(0, in_args.GetAddressOf())) || !in_args) {
    return false;
  }
  VARIANT timeout;
  VariantInit(&timeout);
  timeout.vt = VT_UI4;
  timeout.ulVal = 1;
  in_args->Put(L"Timeout", 0, &timeout, 0);
  VARIANT bright;
  VariantInit(&bright);
  bright.vt = VT_UI1;
  bright.bVal = static_cast<BYTE>(percent);
  in_args->Put(L"Brightness", 0, &bright, 0);
  VARIANT path;
  VariantInit(&path);
  const bool have_path = SUCCEEDED(obj->Get(L"__PATH", 0, &path, nullptr, nullptr));
  Bstr fallback(L"WmiMonitorBrightnessMethods");
  const HRESULT hr = svc->ExecMethod(have_path && path.vt == VT_BSTR ? path.bstrVal : static_cast<BSTR>(fallback),
                                     Bstr(L"WmiSetBrightness"), 0, nullptr, in_args.Get(), nullptr, nullptr);
  VariantClear(&timeout);
  VariantClear(&bright);
  VariantClear(&path);
  return SUCCEEDED(hr);
}

}  // namespace bamti
