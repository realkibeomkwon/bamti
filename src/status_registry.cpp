#include "status_registry.hpp"

#include "log.hpp"

#include <algorithm>

namespace bamti {
namespace {

constexpr ULONGLONG kNotifyCoalesceMs = 50;

ULONGLONG NowMs() {
  return GetTickCount64();
}

}  // namespace

void StatusRegistry::SetNotify(HWND hwnd) {
  std::lock_guard lock(mu_);
  notify_ = hwnd;
}

void StatusRegistry::Register(StatusSource* source) {
  if (source == nullptr) {
    return;
  }
  for (StatusSource* existing : sources_) {
    if (existing == source) {
      return;
    }
  }
  sources_.push_back(source);
}

bool StatusRegistry::StartAll() {
  for (StatusSource* source : sources_) {
    if (source == nullptr) {
      continue;
    }
    if (!source->Start(this)) {
      return false;
    }
  }
  return true;
}

void StatusRegistry::StopAll() {
  for (auto it = sources_.rbegin(); it != sources_.rend(); ++it) {
    if (*it != nullptr) {
      (*it)->Stop();
    }
  }
  std::lock_guard lock(mu_);
  items_.clear();
  notify_pending_ = false;
}

void StatusRegistry::SetActive(bool active) {
  for (StatusSource* source : sources_) {
    if (source != nullptr) {
      source->SetActive(active);
    }
  }
}

std::vector<StatusItem> StatusRegistry::Snapshot() const {
  std::lock_guard lock(mu_);
  std::vector<StatusItem> out;
  out.reserve(items_.size());
  for (const auto& pair : items_) {
    out.push_back(pair.second.item);
  }
  std::sort(out.begin(), out.end(), [](const StatusItem& a, const StatusItem& b) {
    if (a.priority != b.priority) {
      return a.priority > b.priority;
    }
    return a.id < b.id;
  });
  return out;
}

void StatusRegistry::Dispatch(const StatusEvent& ev) {
  StatusSource* source = nullptr;
  {
    std::lock_guard lock(mu_);
    const auto it = items_.find(ev.id);
    if (it == items_.end()) {
      return;
    }
    source = it->second.source;
  }
  if (source != nullptr) {
    source->OnEvent(ev);
  }
}

void StatusRegistry::DropStale() {
  for (StatusSource* source : sources_) {
    if (source != nullptr) {
      source->DropStale();
    }
  }
  FlushNotify();
}

StatusSource* StatusRegistry::SourceByName(const std::string& name) const {
  for (StatusSource* source : sources_) {
    if (source != nullptr && source->Name() != nullptr && name == source->Name()) {
      return source;
    }
  }
  return nullptr;
}

void StatusRegistry::Upsert(StatusItem item) {
  if (item.id.empty()) {
    return;
  }
  {
    std::lock_guard lock(mu_);
    const auto it = items_.find(item.id);
    if (it == items_.end() && items_.size() >= kStatusItemsMax) {
      if (!logged_total_limit_) {
        logged_total_limit_ = true;
        Log(L"status", L"total item limit %zu reached; dropping upsert", kStatusItemsMax);
      }
      return;
    }
    std::string key = item.id;
    Record rec;
    rec.source = SourceByName(item.source);
    rec.item = std::move(item);
    items_.insert_or_assign(std::move(key), std::move(rec));
  }
  NotifyUi();
}

void StatusRegistry::Remove(const std::string& id) {
  {
    std::lock_guard lock(mu_);
    if (items_.erase(id) == 0) {
      return;
    }
  }
  NotifyUi();
}

std::optional<StatusItem> StatusRegistry::Get(const std::string& id) const {
  std::lock_guard lock(mu_);
  const auto it = items_.find(id);
  if (it == items_.end()) {
    return std::nullopt;
  }
  return it->second.item;
}

void StatusRegistry::Flush() {
  FlushNotify();
}

uint32_t StatusRegistry::NotifyWaitTimeoutMs() {
  std::lock_guard lock(mu_);
  if (!notify_pending_) {
    return 0xFFFFFFFFu;
  }
  const ULONGLONG now = NowMs();
  if (last_notify_ms_ == 0 || now - last_notify_ms_ >= kNotifyCoalesceMs) {
    return 0;
  }
  return static_cast<uint32_t>(kNotifyCoalesceMs - (now - last_notify_ms_));
}

void StatusRegistry::NotifyUi() {
  HWND hwnd = nullptr;
  {
    std::lock_guard lock(mu_);
    const ULONGLONG now = NowMs();
    if (last_notify_ms_ != 0 && now - last_notify_ms_ < kNotifyCoalesceMs) {
      notify_pending_ = true;
      return;
    }
    last_notify_ms_ = now;
    notify_pending_ = false;
    hwnd = notify_;
  }
  if (hwnd != nullptr) {
    PostMessageW(hwnd, kStatusChangedMsg, 0, 0);
  }
}

void StatusRegistry::FlushNotify() {
  HWND hwnd = nullptr;
  {
    std::lock_guard lock(mu_);
    if (!notify_pending_) {
      return;
    }
    notify_pending_ = false;
    last_notify_ms_ = NowMs();
    hwnd = notify_;
  }
  if (hwnd != nullptr) {
    PostMessageW(hwnd, kStatusChangedMsg, 0, 0);
  }
}

}  // namespace bamti
