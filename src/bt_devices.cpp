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
#include <atomic>
#include <cwchar>
#include <new>
#include <utility>
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

const GUID kBtSvcAudioSink{0x0000110B, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
const GUID kBtSvcHandsfree{0x0000111E, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
const GUID kBtSvcHeadset{0x00001108, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
const GUID kBtSvcAvrcp{0x0000110E, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};
const GUID kBtSvcHid{0x00001124, 0x0000, 0x1000, {0x80, 0x00, 0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB}};

bool IsPreferredBtService(ULONG major, const GUID& guid) {
  if (major == 0x04) {
    return InlineIsEqualGUID(guid, kBtSvcAudioSink) || InlineIsEqualGUID(guid, kBtSvcHandsfree) ||
           InlineIsEqualGUID(guid, kBtSvcHeadset) || InlineIsEqualGUID(guid, kBtSvcAvrcp);
  }
  if (major == 0x05) {
    return InlineIsEqualGUID(guid, kBtSvcHid);
  }
  return true;
}

void FilterPreferredBtServices(ULONG major, std::vector<GUID>* guids) {
  if (guids == nullptr || (major != 0x04 && major != 0x05)) {
    return;
  }
  auto it = std::remove_if(guids->begin(), guids->end(),
                           [major](const GUID& g) { return !IsPreferredBtService(major, g); });
  guids->erase(it, guids->end());
}

std::vector<GUID> FallbackBtServices(ULONG major) {
  if (major == 0x04) {
    return {kBtSvcAudioSink, kBtSvcHandsfree, kBtSvcHeadset, kBtSvcAvrcp};
  }
  if (major == 0x05) {
    return {kBtSvcHid};
  }
  return {};
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

namespace {

constexpr int kBtServiceParallelMax = 4;
constexpr DWORD kBtServiceWaitMs = 20000;

void LogBtServiceResult(int index, int total_n, const GUID& g, DWORD state, double took_ms) {
  Log(L"bt", L"service %d/%d {%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X} state=%lu took %.0f ms", index,
      total_n, static_cast<unsigned long>(g.Data1), g.Data2, g.Data3, g.Data4[0], g.Data4[1], g.Data4[2], g.Data4[3],
      g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7], static_cast<unsigned long>(state), took_ms);
}

struct BtServiceJob {
  BLUETOOTH_DEVICE_INFO di{};
  GUID guid{};
  DWORD flag = 0;
  DWORD state = static_cast<DWORD>(-1);
  double took_ms = 0;
  int index = 0;
  HANDLE done = nullptr;
  std::atomic<int> refs{0};
};

void ReleaseBtServiceJob(BtServiceJob* job) {
  if (job == nullptr) {
    return;
  }
  if (job->refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
    if (job->done != nullptr) {
      CloseHandle(job->done);
    }
    delete job;
  }
}

void RunBtServiceJob(BtServiceJob* job) {
  BLUETOOTH_FIND_RADIO_PARAMS radio_params{};
  radio_params.dwSize = sizeof(radio_params);
  HANDLE radio = nullptr;
  const HBLUETOOTH_RADIO_FIND radio_find = BluetoothFindFirstRadio(&radio_params, &radio);
  if (radio_find == nullptr) {
    DWORD err = GetLastError();
    if (err == ERROR_SUCCESS) {
      err = ERROR_INVALID_HANDLE;
    }
    job->state = err;
    if (job->done != nullptr) {
      SetEvent(job->done);
    }
    return;
  }

  LARGE_INTEGER s0{};
  LARGE_INTEGER s1{};
  QueryPerformanceCounter(&s0);
  job->state = BluetoothSetServiceState(radio, &job->di, &job->guid, job->flag);
  QueryPerformanceCounter(&s1);
  job->took_ms = QpcMs(s0, s1);

  if (radio != nullptr) {
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(radio_find);
  if (job->done != nullptr) {
    SetEvent(job->done);
  }
}

VOID CALLBACK SetBtServiceJobCallback(PTP_CALLBACK_INSTANCE instance, PVOID ctx) {
  (void)instance;
  auto* job = static_cast<BtServiceJob*>(ctx);
  RunBtServiceJob(job);
  ReleaseBtServiceJob(job);
}

int SetBtServicesSerial(HANDLE radio, BLUETOOTH_DEVICE_INFO* di, GUID* guids, int total_n, DWORD flag) {
  int ok_n = 0;
  for (int i = 0; i < total_n; ++i) {
    LARGE_INTEGER s0{};
    LARGE_INTEGER s1{};
    QueryPerformanceCounter(&s0);
    const DWORD serr = BluetoothSetServiceState(radio, di, &guids[i], flag);
    QueryPerformanceCounter(&s1);
    LogBtServiceResult(i + 1, total_n, guids[i], serr, QpcMs(s0, s1));
    if (serr == ERROR_SUCCESS) {
      ++ok_n;
    }
  }
  return ok_n;
}

int SetBtServicesParallel(const BLUETOOTH_DEVICE_INFO& di, const GUID* guids, int total_n, DWORD flag) {
  int ok_n = 0;
  const ULONGLONG deadline = GetTickCount64() + kBtServiceWaitMs;

  for (int start = 0; start < total_n; start += kBtServiceParallelMax) {
    if (GetTickCount64() >= deadline) {
      break;
    }
    const int remain_n = total_n - start;
    const int batch_n = remain_n < kBtServiceParallelMax ? remain_n : kBtServiceParallelMax;

    BtServiceJob* jobs[kBtServiceParallelMax]{};
    HANDLE waits[kBtServiceParallelMax]{};
    int wait_n = 0;

    for (int j = 0; j < batch_n; ++j) {
      const int index = start + j;
      auto* job = new (std::nothrow) BtServiceJob();
      if (job == nullptr) {
        continue;
      }
      job->di = di;
      if (job->di.dwSize == 0) {
        job->di.dwSize = sizeof(job->di);
      }
      job->guid = guids[static_cast<size_t>(index)];
      job->flag = flag;
      job->index = index;
      job->done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
      if (job->done == nullptr) {
        delete job;
        continue;
      }
      job->refs.store(2, std::memory_order_relaxed);
      if (!TrySubmitThreadpoolCallback(&SetBtServiceJobCallback, job, nullptr)) {
        RunBtServiceJob(job);
        LogBtServiceResult(index + 1, total_n, job->guid, job->state, job->took_ms);
        if (job->state == ERROR_SUCCESS) {
          ++ok_n;
        }
        job->refs.store(1, std::memory_order_relaxed);
        ReleaseBtServiceJob(job);
        continue;
      }
      jobs[wait_n] = job;
      waits[wait_n] = job->done;
      ++wait_n;
    }

    if (wait_n <= 0) {
      continue;
    }

    const ULONGLONG now = GetTickCount64();
    const DWORD wait_ms = now >= deadline ? 0 : static_cast<DWORD>(deadline - now);
    WaitForMultipleObjects(static_cast<DWORD>(wait_n), waits, TRUE, wait_ms);
    for (int j = 0; j < wait_n; ++j) {
      if (WaitForSingleObject(waits[j], 0) == WAIT_OBJECT_0) {
        LogBtServiceResult(jobs[j]->index + 1, total_n, jobs[j]->guid, jobs[j]->state, jobs[j]->took_ms);
        if (jobs[j]->state == ERROR_SUCCESS) {
          ++ok_n;
        }
      }
      ReleaseBtServiceJob(jobs[j]);
    }
  }

  return ok_n;
}

}  // namespace

bool SetBtDeviceConnected(const BLUETOOTH_DEVICE_INFO& info, bool connect, std::vector<GUID>* services) {
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

  const ULONG major = (di.ulClassofDevice >> 8) & 0x1F;
  std::vector<GUID> guids;
  const wchar_t* source = L"enumerated";
  int listed_n = 0;

  if (connect && services != nullptr && !services->empty()) {
    guids = *services;
    source = L"remembered";
  } else {
    DWORD n = 0;
    DWORD err = BluetoothEnumerateInstalledServices(radio, &di, &n, nullptr);
    if ((err == ERROR_SUCCESS || err == ERROR_MORE_DATA) && n > 0) {
      constexpr DWORD kBtServiceMax = 32;
      if (n > kBtServiceMax) {
        n = kBtServiceMax;
      }
      guids.resize(n);
      DWORD inout = n;
      err = BluetoothEnumerateInstalledServices(radio, &di, &inout, guids.data());
      if (err == ERROR_SUCCESS || err == ERROR_MORE_DATA) {
        if (inout < n) {
          guids.resize(inout);
        }
      } else {
        guids.clear();
      }
    }
    listed_n = static_cast<int>(guids.size());
    FilterPreferredBtServices(major, &guids);
  }

  if (guids.empty() && connect) {
    guids = FallbackBtServices(major);
    source = L"fallback";
  }

  Log(L"bt", L"set service source=%s class=0x%02lX listed=%d keep=%d", source, static_cast<unsigned long>(major),
      listed_n, static_cast<int>(guids.size()));

  if (guids.empty()) {
    if (radio != nullptr) {
      CloseHandle(radio);
    }
    BluetoothFindRadioClose(radio_find);
    return finish(0, 0, false);
  }

  const DWORD flag = connect ? BLUETOOTH_SERVICE_ENABLE : BLUETOOTH_SERVICE_DISABLE;
  const int total_n = static_cast<int>(guids.size());
  int ok_n = SetBtServicesParallel(di, guids.data(), total_n, flag);
  if (ok_n == 0) {
    Log(L"bt", L"parallel failed ok=0/%d; retrying serially", total_n);
    ok_n = SetBtServicesSerial(radio, &di, guids.data(), total_n, flag);
  }

  if (radio != nullptr) {
    CloseHandle(radio);
  }
  BluetoothFindRadioClose(radio_find);

  const bool ok = ok_n > 0;
  if (!connect && ok && services != nullptr) {
    *services = std::move(guids);
  }
  return finish(ok_n, total_n, ok);
}

}  // namespace bamti
