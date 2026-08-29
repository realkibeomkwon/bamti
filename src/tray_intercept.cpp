#include "tray_intercept.hpp"

#include "icon_cache.hpp"
#include "log.hpp"
#include "status_item.hpp"

#include <objbase.h>
#include <objidl.h>
#include <shellapi.h>
#include <wincodec.h>
#include <windows.h>
#include <wrl/client.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace bamti {
namespace {

constexpr wchar_t kSpyClass[] = L"Shell_TrayWnd";
constexpr UINT kStopMsg = WM_APP + 40;
constexpr UINT kPrioTimerId = 1;
constexpr UINT kPrioTimerMs = 1000;
constexpr UINT kForwardTimeoutMs = 1000;
constexpr size_t kPendingMax = 64;
constexpr DWORD kCopyDataMax = 65536;

struct PendingMsg {
  UINT msg = 0;
  WPARAM wp = 0;
  LPARAM lp = 0;
  ULONG_PTR dw_data = 0;
  std::vector<uint8_t> bytes;
  bool copy_data = false;
};

constexpr long kParseFailMax = 10;
constexpr int kIconPx = 24;

#pragma pack(push, 1)
struct TrayNotifyIconData {
  uint32_t callback_size;
  uint32_t window_handle;
  uint32_t uid;
  uint32_t flags;
  uint32_t callback_message;
  uint32_t icon_handle;
  wchar_t tooltip[128];
  uint32_t state;
  uint32_t state_mask;
  wchar_t size_info[256];
  uint32_t anonymous;
  wchar_t info_title[64];
  uint32_t info_flags;
  GUID guid_item;
  uint32_t balloon_icon_handle;
};

struct ShellTrayMessage {
  int32_t magic_number;
  uint32_t message_type;
  TrayNotifyIconData icon_data;
  uint32_t version;
};

struct NotifyIconIdentifier {
  int32_t magic_number;
  int32_t message;
  int32_t callback_size;
  int32_t padding;
  uint32_t window_handle;
  uint32_t uid;
  GUID guid_item;
};
#pragma pack(pop)

struct StoredIcon {
  TrayIconInfo info;
  uint32_t icon_handle = 0;
  HWND owner = nullptr;
};

bool OwnProcess(HWND hwnd) {
  DWORD pid = 0;
  GetWindowThreadProcessId(hwnd, &pid);
  return pid == GetCurrentProcessId();
}

HWND FindRealTray(HWND spy) {
  HWND real = FindWindowW(kSpyClass, nullptr);
  while (real != nullptr && (real == spy || OwnProcess(real))) {
    real = FindWindowExW(nullptr, real, kSpyClass, nullptr);
  }
  return real;
}

bool GuidEmpty(const GUID& g) {
  const auto* p = reinterpret_cast<const uint8_t*>(&g);
  for (size_t i = 0; i < sizeof(GUID); ++i) {
    if (p[i] != 0) {
      return false;
    }
  }
  return true;
}

bool KnownNotifySize(uint32_t n) {
  if (n == static_cast<uint32_t>(NOTIFYICONDATAW_V1_SIZE) || n == static_cast<uint32_t>(NOTIFYICONDATAW_V2_SIZE) ||
      n == static_cast<uint32_t>(NOTIFYICONDATAW_V3_SIZE) || n == static_cast<uint32_t>(sizeof(NOTIFYICONDATAW))) {
    return true;
  }
  // Win11 페이로드에서 관측되는 변형. 하한은 V1, 상한은 비정상적으로 큰 값 배제.
  return n >= static_cast<uint32_t>(NOTIFYICONDATAW_V1_SIZE) && n <= 2048;
}

std::wstring TipFrom(const wchar_t* tip, size_t max_chars) {
  size_t n = 0;
  while (n < max_chars && tip[n] != 0) {
    ++n;
  }
  return std::wstring(tip, tip + n);
}

uint64_t ItemKey(const TrayNotifyIconData& data) {
  if (!GuidEmpty(data.guid_item)) {
    return Fnv1a64(reinterpret_cast<const uint8_t*>(&data.guid_item), sizeof(GUID));
  }
  uint64_t hash = Fnv1a64(reinterpret_cast<const uint8_t*>(&data.window_handle), sizeof(data.window_handle));
  return Fnv1a64(reinterpret_cast<const uint8_t*>(&data.uid), sizeof(data.uid), hash);
}

bool EncodePng(const BgraImage& image, std::vector<uint8_t>* out) {
  if (out == nullptr || image.width <= 0 || image.height <= 0 || image.pixels.empty()) {
    return false;
  }
  IWICImagingFactory* wic = WicFactory();
  if (wic == nullptr) {
    return false;
  }
  Microsoft::WRL::ComPtr<IStream> stream;
  if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, stream.GetAddressOf()))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapEncoder> encoder;
  if (FAILED(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, encoder.GetAddressOf()))) {
    return false;
  }
  if (FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache))) {
    return false;
  }
  Microsoft::WRL::ComPtr<IWICBitmapFrameEncode> frame;
  Microsoft::WRL::ComPtr<IPropertyBag2> props;
  if (FAILED(encoder->CreateNewFrame(frame.GetAddressOf(), props.GetAddressOf()))) {
    return false;
  }
  if (FAILED(frame->Initialize(props.Get()))) {
    return false;
  }
  if (FAILED(frame->SetSize(static_cast<UINT>(image.width), static_cast<UINT>(image.height)))) {
    return false;
  }
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  if (FAILED(frame->SetPixelFormat(&format))) {
    return false;
  }
  const UINT stride = static_cast<UINT>(image.width) * 4;
  const UINT bytes = stride * static_cast<UINT>(image.height);
  if (FAILED(frame->WritePixels(static_cast<UINT>(image.height), stride, bytes,
                                 const_cast<BYTE*>(image.pixels.data())))) {
    return false;
  }
  if (FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
    return false;
  }
  HGLOBAL hg = nullptr;
  if (FAILED(GetHGlobalFromStream(stream.Get(), &hg)) || hg == nullptr) {
    return false;
  }
  const SIZE_T n = GlobalSize(hg);
  void* p = GlobalLock(hg);
  if (p == nullptr) {
    return false;
  }
  out->assign(static_cast<const uint8_t*>(p), static_cast<const uint8_t*>(p) + n);
  GlobalUnlock(hg);
  return !out->empty() && out->size() <= kStatusIconPngMaxBytes;
}

