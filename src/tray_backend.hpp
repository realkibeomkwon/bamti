#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include <windows.h>

namespace bamti {

struct TrayIconInfo {
  uint64_t key = 0;
  std::wstring tip;
  std::wstring automation_id;
  std::wstring class_name;
  RECT screen{};
  int order = 0;
  bool system_icon = false;
  bool has_image_child = false;
  bool from_overflow = false;
  BOOL offscreen = FALSE;
  std::vector<uint8_t> png;
  HWND owner = nullptr;
  UINT uid = 0;
  UINT callback_message = 0;
  UINT version = 0;
  GUID guid_item{};  // 등록에 GUID가 쓰였을 때만 채운다. UIA 백엔드는 채우지 않는다.
  std::wstring owner_exe;  // 가로채기 백엔드만 채운다. UIA 백엔드는 비워 둔다.
};

class TrayBackend {
 public:
  virtual ~TrayBackend() = default;
  virtual const char* Name() const = 0;
  virtual bool Probe() = 0;
  virtual bool Enumerate(std::vector<TrayIconInfo>* out) = 0;
  virtual bool Invoke(const TrayIconInfo& icon) = 0;
  virtual void Reset() = 0;
  // 백엔드가 스스로 변경을 알릴 수 있으면 이 콜백을 쓴다.
  // UIA 백엔드는 쓰지 않고, 가로채기 백엔드는 메시지를 받을 때마다 부른다.
  virtual void SetChangeSink(std::function<void()> on_change) { (void)on_change; }
  virtual void SetRectLookup(std::function<bool(uint64_t key, RECT* screen)> lookup) { (void)lookup; }
  virtual bool ParseLive() const { return true; }
  virtual bool Invoke(const TrayIconInfo& icon, bool right) {
    (void)right;
    return Invoke(icon);
  }
  virtual bool Invoke(const TrayIconInfo& icon, bool right, bool dblclk) {
    (void)dblclk;
    return Invoke(icon, right);
  }
  virtual bool ForwardsContextMenu() const { return false; }
  virtual bool SubscribeStructureChanged(HANDLE wake) {
    (void)wake;
    return false;
  }
  virtual void UnsubscribeStructureChanged() {}
  virtual bool StructureChangedLive() const { return false; }
  virtual long TakeStructureChangedCount() { return 0; }
  virtual void AbandonStructureChanged() {}
  virtual void AllowStructureRetry() {}
  virtual void LastInvokeError(HRESULT* hr, const char** pattern) const {
    if (hr != nullptr) {
      *hr = S_OK;
    }
    if (pattern != nullptr) {
      *pattern = "";
    }
  }
  // 셸이 재시작했다. 기본 구현은 아무 일도 하지 않는다.
  virtual void OnShellRestart() {}
};

std::unique_ptr<TrayBackend> MakeUiaTrayBackend();
std::unique_ptr<TrayBackend> MakeInterceptTrayBackend();

}  // namespace bamti
