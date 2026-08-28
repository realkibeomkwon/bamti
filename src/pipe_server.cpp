#include "pipe_server.hpp"

#include "json_line.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <optional>

namespace bamti {
namespace {

constexpr DWORD kPipeBuffer = 16384;
constexpr DWORD kMaxPipeInstances = 16;
constexpr ULONGLONG kHeartbeatMs = 15000;
constexpr size_t kMaxLineBytes = 16384;
constexpr size_t kMaxIdBytes = 128;

struct UserOnlySd {
  std::vector<uint8_t> token_user;
  std::vector<uint8_t> relative;
  bool ok = false;
};

UserOnlySd BuildCurrentUserSd() {
  UserOnlySd out;
  HANDLE token = nullptr;
  if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token)) {
    return out;
  }
  DWORD needed = 0;
  GetTokenInformation(token, TokenUser, nullptr, 0, &needed);
  out.token_user.resize(needed);
  if (!GetTokenInformation(token, TokenUser, out.token_user.data(), needed, &needed)) {
    CloseHandle(token);
    return out;
  }
  CloseHandle(token);

  auto* user = reinterpret_cast<TOKEN_USER*>(out.token_user.data());
  EXPLICIT_ACCESSW access{};
  access.grfAccessPermissions = GENERIC_READ | GENERIC_WRITE | SYNCHRONIZE;
  access.grfAccessMode = SET_ACCESS;
  access.grfInheritance = NO_INHERITANCE;
  access.Trustee.TrusteeForm = TRUSTEE_IS_SID;
  access.Trustee.TrusteeType = TRUSTEE_IS_USER;
  access.Trustee.ptstrName = static_cast<LPWSTR>(user->User.Sid);

  PACL acl = nullptr;
  if (SetEntriesInAclW(1, &access, nullptr, &acl) != ERROR_SUCCESS) {
    return out;
  }

  SECURITY_DESCRIPTOR absolute{};
  if (!InitializeSecurityDescriptor(&absolute, SECURITY_DESCRIPTOR_REVISION) ||
      !SetSecurityDescriptorDacl(&absolute, TRUE, acl, FALSE)) {
    LocalFree(acl);
    return out;
  }

  DWORD sd_bytes = 0;
  MakeSelfRelativeSD(&absolute, nullptr, &sd_bytes);
  out.relative.resize(sd_bytes);
  if (!MakeSelfRelativeSD(&absolute, reinterpret_cast<PSECURITY_DESCRIPTOR>(out.relative.data()), &sd_bytes)) {
    LocalFree(acl);
    out.relative.clear();
    return out;
  }
  LocalFree(acl);
  out.ok = true;
  return out;
}

std::wstring Utf8ToWide(std::string_view u8) {
  if (u8.empty()) {
    return {};
  }
  const int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), nullptr, 0);
  if (n <= 0) {
    return {};
  }
  std::wstring wide(static_cast<size_t>(n), L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, u8.data(), static_cast<int>(u8.size()), wide.data(), n);
  return wide;
}

std::wstring TruncateWide(std::wstring text, size_t max_chars) {
  if (text.size() <= max_chars) {
    return text;
  }
  if (max_chars == 0) {
    return {};
  }
  text.resize(max_chars - 1);
  text.push_back(L'\u2026');
  return text;
}

std::wstring TruncateLabel(std::wstring text) {
  return TruncateWide(std::move(text), kStatusTextMaxChars);
}

float ClampUnit(double value) {
  if (value < 0.0) {
    return 0.0f;
  }
  if (value > 1.0) {
    return 1.0f;
  }
  return static_cast<float>(value);
}