bool IconToPng(HICON icon, std::vector<uint8_t>* out) {
  if (icon == nullptr || out == nullptr) {
    return false;
  }
  BITMAPINFO bmi{};
  bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
  bmi.bmiHeader.biWidth = kIconPx;
  bmi.bmiHeader.biHeight = -kIconPx;
  bmi.bmiHeader.biPlanes = 1;
  bmi.bmiHeader.biBitCount = 32;
  bmi.bmiHeader.biCompression = BI_RGB;
  void* bits = nullptr;
  HBITMAP bmp = CreateDIBSection(nullptr, &bmi, DIB_RGB_COLORS, &bits, nullptr, 0);
  if (bmp == nullptr) {
    return false;
  }
  const HDC dc = CreateCompatibleDC(nullptr);
  const HGDIOBJ old = SelectObject(dc, bmp);
  DrawIconEx(dc, 0, 0, icon, kIconPx, kIconPx, 0, nullptr, DI_NORMAL);
  SelectObject(dc, old);
  DeleteDC(dc);
  BgraImage image;
  const bool ok = BitmapToBgra(bmp, image);
  DeleteObject(bmp);
  if (!ok) {
    return false;
  }
  return EncodePng(image, out);
}

class TrayBackendIntercept final : public TrayBackend {
 public:
  const char* Name() const override { return "intercept"; }

  ~TrayBackendIntercept() { StopSpy(); }

  bool Probe() override { return StartSpy(); }

  bool Enumerate(std::vector<TrayIconInfo>* out) override {
    if (out == nullptr) {
      return false;
    }
    std::lock_guard lock(mu_);
    out->clear();
    out->reserve(items_.size());
    std::vector<size_t> dead;
    for (size_t i = 0; i < items_.size(); ++i) {
      if (items_[i].owner != nullptr && IsWindow(items_[i].owner) == FALSE) {
        dead.push_back(i);
        continue;
      }
      out->push_back(items_[i].info);
    }
    for (size_t i = dead.size(); i > 0; --i) {
      items_.erase(items_.begin() + static_cast<std::ptrdiff_t>(dead[i - 1]));
    }
    return true;
  }

  bool Invoke(const TrayIconInfo&) override { return false; }

