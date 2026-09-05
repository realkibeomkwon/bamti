#include "bt_devices.hpp"

#include "log.hpp"

#include <initguid.h>
#include <devpkey.h>
#include <roapi.h>
#include <setupapi.h>
#include <windows.devices.radios.h>
#include <wrl/client.h>
#include <wrl/wrappers/corewrappers.h>

#include <algorithm>
#include <cwchar>
#include <vector>

namespace bamti {
namespace {

using ABI::Windows::Devices::Radios::IRadio;
using ABI::Windows::Devices::Radios::IRadioStatics;
using ABI::Windows::Devices::Radios::RadioAccessStatus;
using ABI::Windows::Devices::Radios::RadioKind;
using ABI::Windows::Devices::Radios::RadioKind_Bluetooth;
using ABI::Windows::Devices::Radios::RadioState;
using ABI::Windows::Devices::Radios::RadioState_Off;
using ABI::Windows::Devices::Radios::RadioState_On;
using ABI::Windows::Foundation::IAsyncInfo;
using Microsoft::WRL::ComPtr;
using Microsoft::WRL::Wrappers::HStringReference;
using RadioAccessOp = __FIAsyncOperation_1_Windows__CDevices__CRadios__CRadioAccessStatus;
using RadiosView = __FIVectorView_1_Windows__CDevices__CRadios__CRadio;
using RadiosViewOp = __FIAsyncOperation_1___FIVectorView_1_Windows__CDevices__CRadios__CRadio;

DEFINE_DEVPROPKEY(kBtDeviceAddressKey, 0x2BD67D8B, 0x8BEB, 0x48D5, 0x87, 0xE0, 0x6C, 0xDA, 0x34, 0x28, 0x04, 0x0A, 1);
DEFINE_DEVPROPKEY(kBtBatteryKey, 0x104EA319, 0x6EE2, 0x4701, 0xBD, 0x47, 0x8D, 0xDB, 0xF4, 0x25, 0xBB, 0xE5, 2);

constexpr DWORD kRadioWaitMs = 1500;

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

HRESULT WaitAsyncInfo(IAsyncInfo* info, DWORD timeout_ms) {
  if (info == nullptr) {
    return E_POINTER;
  }
  const ULONGLONG start = GetTickCount64();
  for (;;) {
    ABI::Windows::Foundation::AsyncStatus status = ABI::Windows::Foundation::AsyncStatus::Started;
    const HRESULT hr = info->get_Status(&status);
    if (FAILED(hr)) {
      return hr;
    }
    if (status != ABI::Windows::Foundation::AsyncStatus::Started) {
      if (status == ABI::Windows::Foundation::AsyncStatus::Completed) {
        return S_OK;
      }
      HRESULT err = E_FAIL;
      info->get_ErrorCode(&err);
      return FAILED(err) ? err : E_FAIL;
    }
    if (GetTickCount64() - start > timeout_ms) {
      return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    }
    Sleep(15);
  }
}

HRESULT RadioStatics(IRadioStatics** out) {
  if (out == nullptr) {
    return E_POINTER;
  }
  *out = nullptr;
  return RoGetActivationFactory(HStringReference(RuntimeClass_Windows_Devices_Radios_Radio).Get(),
                                __uuidof(IRadioStatics), reinterpret_cast<void**>(out));
}

HRESULT RequestRadioAccess(IRadioStatics* statics, RadioAccessStatus* access) {
  if (statics == nullptr || access == nullptr) {
    return E_POINTER;
  }
  *access = ABI::Windows::Devices::Radios::RadioAccessStatus_Unspecified;
  ComPtr<RadioAccessOp> op;
  const HRESULT hr = statics->RequestAccessAsync(op.GetAddressOf());
  if (FAILED(hr) || !op) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  ComPtr<IAsyncInfo> info;
  op.As(&info);
  const HRESULT wait = WaitAsyncInfo(info.Get(), kRadioWaitMs);
  if (FAILED(wait)) {
    return wait;
  }
  return op->GetResults(access);
}

HRESULT GetRadiosView(IRadioStatics* statics, RadiosView** view) {
  if (statics == nullptr || view == nullptr) {
    return E_POINTER;
  }
  *view = nullptr;
  ComPtr<RadiosViewOp> op;
  const HRESULT hr = statics->GetRadiosAsync(op.GetAddressOf());
  if (FAILED(hr) || !op) {
    return FAILED(hr) ? hr : E_FAIL;
  }
  ComPtr<IAsyncInfo> info;
  op.As(&info);
  const HRESULT wait = WaitAsyncInfo(info.Get(), kRadioWaitMs);
  if (FAILED(wait)) {
    return wait;
  }
  return op->GetResults(view);
}

bool AdapterPresentClassic() {
  BLUETOOTH_FIND_RADIO_PARAMS params{};
  params.dwSize = sizeof(params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND find = BluetoothFindFirstRadio(&params, &radio);
  if (find == nullptr) {
    return false;
  }
  if (radio != nullptr) {
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(find);
  return true;
}

std::wstring AddrToHex(const BLUETOOTH_ADDRESS& addr) {
  wchar_t buf[13]{};
  swprintf_s(buf, L"%02X%02X%02X%02X%02X%02X", addr.rgBytes[5], addr.rgBytes[4], addr.rgBytes[3], addr.rgBytes[2],
             addr.rgBytes[1], addr.rgBytes[0]);
  return buf;
}

void NormalizeAddr(std::wstring* text) {
  if (text == nullptr) {
    return;
  }
  std::wstring out;
  out.reserve(text->size());
  for (wchar_t ch : *text) {
    if (ch == L':' || ch == L'-' || ch == L' ') {
      continue;
    }
    out.push_back(static_cast<wchar_t>(towupper(ch)));
  }
  *text = std::move(out);
}

void FillBatteries(std::vector<BtDeviceInfo>* devices) {
  if (devices == nullptr || devices->empty()) {
    return;
  }
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);

  const HDEVINFO set = SetupDiGetClassDevsW(nullptr, L"BTHENUM", nullptr, DIGCF_PRESENT | DIGCF_ALLCLASSES);
  if (set == INVALID_HANDLE_VALUE) {
    QueryPerformanceCounter(&t1);
    static bool logged_fail = false;
    if (!logged_fail) {
      logged_fail = true;
      Log(L"cc", L"bt battery SetupDi failed err=%lu took %.2f ms", GetLastError(), QpcMs(t0, t1));
    }
    return;
  }

  int found = 0;
  SP_DEVINFO_DATA info{};
  info.cbSize = sizeof(info);
  for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &info); ++i) {
    DEVPROPTYPE type = 0;
    wchar_t addr[64]{};
    DWORD nbytes = 0;
    if (!SetupDiGetDevicePropertyW(set, &info, &kBtDeviceAddressKey, &type, reinterpret_cast<PBYTE>(addr),
                                   sizeof(addr), &nbytes, 0) ||
        type != DEVPROP_TYPE_STRING) {
      continue;
    }
    std::wstring key = addr;
    NormalizeAddr(&key);
    BYTE pct = 0;
    type = 0;
    nbytes = 0;
    if (!SetupDiGetDevicePropertyW(set, &info, &kBtBatteryKey, &type, &pct, sizeof(pct), &nbytes, 0) ||
        type != DEVPROP_TYPE_BYTE) {
      continue;
    }
    for (BtDeviceInfo& d : *devices) {
      if (d.address == key) {
        d.battery = static_cast<int>(pct);
        ++found;
      }
    }
  }
  SetupDiDestroyDeviceInfoList(set);
  QueryPerformanceCounter(&t1);
  const double ms = QpcMs(t0, t1);
  static bool logged = false;
  if (!logged) {
    logged = true;
    Log(L"cc", L"bt battery lookup took %.2f ms found=%d", ms, found);
    if (ms > 10.0) {
      Log(L"cc", L"bt battery lookup exceeded 10 ms");
    }
    if (found == 0) {
      Log(L"cc", L"bt battery key produced no values; hiding percents");
    }
  }
}

}  // namespace