std::optional<StatusPanel> ParsePanel(std::string_view raw) {
  StatusPanel panel;
  if (const auto title = json::GetString(raw, "title")) {
    panel.title = TruncateWide(Utf8ToWide(*title), kStatusPanelTextMaxChars);
  }
  if (const auto subtitle = json::GetString(raw, "subtitle")) {
    panel.subtitle = TruncateWide(Utf8ToWide(*subtitle), kStatusPanelTextMaxChars);
  }
  if (const auto updated = json::GetString(raw, "updated")) {
    panel.updated_text = TruncateWide(Utf8ToWide(*updated), kStatusPanelTextMaxChars);
  }
  if (const auto gauges = json::GetRaw(raw, "gauges")) {
    json::ForEachArray(*gauges, [&](std::string_view one) {
      size_t gauges = 0;
      for (const StatusRow& row : panel.rows) {
        if (row.type == RowType::kGauge) {
          ++gauges;
        }
      }
      if (gauges >= kStatusGaugeMax || panel.rows.size() >= kStatusRowMax) {
        return true;
      }
      StatusRow row;
      row.type = RowType::kGauge;
      if (const auto label = json::GetString(one, "label")) {
        row.label = TruncateWide(Utf8ToWide(*label), kStatusPanelTextMaxChars);
      }
      if (const auto value = json::GetDouble(one, "value")) {
        row.value = ClampUnit(*value);
      }
      if (const auto detail = json::GetString(one, "detail")) {
        row.detail = TruncateWide(Utf8ToWide(*detail), kStatusPanelTextMaxChars);
      }
      if (const auto note = json::GetString(one, "note")) {
        row.note = TruncateWide(Utf8ToWide(*note), kStatusPanelTextMaxChars);
      }
      panel.rows.push_back(std::move(row));
      return true;
    });
  }
  std::vector<std::wstring> actions;
  for (const auto& action : json::GetStringArray(raw, "actions")) {
    if (actions.size() >= kStatusActionMax) {
      break;
    }
    const std::wstring wide = TruncateWide(Utf8ToWide(action), kStatusPanelTextMaxChars);
    if (!wide.empty()) {
      actions.push_back(wide);
    }
  }
  if (!actions.empty() && panel.rows.size() < kStatusRowMax) {
    StatusRow sep;
    sep.type = RowType::kSeparator;
    panel.rows.push_back(std::move(sep));
    for (size_t i = 0; i < actions.size() && panel.rows.size() < kStatusRowMax; ++i) {
      StatusRow button;
      button.type = RowType::kButton;
      button.row_id = std::to_string(i);
      button.label = actions[i];
      panel.rows.push_back(std::move(button));
    }
  }
  if (panel.title.empty() && panel.rows.empty()) {
    return std::nullopt;
  }
  return panel;
}

bool ValidId(std::string_view id) {
  if (id.empty() || id.size() > kMaxIdBytes) {
    return false;
  }
  return std::all_of(id.begin(), id.end(), [](unsigned char c) { return c >= 0x20 && c != '"'; });
}

ULONGLONG NowMs() {
  return GetTickCount64();
}

}  // namespace

struct PipeServer::Client {
  uint64_t id = 0;
  HANDLE pipe = INVALID_HANDLE_VALUE;
  std::thread thread;
  std::mutex write_mu;
  std::atomic<ULONGLONG> last_tick{0};
  std::atomic<bool> alive{true};
};

PipeServer::PipeServer() = default;

PipeServer::~PipeServer() {
  Stop();
}

bool PipeServer::Start(HWND notify) {
  Stop();
  notify_ = notify;
  auto sd = BuildCurrentUserSd();
  if (!sd.ok) {
    return false;
  }
  sd_bytes_ = std::move(sd.relative);
  stop_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (stop_event_ == nullptr) {
    return false;
  }
  listen_thread_ = std::thread([this] { ListenLoop(); });
  return true;
}

