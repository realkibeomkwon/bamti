#pragma once

#include "status_item.hpp"
#include "status_source.hpp"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace bamti {

class StatusRegistry : public StatusSink {
 public:
  StatusRegistry() = default;
  StatusRegistry(const StatusRegistry&) = delete;
  StatusRegistry& operator=(const StatusRegistry&) = delete;

  void SetNotify(HWND hwnd);
  void Register(StatusSource* source);
  bool StartAll();
  void StopAll();
  void SetActive(bool active);
  std::vector<StatusItem> Snapshot() const;
  void Dispatch(const StatusEvent& ev);
  void DropStale();

  void Upsert(StatusItem item) override;
  void Remove(const std::string& id) override;
  std::optional<StatusItem> Get(const std::string& id) const override;
  void Flush() override;
  uint32_t NotifyWaitTimeoutMs() override;

 private:
  struct Record {
    StatusItem item;
    StatusSource* source = nullptr;
  };

  void NotifyUi();
  void FlushNotify();
  StatusSource* SourceByName(const std::string& name) const;

  HWND notify_ = nullptr;
  std::vector<StatusSource*> sources_;
  mutable std::mutex mu_;
  std::unordered_map<std::string, Record> items_;
  ULONGLONG last_notify_ms_ = 0;
  bool notify_pending_ = false;
  bool logged_total_limit_ = false;
};

}  // namespace bamti