const wchar_t* BtClassGlyph(ULONG class_of_device) {
  const ULONG major = (class_of_device >> 8) & 0x1F;
  const ULONG minor = (class_of_device >> 2) & 0x3F;
  if (major == 0x04) {
    return L"\xE7F6";
  }
  if (major == 0x05) {
    if ((minor & 0x10) != 0) {
      return L"\xE765";
    }
    if ((minor & 0x20) != 0) {
      return L"\xE962";
    }
  }
  return L"\xE702";
}

BtRadioState QueryBtRadio() {
  BtRadioState state;
  state.present = AdapterPresentClassic();

  ComPtr<IRadioStatics> statics;
  if (FAILED(RadioStatics(statics.GetAddressOf())) || !statics) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log(L"cc", L"bt radio WinRT factory failed; toggle will open settings");
    }
    return state;
  }

  RadioAccessStatus access = ABI::Windows::Devices::Radios::RadioAccessStatus_Unspecified;
  if (SUCCEEDED(RequestRadioAccess(statics.Get(), &access))) {
    state.can_toggle = access == ABI::Windows::Devices::Radios::RadioAccessStatus_Allowed;
  }

  ComPtr<RadiosView> radios;
  if (FAILED(GetRadiosView(statics.Get(), radios.GetAddressOf())) || !radios) {
    return state;
  }
  unsigned n = 0;
  radios->get_Size(&n);
  for (unsigned i = 0; i < n; ++i) {
    ComPtr<IRadio> radio;
    if (FAILED(radios->GetAt(i, radio.GetAddressOf())) || !radio) {
      continue;
    }
    RadioKind kind = ABI::Windows::Devices::Radios::RadioKind_Other;
    if (FAILED(radio->get_Kind(&kind)) || kind != RadioKind_Bluetooth) {
      continue;
    }
    state.present = true;
    RadioState rs = ABI::Windows::Devices::Radios::RadioState_Unknown;
    if (SUCCEEDED(radio->get_State(&rs))) {
      state.on = rs == RadioState_On;
    }
    break;
  }
  return state;
}

