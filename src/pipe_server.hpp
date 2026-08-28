#pragma once

#include "status_item.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bamti {

class PipeServer {
 public:
  PipeServer();
  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;
  ~PipeServer();

  bool Start(HWND notify);
  void Stop();
  void DropStale();
  std::vector<StatusItem> Snapshot() const;
  void SendClick(const std::string& id, std::string_view button);
  void SendEvent(const std::string& id, std::string_view event, std::string_view row_id = {},
                 std::string_view button = {}, bool on = false);

 private:
  struct Client;
  struct Record {
    StatusItem item;
    uint64_t owner = 0;
  };

  void ListenLoop();
  void ClientLoop(Client* client);
  void HandleLine(Client* client, std::string_view line);
  void HandleV1(Client* client, std::string_view line);
  void HandleV2(Client* client, std::string_view line);
  bool AllowUpsertLocked(const std::string& id, uint64_t owner);
  void NotifyUi();
  void FlushNotify();
  DWORD NotifyWaitTimeoutMs();
  HANDLE CreateListenPipe();
  void CloseClientPipe(Client* client);
  bool WriteLine(Client* client, std::string_view line);

  HWND notify_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE listen_pipe_ = INVALID_HANDLE_VALUE;
  std::vector<uint8_t> sd_bytes_;
  uint64_t next_id_ = 1;
  std::thread listen_thread_;
  mutable std::mutex mu_;
  std::vector<std::unique_ptr<Client>> clients_;
  std::unordered_map<std::string, Record> items_;
  ULONGLONG last_notify_ms_ = 0;
  bool notify_pending_ = false;
  bool logged_client_limit_ = false;
  bool logged_total_limit_ = false;
};

}  // namespace bamti