  bool Invoke(const TrayIconInfo& icon, bool right) override {
    HWND owner = nullptr;
    UINT uid = 0;
    UINT callback = 0;
    UINT version = 0;
    uint64_t key = icon.key;
    {
      std::lock_guard lock(mu_);
      for (const StoredIcon& one : items_) {
        if (one.info.key == key) {
          owner = one.info.owner;
          uid = one.info.uid;
          callback = one.info.callback_message;
          version = one.info.version;
          break;
        }
      }
    }
    if (owner == nullptr || callback == 0 || IsWindow(owner) == FALSE) {
      return false;
    }
    DWORD pid = 0;
    GetWindowThreadProcessId(owner, &pid);
    if (pid != 0) {
      AllowSetForegroundWindow(pid);
    }
    POINT pt{};
    RECT rc{};
    bool have_rect = false;
    {
      std::lock_guard lock(mu_);
      if (rect_lookup_) {
        have_rect = rect_lookup_(key, &rc);
      }
    }
    if (have_rect) {
      pt.x = (rc.left + rc.right) / 2;
      pt.y = (rc.top + rc.bottom) / 2;
    } else {
      GetCursorPos(&pt);
    }
    const UINT down = right ? WM_RBUTTONDOWN : WM_LBUTTONDOWN;
    const UINT up = right ? WM_RBUTTONUP : WM_LBUTTONUP;
    if (version >= 4) {
      const WPARAM wp = MAKEWPARAM(static_cast<UINT>(pt.x), static_cast<UINT>(pt.y));
      PostMessageW(owner, callback, wp, MAKELPARAM(down, uid));
      PostMessageW(owner, callback, wp, MAKELPARAM(up, uid));
      if (right) {
        PostMessageW(owner, callback, wp, MAKELPARAM(WM_CONTEXTMENU, uid));
      }
    } else {
      PostMessageW(owner, callback, uid, down);
      PostMessageW(owner, callback, uid, up);
      if (right) {
        PostMessageW(owner, callback, uid, WM_CONTEXTMENU);
      }
    }
    return true;
  }

  bool ForwardsContextMenu() const override { return true; }

  void SetRectLookup(std::function<bool(uint64_t key, RECT* screen)> lookup) override {
    std::lock_guard lock(mu_);
    rect_lookup_ = std::move(lookup);
  }

  void Reset() override {}

  void SetChangeSink(std::function<void()> on_change) override {
    std::lock_guard lock(mu_);
    on_change_ = std::move(on_change);
  }