bool SetBtRadio(bool on) {
  ComPtr<IRadioStatics> statics;
  if (FAILED(RadioStatics(statics.GetAddressOf())) || !statics) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log(L"cc", L"SetBtRadio factory failed");
    }
    return false;
  }
  RadioAccessStatus access = ABI::Windows::Devices::Radios::RadioAccessStatus_Unspecified;
  if (FAILED(RequestRadioAccess(statics.Get(), &access)) ||
      access != ABI::Windows::Devices::Radios::RadioAccessStatus_Allowed) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log(L"cc", L"SetBtRadio access denied status=%d", static_cast<int>(access));
    }
    return false;
  }
  ComPtr<RadiosView> radios;
  if (FAILED(GetRadiosView(statics.Get(), radios.GetAddressOf())) || !radios) {
    return false;
  }
  unsigned n = 0;
  radios->get_Size(&n);
  bool any = false;
  for (unsigned i = 0; i < n; ++i) {
    ComPtr<IRadio> radio;
    if (FAILED(radios->GetAt(i, radio.GetAddressOf())) || !radio) {
      continue;
    }
    RadioKind kind = ABI::Windows::Devices::Radios::RadioKind_Other;
    if (FAILED(radio->get_Kind(&kind)) || kind != RadioKind_Bluetooth) {
      continue;
    }
    ComPtr<RadioAccessOp> op;
    const HRESULT hr = radio->SetStateAsync(on ? RadioState_On : RadioState_Off, op.GetAddressOf());
    if (FAILED(hr) || !op) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        Log(L"cc", L"SetBtRadio SetStateAsync failed hr=0x%08lx", static_cast<unsigned long>(hr));
      }
      return false;
    }
    ComPtr<IAsyncInfo> info;
    op.As(&info);
    const HRESULT wait = WaitAsyncInfo(info.Get(), kRadioWaitMs);
    if (FAILED(wait)) {
      static bool logged = false;
      if (!logged) {
        logged = true;
        Log(L"cc", L"SetBtRadio wait failed hr=0x%08lx", static_cast<unsigned long>(wait));
      }
      return false;
    }
    any = true;
    break;
  }
  if (any) {
    Log(L"cc", L"SetBtRadio requested on=%d", on ? 1 : 0);
  }
  return any;
}

std::vector<BtDeviceInfo> EnumBtDevices() {
  std::vector<BtDeviceInfo> out;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);

  BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
  radio_params.dwSize = sizeof(radio_params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio);
  if (radio_find == nullptr) {
    QueryPerformanceCounter(&t1);
    static bool logged_empty = false;
    if (!logged_empty) {
      logged_empty = true;
      Log(L"cc", L"bt enum no radio took %.2f ms", QpcMs(t0, t1));
    }
    return out;
  }

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
      BtDeviceInfo row;
      row.raw = info;
      row.name = info.szName;
      row.connected = info.fConnected != FALSE;
      row.paired = info.fAuthenticated != FALSE || info.fRemembered != FALSE;
      row.address = AddrToHex(info.Address);
      if (!row.name.empty()) {
        out.push_back(std::move(row));
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

  const auto mid =
      std::stable_partition(out.begin(), out.end(), [](const BtDeviceInfo& d) { return d.connected; });
  std::sort(out.begin(), mid, [](const BtDeviceInfo& a, const BtDeviceInfo& b) { return a.name < b.name; });
  std::sort(mid, out.end(), [](const BtDeviceInfo& a, const BtDeviceInfo& b) { return a.name < b.name; });

  if (out.size() > 8) {
    out.resize(8);
  }
  FillBatteries(&out);

  QueryPerformanceCounter(&t1);
  const double ms = QpcMs(t0, t1);
  static bool logged = false;
  if (!logged) {
    logged = true;
    Log(L"cc", L"bt enum took %.2f ms n=%u", ms, static_cast<unsigned>(out.size()));
    if (ms > 10.0) {
      Log(L"cc", L"bt enum exceeded 10 ms");
    }
  }
  return out;
}