void PipeServer::Stop() {
  if (stop_event_ != nullptr) {
    SetEvent(stop_event_);
  }
  HANDLE listen = INVALID_HANDLE_VALUE;
  {
    std::lock_guard lock(mu_);
    notify_ = nullptr;
    listen = listen_pipe_;
    listen_pipe_ = INVALID_HANDLE_VALUE;
  }
  if (listen != INVALID_HANDLE_VALUE) {
    CancelIoEx(listen, nullptr);
    CloseHandle(listen);
  }
  if (listen_thread_.joinable()) {
    listen_thread_.join();
  }
  std::vector<std::thread> join;
  {
    std::lock_guard lock(mu_);
    for (auto& client : clients_) {
      CloseClientPipe(client.get());
      if (client && client->thread.joinable()) {
        join.push_back(std::move(client->thread));
      }
    }
  }
  for (auto& t : join) {
    t.join();
  }
  {
    std::lock_guard lock(mu_);
    clients_.clear();
    items_.clear();
  }
  if (stop_event_ != nullptr) {
    CloseHandle(stop_event_);
    stop_event_ = nullptr;
  }
  sd_bytes_.clear();
}

void PipeServer::DropStale() {
  const ULONGLONG now = NowMs();
  bool closed = false;
  {
    std::lock_guard lock(mu_);
    for (auto& client : clients_) {
      if (client && client->alive.load() && now - client->last_tick.load() > kHeartbeatMs) {
        CloseClientPipe(client.get());
        closed = true;
      }
    }
  }
  if (closed) {
    // ClientLoop removes items and notifies the UI when ReadFile unblocks.
  }
}

std::vector<StatusItem> PipeServer::Snapshot() const {
  std::lock_guard lock(mu_);
  std::vector<StatusItem> out;
  out.reserve(items_.size());
  for (const auto& [id, rec] : items_) {
    out.push_back(rec.item);
  }
  return out;
}

void PipeServer::SendClick(const std::string& id, std::string_view button) {
  uint64_t owner = 0;
  {
    std::lock_guard lock(mu_);
    const auto it = items_.find(id);
    if (it == items_.end()) {
      return;
    }
    owner = it->second.owner;
  }
  Client* target = nullptr;
  {
    std::lock_guard lock(mu_);
    for (auto& client : clients_) {
      if (client && client->id == owner) {
        target = client.get();
        break;
      }
    }
  }
  if (target == nullptr) {
    return;
  }
  std::string line = "{\"v\":1,\"op\":\"click\",\"id\":\"";
  line += json::Escape(id);
  line += "\",\"button\":\"";
  line += json::Escape(button);
  line += "\"}";
  WriteLine(target, line);
}

HANDLE PipeServer::CreateListenPipe() {
  SECURITY_ATTRIBUTES sa{};
  sa.nLength = sizeof(sa);
  sa.bInheritHandle = FALSE;
  sa.lpSecurityDescriptor = sd_bytes_.empty() ? nullptr : sd_bytes_.data();

  DWORD mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS;
  HANDLE pipe = CreateNamedPipeW(kStatusPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, mode, kMaxPipeInstances,
                                 kPipeBuffer, kPipeBuffer, 50, &sa);
  if (pipe != INVALID_HANDLE_VALUE) {
    return pipe;
  }
  mode = PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT;
  return CreateNamedPipeW(kStatusPipeName, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED, mode, kMaxPipeInstances,
                          kPipeBuffer, kPipeBuffer, 50, &sa);
}