 private:
  bool StartSpy() {
    {
      std::lock_guard lock(mu_);
      if (spy_ != nullptr && IsWindow(spy_)) {
        return true;
      }
    }
    StopSpy();
    ready_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ready_ == nullptr) {
      Log(L"tray", L"intercept ready event failed err=%lu", GetLastError());
      return false;
    }
    thread_ = std::thread([this] { SpyLoop(); });
    WaitForSingleObject(ready_, 2000);
    HWND spy = nullptr;
    {
      std::lock_guard lock(mu_);
      spy = spy_;
    }
    if (spy == nullptr || !IsWindow(spy)) {
      Log(L"tray", L"intercept spy create failed");
      StopSpy();
      return false;
    }
    return true;
  }

  void StopSpy() {
    HWND spy = nullptr;
    DWORD tid = 0;
    {
      std::lock_guard lock(mu_);
      spy = spy_;
      tid = thread_id_;
    }
    bool posted = false;
    if (spy != nullptr && IsWindow(spy)) {
      posted = PostMessageW(spy, kStopMsg, 0, 0) != FALSE;
    }
    if (!posted && tid != 0) {
      PostThreadMessageW(tid, WM_QUIT, 0, 0);
    }
    if (thread_.joinable()) {
      thread_.join();
    }
    if (ready_ != nullptr) {
      CloseHandle(ready_);
      ready_ = nullptr;
    }
    std::lock_guard lock(mu_);
    spy_ = nullptr;
    thread_id_ = 0;
  }

  void SpyLoop() {
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    {
      std::lock_guard lock(mu_);
      thread_id_ = GetCurrentThreadId();
    }
    MSG pump{};
    PeekMessageW(&pump, nullptr, WM_USER, WM_USER, PM_NOREMOVE);

    WNDCLASSW wc{};
    wc.lpszClassName = kSpyClass;
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = SpyProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    if (RegisterClassW(&wc) == 0) {
      const DWORD err = GetLastError();
      if (err != ERROR_CLASS_ALREADY_EXISTS) {
        Log(L"tray", L"intercept RegisterClass err=%lu", err);
        if (ready_ != nullptr) {
          SetEvent(ready_);
        }
        if (SUCCEEDED(co)) {
          CoUninitialize();
        }
        return;
      }
    }

    HWND spy = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_TOPMOST, kSpyClass, kSpyClass, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, nullptr, nullptr,
                                wc.hInstance, this);
    if (spy == nullptr) {
      Log(L"tray", L"intercept CreateWindow err=%lu", GetLastError());
      if (ready_ != nullptr) {
        SetEvent(ready_);
      }
      UnregisterClassW(kSpyClass, wc.hInstance);
      if (SUCCEEDED(co)) {
        CoUninitialize();
      }
      return;
    }

    ChangeWindowMessageFilterEx(spy, WM_COPYDATA, MSGFLT_ALLOW, nullptr);
    {
      std::lock_guard lock(mu_);
      spy_ = spy;
    }
    SetTimer(spy, kPrioTimerId, kPrioTimerMs, nullptr);
    SetWindowPos(spy, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    if (ready_ != nullptr) {
      SetEvent(ready_);
    }
    Log(L"tray", L"intercept pass-through spy=0x%llX explorer=0x%llX timer_ms=%u sizeof_msg=%zu",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(spy)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(FindRealTray(spy))), kPrioTimerMs,
        sizeof(ShellTrayMessage));

    const UINT created = RegisterWindowMessageW(L"TaskbarCreated");
    if (created != 0) {
      SendNotifyMessageW(HWND_BROADCAST, created, 0, 0);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
      TranslateMessage(&msg);
      DispatchMessageW(&msg);
    }

    KillTimer(spy, kPrioTimerId);
    {
      std::lock_guard lock(mu_);
      spy_ = nullptr;
    }
    if (IsWindow(spy)) {
      DestroyWindow(spy);
    }
    FlushPending();
    if (created != 0) {
      SendNotifyMessageW(HWND_BROADCAST, created, 0, 0);
    }
    UnregisterClassW(kSpyClass, wc.hInstance);
    Log(L"tray", L"intercept spy stop forwarded=%u fail=%u pending=%zu parse_fail=%ld", forward_ok_, forward_fail_,
        pending_.size(), parse_fail_);
    if (SUCCEEDED(co)) {
      CoUninitialize();
    }
  }

  static LRESULT CALLBACK SpyProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    TrayBackendIntercept* self = nullptr;
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lp);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      self = static_cast<TrayBackendIntercept*>(cs->lpCreateParams);
    } else {
      self = reinterpret_cast<TrayBackendIntercept*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    }
    if (self == nullptr) {
      return DefWindowProcW(hwnd, msg, wp, lp);
    }
    return self->Handle(hwnd, msg, wp, lp);
  }

  LRESULT Handle(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kStopMsg) {
      KillTimer(hwnd, kPrioTimerId);
      DestroyWindow(hwnd);
      PostQuitMessage(0);
      return 0;
    }
    if (msg == WM_TIMER && wp == kPrioTimerId) {
      KeepPriority(hwnd);
      FlushPending();
      return 0;
    }
    if (msg == WM_COPYDATA) {
      auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
      if (cds != nullptr && cds->dwData == 3) {
        return AnswerGetRect(cds);
      }
    }
    if (msg == WM_COPYDATA || msg == WM_ACTIVATEAPP || msg == WM_COMMAND) {
      ForwardOrQueue(hwnd, msg, wp, lp);
      return 1;
    }
    if (msg >= WM_USER) {
      ForwardOrQueue(hwnd, msg, wp, lp);
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  void KeepPriority(HWND spy) {
    const HWND first = FindWindowW(kSpyClass, nullptr);
    if (first == spy) {
      return;
    }
    ++z_loss_;
    Log(L"tray", L"intercept z-order lost first=0x%llX spy=0x%llX count=%u",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(first)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(spy)), z_loss_);
    SetWindowPos(spy, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
    const HWND after = FindWindowW(kSpyClass, nullptr);
    if (after == spy) {
      Log(L"tray", L"intercept z-order restored");
    }
  }

  void ForwardOrQueue(HWND spy, UINT msg, WPARAM wp, LPARAM lp) {
    ULONG_PTR dw = 0;
    DWORD cb = 0;
    if (msg == WM_COPYDATA && lp != 0) {
      auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
      dw = cds->dwData;
      cb = cds->cbData;
      ParseCopyData(cds);
    }
    if (TryForward(spy, msg, wp, lp)) {
      ++forward_ok_;
      if (msg == WM_COPYDATA && copy_log_ < 8) {
        ++copy_log_;
        Log(L"tray", L"intercept copydata dw=%llu cb=%lu ok=%u", static_cast<unsigned long long>(dw), cb, forward_ok_);
      }
      return;
    }
    ++forward_fail_;
    Enqueue(msg, wp, lp);
    Log(L"tray", L"intercept forward fail msg=0x%04X dw=%llu pending=%zu", msg, static_cast<unsigned long long>(dw),
        pending_.size());
  }

  bool TryForward(HWND spy, UINT msg, WPARAM wp, LPARAM lp) {
    const HWND real = FindRealTray(spy);
    if (real == nullptr) {
      return false;
    }
    if (msg >= WM_USER) {
      return PostMessageW(real, msg, wp, lp) != FALSE;
    }
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(real, msg, wp, lp, SMTO_ABORTIFHUNG, kForwardTimeoutMs, &result) != 0;
  }

  bool TryForwardQueued(HWND spy, const PendingMsg& one) {
    const HWND real = FindRealTray(spy);
    if (real == nullptr) {
      return false;
    }
    if (one.msg >= WM_USER) {
      return PostMessageW(real, one.msg, one.wp, one.lp) != FALSE;
    }
    if (one.copy_data) {
      COPYDATASTRUCT cds{};
      cds.dwData = one.dw_data;
      cds.cbData = static_cast<DWORD>(one.bytes.size());
      cds.lpData = one.bytes.empty() ? nullptr : const_cast<uint8_t*>(one.bytes.data());
      DWORD_PTR result = 0;
      return SendMessageTimeoutW(real, WM_COPYDATA, one.wp, reinterpret_cast<LPARAM>(&cds), SMTO_ABORTIFHUNG,
                                 kForwardTimeoutMs, &result) != 0;
    }
    DWORD_PTR result = 0;
    return SendMessageTimeoutW(real, one.msg, one.wp, one.lp, SMTO_ABORTIFHUNG, kForwardTimeoutMs, &result) != 0;
  }

  void Enqueue(UINT msg, WPARAM wp, LPARAM lp) {
    PendingMsg one;
    one.msg = msg;
    one.wp = wp;
    if (msg == WM_COPYDATA) {
      auto* cds = reinterpret_cast<COPYDATASTRUCT*>(lp);
      if (cds == nullptr) {
        return;
      }
      one.copy_data = true;
      one.dw_data = cds->dwData;
      if (cds->lpData != nullptr && cds->cbData > 0 && cds->cbData <= kCopyDataMax) {
        const auto* bytes = static_cast<const uint8_t*>(cds->lpData);
        one.bytes.assign(bytes, bytes + cds->cbData);
      } else if (cds->cbData > kCopyDataMax) {
        Log(L"tray", L"intercept copydata too large cb=%lu", cds->cbData);
        return;
      }
    } else {
      one.lp = lp;
    }
    pending_.push_back(std::move(one));
    if (pending_.size() > kPendingMax) {
      pending_.erase(pending_.begin());
      Log(L"tray", L"intercept pending overflow dropped oldest");
    }
  }

  void FlushPending() {
    if (pending_.empty()) {
      return;
    }
    HWND spy = nullptr;
    {
      std::lock_guard lock(mu_);
      spy = spy_;
    }
    std::vector<PendingMsg> left;
    left.reserve(pending_.size());
    for (PendingMsg& one : pending_) {
      if (!TryForwardQueued(spy, one)) {
        left.push_back(std::move(one));
      }
    }
    if (left.size() != pending_.size()) {
      Log(L"tray", L"intercept pending flushed remain=%zu", left.size());
    }
    pending_.swap(left);
  }

  LRESULT AnswerGetRect(COPYDATASTRUCT* cds) {
    POINT cursor{};
    GetCursorPos(&cursor);
    auto pack = [](LONG v) -> LRESULT {
      const USHORT s = static_cast<USHORT>(v);
      return MAKELRESULT(s, s);
    };
    if (cds == nullptr || cds->lpData == nullptr || cds->cbData < sizeof(NotifyIconIdentifier)) {
      return pack(cursor.x);
    }
    NotifyIconIdentifier id{};
    memcpy(&id, cds->lpData, sizeof(id));
    uint64_t key = 0;
    if (!GuidEmpty(id.guid_item)) {
      key = Fnv1a64(reinterpret_cast<const uint8_t*>(&id.guid_item), sizeof(GUID));
    } else {
      key = Fnv1a64(reinterpret_cast<const uint8_t*>(&id.window_handle), sizeof(id.window_handle));
      key = Fnv1a64(reinterpret_cast<const uint8_t*>(&id.uid), sizeof(id.uid), key);
    }
    RECT rc{};
    bool found = false;
    {
      std::lock_guard lock(mu_);
      if (rect_lookup_) {
        found = rect_lookup_(key, &rc);
      }
    }
    LONG v = id.message == 2 ? cursor.y : cursor.x;
    if (found) {
      v = id.message == 2 ? (rc.top + rc.bottom) / 2 : (rc.left + rc.right) / 2;
    }
    return pack(v);
  }

  void ParseCopyData(COPYDATASTRUCT* cds) {
    if (!parse_enabled_ || cds == nullptr || cds->dwData != 1) {
      return;
    }
    const ULONGLONG t0 = GetTickCount64();
    if (cds->cbData < sizeof(ShellTrayMessage) || cds->lpData == nullptr) {
      NoteParseFail();
      return;
    }
    ShellTrayMessage parsed{};
    memcpy(&parsed, cds->lpData, sizeof(parsed));
    const TrayNotifyIconData& data = parsed.icon_data;
    if (!magic_set_) {
      magic_ = parsed.magic_number;
      magic_set_ = true;
      Log(L"tray", L"intercept magic=%d cb=%lu nid=%u nid_v1=%u v2=%u v3=%u cur=%zu type=%u flags=0x%X", magic_,
          cds->cbData, data.callback_size, static_cast<unsigned>(NOTIFYICONDATAW_V1_SIZE),
          static_cast<unsigned>(NOTIFYICONDATAW_V2_SIZE), static_cast<unsigned>(NOTIFYICONDATAW_V3_SIZE),
          sizeof(NOTIFYICONDATAW), parsed.message_type, data.flags);
    } else if (parsed.magic_number != magic_) {
      NoteParseFail();
      return;
    }
    if (!KnownNotifySize(data.callback_size)) {
      if (parse_fail_ == 0) {
        Log(L"tray", L"intercept nid_size=%u type=%u flags=0x%X", data.callback_size, parsed.message_type, data.flags);
      }
      NoteParseFail();
      return;
    }

    HICON icon = nullptr;
    if ((data.flags & NIF_ICON) != 0) {
      if (data.icon_handle == 0) {
        if (parse_fail_ == 0) {
          Log(L"tray", L"intercept icon handle 0 flags=0x%X", data.flags);
        }
        NoteParseFail();
        return;
      }
      icon = reinterpret_cast<HICON>(static_cast<uintptr_t>(data.icon_handle));
      ICONINFO ii{};
      if (GetIconInfo(icon, &ii) == FALSE) {
        if (parse_fail_ == 0) {
          Log(L"tray", L"intercept GetIconInfo fail handle=0x%X err=%lu", data.icon_handle, GetLastError());
        }
        NoteParseFail();
        return;
      }
      if (ii.hbmColor != nullptr) {
        DeleteObject(ii.hbmColor);
      }
      if (ii.hbmMask != nullptr) {
        DeleteObject(ii.hbmMask);
      }
    }

    parse_fail_ = 0;
    ApplyIconMessage(parsed, icon);
    const ULONGLONG elapsed = GetTickCount64() - t0;
    if (elapsed >= 1 && parse_log_ < 8) {
      ++parse_log_;
      Log(L"tray", L"intercept parse_ms=%llu type=%u", elapsed, parsed.message_type);
    }
  }

  void NoteParseFail() {
    ++parse_fail_;
    if (parse_fail_ == 1 || parse_fail_ == kParseFailMax) {
      Log(L"tray", L"intercept parse fail streak=%ld", parse_fail_);
    }
    if (parse_fail_ >= kParseFailMax && parse_enabled_) {
      parse_enabled_ = false;
      Log(L"tray", L"intercept parse disabled after %ld failures; forwarding only", parse_fail_);
    }
  }

  void ApplyIconMessage(const ShellTrayMessage& parsed, HICON icon) {
    const TrayNotifyIconData& data = parsed.icon_data;
    const uint64_t key = ItemKey(data);
    const HWND owner = reinterpret_cast<HWND>(static_cast<uintptr_t>(data.window_handle));
    std::vector<uint8_t> png;
    bool converted = false;
    if (icon != nullptr && (data.flags & NIF_ICON) != 0) {
      bool skip = false;
      {
        std::lock_guard lock(mu_);
        for (const StoredIcon& one : items_) {
          if (one.info.key == key && one.icon_handle == data.icon_handle) {
            skip = true;
            break;
          }
        }
      }
      if (!skip) {
        const ULONGLONG t0 = GetTickCount64();
        converted = IconToPng(icon, &png);
        const ULONGLONG conv = GetTickCount64() - t0;
        if (converted && icon_log_ < 8) {
          ++icon_log_;
          Log(L"tray", L"intercept icon_ms=%llu png=%zu", conv, png.size());
        }
      }
    }

    std::function<void()> sink;
    {
      std::lock_guard lock(mu_);
      if (parsed.message_type == NIM_DELETE) {
        for (auto it = items_.begin(); it != items_.end(); ++it) {
          if (it->info.key == key) {
            items_.erase(it);
            sink = on_change_;
            break;
          }
        }
      } else if (parsed.message_type == NIM_ADD || parsed.message_type == NIM_MODIFY ||
                 parsed.message_type == NIM_SETVERSION) {
        StoredIcon* slot = nullptr;
        for (StoredIcon& one : items_) {
          if (one.info.key == key) {
            slot = &one;
            break;
          }
        }
        if (slot == nullptr && parsed.message_type != NIM_SETVERSION) {
          StoredIcon created;
          created.info.key = key;
          created.info.order = next_order_++;
          created.info.automation_id = L"NotifyItemIcon";
          items_.push_back(std::move(created));
          slot = &items_.back();
        }
        if (slot == nullptr) {
          return;
        }
        slot->owner = owner;
        slot->info.owner = owner;
        slot->info.uid = data.uid;
        slot->info.callback_message = data.callback_message;
        if (parsed.message_type == NIM_SETVERSION) {
          slot->info.version = data.anonymous;
        } else if (parsed.version != 0) {
          slot->info.version = parsed.version;
        }
        if ((data.flags & NIF_TIP) != 0) {
          slot->info.tip = TipFrom(data.tooltip, 128);
        }
        if ((data.flags & NIF_STATE) != 0) {
          const uint32_t state = (slot->info.offscreen == TRUE ? NIS_HIDDEN : 0);
          const uint32_t next = (state & ~data.state_mask) | (data.state & data.state_mask);
          slot->info.offscreen = (next & NIS_HIDDEN) != 0 ? TRUE : FALSE;
        } else if (parsed.message_type == NIM_ADD) {
          slot->info.offscreen = (data.state & NIS_HIDDEN) != 0 ? TRUE : FALSE;
        }
        if (converted) {
          slot->info.png = std::move(png);
          slot->icon_handle = data.icon_handle;
        }
        sink = on_change_;
      }
    }
    if (sink) {
      sink();
    }
  }

  std::mutex mu_;
  std::thread thread_;
  HWND spy_ = nullptr;
  HANDLE ready_ = nullptr;
  DWORD thread_id_ = 0;
  std::vector<PendingMsg> pending_;
  UINT forward_ok_ = 0;
  UINT forward_fail_ = 0;
  UINT z_loss_ = 0;
  UINT copy_log_ = 0;
  UINT parse_log_ = 0;
  UINT icon_log_ = 0;
  bool parse_enabled_ = true;
  bool magic_set_ = false;
  int32_t magic_ = 0;
  long parse_fail_ = 0;
  int next_order_ = 0;
  std::vector<StoredIcon> items_;
  std::function<void()> on_change_;
  std::function<bool(uint64_t, RECT*)> rect_lookup_;
};

}  // namespace

std::unique_ptr<TrayBackend> MakeInterceptTrayBackend() {
  return std::make_unique<TrayBackendIntercept>();
}

}  // namespace bamti
