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

 private:
  struct Client;
  struct Record {
    StatusItem item;
    uint64_t owner = 0;
  };

  void ListenLoop();
  void ClientLoop(Client* client);
  void HandleLine(Client* client, std::string_view line);
  void NotifyUi();
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
};

}  // namespace bamti