void PipeServer::ListenLoop() {
  while (stop_event_ != nullptr && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    HANDLE pipe = CreateListenPipe();
    if (pipe == INVALID_HANDLE_VALUE) {
      if (WaitForSingleObject(stop_event_, 250) == WAIT_OBJECT_0) {
        break;
      }
      continue;
    }
    {
      std::lock_guard lock(mu_);
      listen_pipe_ = pipe;
    }

    OVERLAPPED ov{};
    ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (ov.hEvent == nullptr) {
      CloseHandle(pipe);
      break;
    }

    BOOL connected = ConnectNamedPipe(pipe, &ov);
    DWORD err = connected ? ERROR_SUCCESS : GetLastError();
    if (err == ERROR_IO_PENDING) {
      const HANDLE waits[] = {stop_event_, ov.hEvent};
      const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (wait == WAIT_OBJECT_0) {
        CancelIoEx(pipe, &ov);
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        std::lock_guard lock(mu_);
        if (listen_pipe_ == pipe) {
          listen_pipe_ = INVALID_HANDLE_VALUE;
        }
        break;
      }
      DWORD unused = 0;
      if (!GetOverlappedResult(pipe, &ov, &unused, FALSE)) {
        CloseHandle(ov.hEvent);
        CloseHandle(pipe);
        std::lock_guard lock(mu_);
        if (listen_pipe_ == pipe) {
          listen_pipe_ = INVALID_HANDLE_VALUE;
        }
        continue;
      }
    } else if (err != ERROR_SUCCESS && err != ERROR_PIPE_CONNECTED) {
      CloseHandle(ov.hEvent);
      CloseHandle(pipe);
      std::lock_guard lock(mu_);
      if (listen_pipe_ == pipe) {
        listen_pipe_ = INVALID_HANDLE_VALUE;
      }
      continue;
    }
    CloseHandle(ov.hEvent);

    auto client = std::make_unique<Client>();
    client->pipe = pipe;
    client->last_tick.store(NowMs());
    Client* raw = nullptr;
    {
      std::lock_guard lock(mu_);
      if (WaitForSingleObject(stop_event_, 0) == WAIT_OBJECT_0) {
        CloseHandle(pipe);
        if (listen_pipe_ == pipe) {
          listen_pipe_ = INVALID_HANDLE_VALUE;
        }
        break;
      }
      if (listen_pipe_ == pipe) {
        listen_pipe_ = INVALID_HANDLE_VALUE;
      }
      client->id = next_id_++;
      raw = client.get();
      clients_.push_back(std::move(client));
    }
    raw->thread = std::thread([this, raw] { ClientLoop(raw); });
  }
}

void PipeServer::ClientLoop(Client* client) {
  std::string pending;
  char chunk[1024];
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    return;
  }

  while (client->alive.load() && WaitForSingleObject(stop_event_, 0) != WAIT_OBJECT_0) {
    ResetEvent(ov.hEvent);
    DWORD read = 0;
    const BOOL ok = ReadFile(client->pipe, chunk, sizeof(chunk), &read, &ov);
    if (!ok) {
      const DWORD err = GetLastError();
      if (err != ERROR_IO_PENDING) {
        break;
      }
      const HANDLE waits[] = {stop_event_, ov.hEvent};
      const DWORD wait = WaitForMultipleObjects(2, waits, FALSE, INFINITE);
      if (wait != WAIT_OBJECT_0 + 1) {
        CancelIoEx(client->pipe, &ov);
        break;
      }
      if (!GetOverlappedResult(client->pipe, &ov, &read, FALSE) || read == 0) {
        break;
      }
    } else if (read == 0) {
      break;
    }
    pending.append(chunk, chunk + read);
    if (pending.size() > kMaxLineBytes * 2) {
      break;
    }
    size_t start = 0;
    for (;;) {
      const size_t nl = pending.find('\n', start);
      if (nl == std::string::npos) {
        break;
      }
      std::string_view line(pending.data() + start, nl - start);
      if (!line.empty() && line.back() == '\r') {
        line.remove_suffix(1);
      }
      if (!line.empty()) {
        HandleLine(client, line);
      }
      start = nl + 1;
    }
    pending.erase(0, start);
    if (pending.size() > kMaxLineBytes) {
      break;
    }
  }
  CloseHandle(ov.hEvent);

  const uint64_t owner = client->id;
  {
    std::lock_guard lock(mu_);
    client->alive.store(false);
    CloseClientPipe(client);
    for (auto it = items_.begin(); it != items_.end();) {
      if (it->second.owner == owner) {
        it = items_.erase(it);
      } else {
        ++it;
      }
    }
  }
  NotifyUi();
}

