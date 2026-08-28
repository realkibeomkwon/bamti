#pragma once

#include "status_item.hpp"
#include "status_source.hpp"

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace bamti {

class PipeServer : public StatusSource {
 public:
  PipeServer();
  PipeServer(const PipeServer&) = delete;
  PipeServer& operator=(const PipeServer&) = delete;
  ~PipeServer() override;

  const char* Name() const override;
  bool Start(StatusSink* sink) override;
  void Stop() override;
  void OnEvent(const StatusEvent& ev) override;
  void SetActive(bool active) override;
  void DropStale() override;

 private:
  struct Client;

  void ListenLoop();
  void ClientLoop(Client* client);
  void HandleLine(Client* client, std::string_view line);
  void HandleV1(Client* client, std::string_view line);
  void HandleV2(Client* client, std::string_view line);
  bool AllowUpsertLocked(const std::string& id, uint64_t owner);
  void PublishUpsert(StatusItem item, uint64_t owner);
  void PublishRemove(const std::string& id, uint64_t owner);
  void SendEvent(const std::string& id, std::string_view event, std::string_view row_id = {},
                 std::string_view button = {}, bool on = false);
  HANDLE CreateListenPipe();
  void CloseClientPipe(Client* client);
  bool WriteLine(Client* client, std::string_view line);

  StatusSink* sink_ = nullptr;
  HANDLE stop_event_ = nullptr;
  HANDLE listen_pipe_ = INVALID_HANDLE_VALUE;
  std::vector<uint8_t> sd_bytes_;
  uint64_t next_id_ = 1;
  std::thread listen_thread_;
  mutable std::mutex mu_;
  std::vector<std::unique_ptr<Client>> clients_;
  std::unordered_map<std::string, uint64_t> owners_;
  bool active_ = true;
  bool logged_client_limit_ = false;
  bool logged_total_limit_ = false;
};

}  // namespace bamti
