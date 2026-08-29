#include "tray_intercept.hpp"

#include "log.hpp"

#include <windows.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
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

class TrayBackendIntercept final : public TrayBackend {
 public:
  const char* Name() const override { return "intercept"; }

  ~TrayBackendIntercept() { StopSpy(); }

  bool Probe() override { return StartSpy(); }

  bool Enumerate(std::vector<TrayIconInfo>* out) override {
    if (out == nullptr) {
      return false;
    }
    out->clear();
    return true;
  }

  bool Invoke(const TrayIconInfo&) override { return false; }

  void Reset() override {}

  void SetChangeSink(std::function<void()> on_change) override { (void)on_change; }

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
    Log(L"tray", L"intercept pass-through spy=0x%llX explorer=0x%llX timer_ms=%u",
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(spy)),
        static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(FindRealTray(spy))), kPrioTimerMs);

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
    Log(L"tray", L"intercept spy stop forwarded=%u fail=%u pending=%zu", forward_ok_, forward_fail_,
        pending_.size());
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
};

}  // namespace

std::unique_ptr<TrayBackend> MakeInterceptTrayBackend() {
  return std::make_unique<TrayBackendIntercept>();
}

}  // namespace bamti
