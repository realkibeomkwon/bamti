#pragma once

#include "status_item.hpp"

#include <cstdint>
#include <optional>
#include <string>

namespace bamti {

struct StatusEvent {
  std::string id;
  std::string event;
  std::string row_id;
  std::string button;
  bool on = false;
  // event == "slide"일 때는 절대값(0.0~1.0), event == "scroll"일 때는 상대 증감이다.
  float value = 0.0f;
};

class StatusSink {
 public:
  virtual ~StatusSink() = default;
  virtual void Upsert(StatusItem item) = 0;
  virtual void Remove(const std::string& id) = 0;
  virtual std::optional<StatusItem> Get(const std::string& id) const = 0;
  virtual void Flush() {}
  virtual uint32_t NotifyWaitTimeoutMs() { return 0xFFFFFFFFu; }
};

class StatusSource {
 public:
  virtual ~StatusSource() = default;
  virtual const char* Name() const = 0;
  virtual bool Start(StatusSink* sink) = 0;
  virtual void Stop() = 0;
  virtual void OnEvent(const StatusEvent& ev) { (void)ev; }
  virtual void SetActive(bool active) { (void)active; }
  virtual void DropStale() {}
};

}  // namespace bamti
