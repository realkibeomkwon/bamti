#pragma once

#include <cstdint>
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
  BOOL offscreen = FALSE;
};

class TrayBackend {
 public:
  virtual ~TrayBackend() = default;
  virtual const char* Name() const = 0;
  virtual bool Probe() = 0;
  virtual bool Enumerate(std::vector<TrayIconInfo>* out) = 0;
  virtual bool Invoke(const TrayIconInfo& icon) = 0;
  virtual void Reset() = 0;
  virtual bool SubscribeStructureChanged(HANDLE wake) {
    (void)wake;
    return false;
  }
  virtual void UnsubscribeStructureChanged() {}
  virtual bool StructureChangedLive() const { return false; }
  virtual long TakeStructureChangedCount() { return 0; }
  virtual void AbandonStructureChanged() {}
  virtual void LastInvokeError(HRESULT* hr, const char** pattern) const {
    if (hr != nullptr) {
      *hr = S_OK;
    }
    if (pattern != nullptr) {
      *pattern = "";
    }
  }
};

std::unique_ptr<TrayBackend> MakeUiaTrayBackend();

}  // namespace bamti