void PipeServer::HandleLine(Client* client, std::string_view line) {
  client->last_tick.store(NowMs());
  const auto op = json::GetString(line, "op");
  const auto ver = json::GetInt(line, "v");
  if (!op || !ver || *ver != 1) {
    return;
  }
  if (*op == "ping") {
    return;
  }

  bool changed = false;
  if (*op == "remove") {
    const auto id = json::GetString(line, "id");
    if (!id || !ValidId(*id)) {
      return;
    }
    std::lock_guard lock(mu_);
    const auto it = items_.find(*id);
    if (it != items_.end() && it->second.owner == client->id) {
      items_.erase(it);
      changed = true;
    }
  } else if (*op == "upsert") {
    const auto id = json::GetString(line, "id");
    const auto text = json::GetString(line, "text");
    if (!id || !text || !ValidId(*id)) {
      return;
    }
    StatusItem item;
    item.id = *id;
    item.source = "pipe";
    item.text = TruncateLabel(Utf8ToWide(*text));
    if (item.text.empty() && !text->empty()) {
      return;
    }
    if (const auto tip = json::GetString(line, "tooltip")) {
      item.tooltip = Utf8ToWide(*tip);
    }
    std::wstring glyph;
    if (const auto g = json::GetString(line, "icon_glyph")) {
      glyph = TruncateWide(Utf8ToWide(*g), kStatusGlyphMaxChars);
    } else if (const auto icon = json::GetString(line, "icon")) {
      glyph = TruncateWide(Utf8ToWide(*icon), kStatusGlyphMaxChars);
    }
    if (!glyph.empty()) {
      item.icon.kind = IconKind::kGlyph;
      item.icon.glyph = std::move(glyph);
      item.icon.cache_key = HashStatusIcon(item.icon);
    }
    if (const auto accent = json::GetUint32(line, "accent")) {
      item.accent = *accent;
    }
    if (const auto pri = json::GetInt(line, "priority")) {
      item.priority = *pri;
    }
    if (const auto panel = json::GetRaw(line, "panel")) {
      item.panel = ParsePanel(*panel);
    }
    std::lock_guard lock(mu_);
    auto it = items_.find(item.id);
    item.revision = (it == items_.end()) ? 1 : it->second.item.revision + 1;
    items_[item.id] = Record{std::move(item), client->id};
    changed = true;
  }
  if (changed) {
    NotifyUi();
  }
}

void PipeServer::NotifyUi() {
  HWND hwnd = nullptr;
  {
    std::lock_guard lock(mu_);
    hwnd = notify_;
  }
  if (hwnd != nullptr) {
    PostMessageW(hwnd, kStatusChangedMsg, 0, 0);
  }
}

void PipeServer::CloseClientPipe(Client* client) {
  if (client == nullptr) {
    return;
  }
  client->alive.store(false);
  std::lock_guard write(client->write_mu);
  if (client->pipe != INVALID_HANDLE_VALUE) {
    CancelIoEx(client->pipe, nullptr);
    DisconnectNamedPipe(client->pipe);
    CloseHandle(client->pipe);
    client->pipe = INVALID_HANDLE_VALUE;
  }
}

bool PipeServer::WriteLine(Client* client, std::string_view line) {
  if (client == nullptr) {
    return false;
  }
  std::lock_guard write(client->write_mu);
  if (client->pipe == INVALID_HANDLE_VALUE) {
    return false;
  }
  std::string data(line);
  data.push_back('\n');
  OVERLAPPED ov{};
  ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
  if (ov.hEvent == nullptr) {
    return false;
  }
  DWORD written = 0;
  BOOL ok = WriteFile(client->pipe, data.data(), static_cast<DWORD>(data.size()), &written, &ov);
  if (!ok && GetLastError() == ERROR_IO_PENDING) {
    ok = WaitForSingleObject(ov.hEvent, 250) == WAIT_OBJECT_0 &&
         GetOverlappedResult(client->pipe, &ov, &written, FALSE);
  }
  CloseHandle(ov.hEvent);
  return ok != 0;
}

}  // namespace bamti