std::vector<BtDeviceInfo> ScanBtDevices() {
  std::vector<BtDeviceInfo> out;
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);

  BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
  radio_params.dwSize = sizeof(radio_params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio);
  if (radio_find == nullptr) {
    QueryPerformanceCounter(&t1);
    Log(L"bt", L"scan took %.0f ms n=%d", QpcMs(t0, t1), 0);
    return out;
  }

  BLUETOOTH_DEVICE_SEARCH_PARAMS search{};
  search.dwSize = sizeof(search);
  search.fReturnAuthenticated = FALSE;
  search.fReturnRemembered = FALSE;
  search.fReturnUnknown = TRUE;
  search.fReturnConnected = FALSE;
  search.fIssueInquiry = TRUE;
  search.cTimeoutMultiplier = 4;  // 1당 1.28초이므로 약 5.1초
  search.hRadio = radio;
  BLUETOOTH_DEVICE_INFO info{};
  info.dwSize = sizeof(info);
  const HBLUETOOTH_DEVICE_FIND find = BluetoothFindFirstDevice(&search, &info);
  if (find != nullptr) {
    do {
      BtDeviceInfo row;
      row.raw = info;
      row.name = info.szName;
      row.connected = info.fConnected != FALSE;
      row.paired = info.fAuthenticated != FALSE || info.fRemembered != FALSE;
      row.address = AddrToHex(info.Address);
      out.push_back(std::move(row));
      info = {};
      info.dwSize = sizeof(info);
    } while (BluetoothFindNextDevice(find, &info));
    BluetoothFindDeviceClose(find);
  }
  if (radio != nullptr) {
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(radio_find);

  std::stable_sort(out.begin(), out.end(), [](const BtDeviceInfo& a, const BtDeviceInfo& b) {
    const bool a_empty = a.name.empty();
    const bool b_empty = b.name.empty();
    if (a_empty != b_empty) {
      return !a_empty;
    }
    return a.name < b.name;
  });

  constexpr int kBtScanMax = 8;
  if (out.size() > static_cast<size_t>(kBtScanMax)) {
    out.resize(static_cast<size_t>(kBtScanMax));
  }

  QueryPerformanceCounter(&t1);
  Log(L"bt", L"scan took %.0f ms n=%d", QpcMs(t0, t1), static_cast<int>(out.size()));
  return out;
}

bool SetBtDeviceConnected(const BLUETOOTH_DEVICE_INFO& info, bool connect) {
  LARGE_INTEGER t0{};
  LARGE_INTEGER t1{};
  QueryPerformanceCounter(&t0);

  const wchar_t* name = info.szName[0] != L'\0' ? info.szName : L"";
  auto finish = [&](int ok_n, int total_n, bool ok) {
    QueryPerformanceCounter(&t1);
    Log(L"bt", L"set service state %s connect=%d ok=%d/%d took %.0f ms", name, connect ? 1 : 0, ok_n, total_n,
        QpcMs(t0, t1));
    return ok;
  };

  BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
  radio_params.dwSize = sizeof(radio_params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio);
  if (radio_find == nullptr) {
    return finish(0, 0, false);
  }

  BLUETOOTH_DEVICE_INFO di = info;
  if (di.dwSize == 0) {
    di.dwSize = sizeof(di);
  }

  DWORD n = 0;
  DWORD err = BluetoothEnumerateInstalledServices(radio, &di, &n, nullptr);
  if ((err != ERROR_SUCCESS && err != ERROR_MORE_DATA) || n == 0) {
    if (radio != nullptr) {
      CloseHandle(radio);
    }
    BluetoothFindRadioClose(radio_find);
    return finish(0, 0, false);
  }

  constexpr DWORD kBtServiceMax = 32;
  if (n > kBtServiceMax) {
    n = kBtServiceMax;
  }
  std::vector<GUID> guids(n);
  DWORD inout = n;
  err = BluetoothEnumerateInstalledServices(radio, &di, &inout, guids.data());
  if (err != ERROR_SUCCESS && err != ERROR_MORE_DATA) {
    if (radio != nullptr) {
      CloseHandle(radio);
    }
    BluetoothFindRadioClose(radio_find);
    return finish(0, static_cast<int>(n), false);
  }
  if (inout < n) {
    n = inout;
  }

  const DWORD flag = connect ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE;
  int ok_n = 0;
  for (DWORD i = 0; i < n; ++i) {
    if (BluetoothSetServiceState(radio, &di, &guids[i], flag) == ERROR_SUCCESS) {
      ++ok_n;
    }
  }

  if (radio != nullptr) {
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(radio_find);
  return finish(ok_n, static_cast<int>(n), ok_n > 0);
}

}  // namespace bamti
