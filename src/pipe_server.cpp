#include "pipe_server.hpp"

#include "json_line.hpp"
#include "log.hpp"

#include <aclapi.h>
#include <sddl.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <optional>

namespace bamti {
namespace {

constexpr DWORD kPipeBuffer = 16384;
constexpr DWORD kMaxPipeInstances = 16;
constexpr ULONGLONG kHeartbeatMs = 15000;
constexpr size_t kMaxLineBytes = kStatusLineMaxBytes;
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
  if (!std::isfinite(value) || value < 0.0) {
    return 0.0f;
  }
  if (value > 1.0) {
    return 1.0f;
  }
  return static_cast<float>(value);
}

bool DecodeBase64(std::string_view in, std::vector<uint8_t>* out) {
  static const int8_t kTable[256] = {
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
      -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63, 52, 53, 54, 55,
      56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1, -1, 0,  1,  2,  3,  4,  5,  6,  7,  8,  9,  10, 11, 12,
      13, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1, -1, 26, 27, 28, 29, 30, 31, 32,
      33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1};
  out->clear();
  int val = 0;
  int bits = 0;
  for (unsigned char c : in) {
    if (c == '=' || c <= ' ') {
      continue;
    }
    const int8_t d = kTable[c];
    if (d < 0) {
      out->clear();
      return false;
    }
    val = (val << 6) | d;
    bits += 6;
    if (bits >= 8) {
      bits -= 8;
      out->push_back(static_cast<uint8_t>((val >> bits) & 0xFF));
    }
  }
  return true;
}

void SkipRawWs(std::string_view& s) {
  while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') {
    s.remove_prefix(1);
  }
}

uint32_t ParseHexRgb(std::string_view s) {
  if (s.size() < 7 || s.front() != '#') {
    return 0;
  }
  unsigned rgb = 0;
  for (int i = 1; i <= 6; ++i) {
    unsigned nibble = 0;
    const char c = s[static_cast<size_t>(i)];
    if (c >= '0' && c <= '9') {
      nibble = static_cast<unsigned>(c - '0');
    } else if (c >= 'a' && c <= 'f') {
      nibble = static_cast<unsigned>(c - 'a' + 10);
    } else if (c >= 'A' && c <= 'F') {
      nibble = static_cast<unsigned>(c - 'A' + 10);
    } else {
      return 0;
    }
    rgb = (rgb << 4) | nibble;
  }
  return rgb;
}

uint32_t ParseAccentRaw(std::string_view raw) {
  SkipRawWs(raw);
  if (raw.empty()) {
    return 0;
  }
  if (raw.front() == '"') {
    raw.remove_prefix(1);
    const size_t end = raw.find('"');
    if (end == std::string_view::npos) {
      return 0;
    }
    return ParseHexRgb(raw.substr(0, end));
  }
  char* end = nullptr;
  const unsigned long value = std::strtoul(raw.data(), &end, 10);
  if (end == raw.data()) {
    return 0;
  }
  return static_cast<uint32_t>(value);
}

StatusState ParseState(std::string_view s) {
  if (s == "warn") {
    return StatusState::kWarn;
  }
  if (s == "error") {
    return StatusState::kError;
  }
  if (s == "on") {
    return StatusState::kOn;
  }
  if (s == "off") {
    return StatusState::kOff;
  }
  if (s == "busy") {
    return StatusState::kBusy;
  }
  return StatusState::kNormal;
}

StatusIcon ParseIconV2(std::string_view raw) {
  StatusIcon icon;
  const auto kind = json::GetString(raw, "kind");
  if (!kind) {
    return icon;
  }
  if (*kind == "glyph") {
    if (const auto glyph = json::GetString(raw, "glyph")) {
      icon.kind = IconKind::kGlyph;
      icon.glyph = TruncateWide(Utf8ToWide(*glyph), kStatusGlyphMaxChars);
    }
  } else if (*kind == "png") {
    if (const auto data = json::GetString(raw, "data")) {
      std::vector<uint8_t> bytes;
      if (!DecodeBase64(*data, &bytes) || bytes.empty()) {
        // omit
      } else if (bytes.size() > kStatusIconPngMaxBytes) {
        Log(L"status", L"png icon rejected size=%zu", bytes.size());
      } else {
        icon.kind = IconKind::kPng;
        icon.bytes = std::move(bytes);
      }
    }
  } else if (*kind == "file") {
    if (const auto path = json::GetString(raw, "path")) {
      std::wstring wide = Utf8ToWide(*path);
      if (wide.size() > 4096) {
        wide.resize(4096);
      }
      auto ascii_eq = [](wchar_t a, wchar_t b) {
        if (a >= L'A' && a <= L'Z') {
          a = static_cast<wchar_t>(a - L'A' + L'a');
        }
        return a == b;
      };
      const bool png = wide.size() >= 4 && ascii_eq(wide[wide.size() - 4], L'.') &&
                       ascii_eq(wide[wide.size() - 3], L'p') && ascii_eq(wide[wide.size() - 2], L'n') &&
                       ascii_eq(wide[wide.size() - 1], L'g');
      const bool ico = wide.size() >= 4 && ascii_eq(wide[wide.size() - 4], L'.') &&
                       ascii_eq(wide[wide.size() - 3], L'i') && ascii_eq(wide[wide.size() - 2], L'c') &&
                       ascii_eq(wide[wide.size() - 1], L'o');
      if ((png || ico) && !wide.empty()) {
        icon.kind = IconKind::kFile;
        icon.path = std::move(wide);
      }
    }
  }
  if (icon.kind != IconKind::kNone) {
    icon.cache_key = HashStatusIcon(icon);
  }
  return icon;
}

void ParseRowsV2(std::string_view raw, std::vector<StatusRow>* rows) {
  json::ForEachArray(raw, [&](std::string_view one) {
    if (rows->size() >= kStatusRowMax) {
      return true;
    }
    const auto type = json::GetString(one, "type");
    StatusRow row;
    if (!type) {
      return true;
    }
    if (*type == "gauge") {
      row.type = RowType::kGauge;
    } else if (*type == "kv") {
      row.type = RowType::kKeyValue;
    } else if (*type == "text") {
      row.type = RowType::kText;
    } else if (*type == "separator") {
      row.type = RowType::kSeparator;
    } else if (*type == "toggle") {
      row.type = RowType::kToggle;
    } else if (*type == "button") {
      row.type = RowType::kButton;
    } else {
      if (const auto fallback = json::GetString(one, "fallback_text")) {
        row.type = RowType::kText;
        row.fallback_text = TruncateWide(Utf8ToWide(*fallback), kStatusPanelTextMaxChars);
        row.label = row.fallback_text;
        rows->push_back(std::move(row));
      }
      return true;
    }
    if (const auto row_id = json::GetString(one, "row_id")) {
      row.row_id = row_id->size() > kMaxIdBytes ? row_id->substr(0, kMaxIdBytes) : *row_id;
    }
    if (const auto label = json::GetString(one, "label")) {
      row.label = TruncateWide(Utf8ToWide(*label), kStatusPanelTextMaxChars);
    }
    if (const auto value_text = json::GetString(one, "value_text")) {
      row.value_text = TruncateWide(Utf8ToWide(*value_text), kStatusPanelTextMaxChars);
    }
    if (row.type == RowType::kKeyValue) {
      if (const auto value = json::GetString(one, "value")) {
        row.value_text = TruncateWide(Utf8ToWide(*value), kStatusPanelTextMaxChars);
      }
    }
    if (const auto detail = json::GetString(one, "detail")) {
      row.detail = TruncateWide(Utf8ToWide(*detail), kStatusPanelTextMaxChars);
    }
    if (const auto note = json::GetString(one, "note")) {
      row.note = TruncateWide(Utf8ToWide(*note), kStatusPanelTextMaxChars);
    }
    if (row.type == RowType::kText) {
      if (const auto text = json::GetString(one, "text")) {
        row.label = TruncateWide(Utf8ToWide(*text), kStatusPanelTextMaxChars);
      }
    }
    if (const auto fallback = json::GetString(one, "fallback_text")) {
      row.fallback_text = TruncateWide(Utf8ToWide(*fallback), kStatusPanelTextMaxChars);
    }
    if (const auto value = json::GetDouble(one, "value")) {
      row.value = ClampUnit(*value);
    }
    if (const auto on = json::GetBool(one, "on")) {
      row.on = *on;
    }
    if (const auto style = json::GetString(one, "style")) {
      if (row.type == RowType::kButton) {
        row.danger = (*style == "danger");
      } else if (row.type == RowType::kText && *style == "note") {
        row.muted = true;
      }
    }
    rows->push_back(std::move(row));
    return true;
  });
}

void ApplySegmentV2(std::string_view raw, StatusItem* item) {
  if (const auto text = json::GetString(raw, "text")) {
    item->text = TruncateLabel(Utf8ToWide(*text));
  }
  if (const auto icon = json::GetRaw(raw, "icon")) {
    item->icon = ParseIconV2(*icon);
  }
  if (const auto tip = json::GetString(raw, "tooltip")) {
    item->tooltip = TruncateWide(Utf8ToWide(*tip), kStatusPanelTextMaxChars);
  }
  if (const auto state = json::GetString(raw, "state")) {
    item->state = ParseState(*state);
  }
  if (const auto accent = json::GetRaw(raw, "accent")) {
    item->accent = ParseAccentRaw(*accent);
  }
  if (const auto pri = json::GetInt(raw, "priority")) {
    item->priority = *pri;
  }
  if (const auto visible = json::GetBool(raw, "visible")) {
    item->visible = *visible;
  }
}

std::optional<StatusPanel> ParsePanelV2(std::string_view raw) {
  SkipRawWs(raw);
  if (raw.empty() || raw.front() != '{') {
    return std::nullopt;
  }
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
  if (const auto rows = json::GetRaw(raw, "rows")) {
    ParseRowsV2(*rows, &panel.rows);
  }
  if (panel.title.empty() && panel.rows.empty()) {
    return std::nullopt;
  }
  return panel;
}

void ApplyPanelPatch(std::string_view raw, StatusItem* item) {
  SkipRawWs(raw);
  if (raw.empty() || raw.front() == 'n') {
    item->panel.reset();
    return;
  }
  item->panel = ParsePanelV2(raw);
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
  int proto = 1;
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

const char* PipeServer::Name() const {
  return "pipe";
}

void PipeServer::SetActive(bool active) {
  active_ = active;
}

bool PipeServer::Start(StatusSink* sink) {
  Stop();
  sink_ = sink;
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
    sink_ = nullptr;
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
    owners_.clear();
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
  if (sink_ != nullptr) {
    sink_->Flush();
  }
}

void PipeServer::OnEvent(const StatusEvent& ev) {
  SendEvent(ev.id, ev.event, ev.row_id, ev.button, ev.on);
}

void PipeServer::SendEvent(const std::string& id, std::string_view event, std::string_view row_id,
                           std::string_view button, bool on) {
  uint64_t owner = 0;
  {
    std::lock_guard lock(mu_);
    const auto it = owners_.find(id);
    if (it == owners_.end()) {
      return;
    }
    owner = it->second;
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
  if (target->proto == 2) {
    std::string line = "{\"v\":2,\"op\":\"event\",\"id\":\"";
    line += json::Escape(id);
    line += "\",\"event\":\"";
    line += json::Escape(event);
    line += "\"";
    if (!row_id.empty()) {
      line += ",\"row_id\":\"";
      line += json::Escape(row_id);
      line += "\"";
    }
    if (!button.empty()) {
      line += ",\"button\":\"";
      line += json::Escape(button);
      line += "\"";
    }
    if (event == "toggle") {
      line += on ? ",\"on\":true" : ",\"on\":false";
    }
    line += "}";
    WriteLine(target, line);
    return;
  }
  if (event != "click" && event != "invoke") {
    return;
  }
  const std::string_view v1_button = !button.empty() ? button : row_id;
  std::string line = "{\"v\":1,\"op\":\"click\",\"id\":\"";
  line += json::Escape(id);
  line += "\",\"button\":\"";
  line += json::Escape(v1_button);
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
  WriteLine(client,
            "{\"v\":2,\"op\":\"hello\",\"renderer\":\"bamti\",\"version\":\"1.2.0\",\"proto\":[1,2],"
            "\"features\":[\"icon_glyph\",\"icon_png\",\"gauge\",\"kv\",\"toggle\",\"button\",\"text\","
            "\"separator\",\"events\"],\"limits\":{\"segment_text\":32,\"panel_rows\":32,\"panel_text\":128,"
            "\"icon_png_bytes\":8192,\"upserts_per_sec\":10}}");

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
      bool got = false;
      for (;;) {
        const DWORD wait =
            WaitForMultipleObjects(2, waits, FALSE, sink_ != nullptr ? sink_->NotifyWaitTimeoutMs() : INFINITE);
        if (wait == WAIT_TIMEOUT) {
          if (sink_ != nullptr) {
            sink_->Flush();
          }
          continue;
        }
        if (wait != WAIT_OBJECT_0 + 1) {
          CancelIoEx(client->pipe, &ov);
          break;
        }
        if (!GetOverlappedResult(client->pipe, &ov, &read, FALSE) || read == 0) {
          break;
        }
        got = true;
        break;
      }
      if (!got) {
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
  std::vector<std::string> drop;
  {
    std::lock_guard lock(mu_);
    client->alive.store(false);
    CloseClientPipe(client);
    for (auto it = owners_.begin(); it != owners_.end();) {
      if (it->second == owner) {
        drop.push_back(it->first);
        it = owners_.erase(it);
      } else {
        ++it;
      }
    }
  }
  for (const std::string& id : drop) {
    if (sink_ != nullptr) {
      sink_->Remove(id);
    }
  }
}

void PipeServer::HandleLine(Client* client, std::string_view line) {
  client->last_tick.store(NowMs());
  const auto ver = json::GetInt(line, "v");
  if (!ver) {
    return;
  }
  if (*ver == 1) {
    return HandleV1(client, line);
  }
  if (*ver == 2) {
    return HandleV2(client, line);
  }
}

void PipeServer::HandleV1(Client* client, std::string_view line) {
  client->proto = 1;
  const auto op = json::GetString(line, "op");
  if (!op) {
    return;
  }
  if (*op == "ping") {
    return;
  }

  if (*op == "remove") {
    const auto id = json::GetString(line, "id");
    if (!id || !ValidId(*id)) {
      return;
    }
    PublishRemove(*id, client->id);
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
    {
      std::lock_guard lock(mu_);
      if (!AllowUpsertLocked(item.id, client->id)) {
        return;
      }
    }
    PublishUpsert(std::move(item), client->id);
  }
}

void PipeServer::HandleV2(Client* client, std::string_view line) {
  client->proto = 2;
  const auto op = json::GetString(line, "op");
  if (!op || *op == "ping" || *op == "hello") {
    return;
  }

  if (*op == "remove") {
    const auto id = json::GetString(line, "id");
    if (!id || !ValidId(*id)) {
      return;
    }
    PublishRemove(*id, client->id);
  } else if (*op == "upsert") {
    const auto id = json::GetString(line, "id");
    if (!id || !ValidId(*id)) {
      return;
    }
    StatusItem item;
    item.id = *id;
    item.source = "pipe";
    if (const auto segment = json::GetRaw(line, "segment")) {
      ApplySegmentV2(*segment, &item);
    }
    if (item.icon.kind == IconKind::kNone && item.text.empty()) {
      return;
    }
    if (const auto panel = json::GetRaw(line, "panel")) {
      ApplyPanelPatch(*panel, &item);
    }
    {
      std::lock_guard lock(mu_);
      if (!AllowUpsertLocked(item.id, client->id)) {
        return;
      }
    }
    PublishUpsert(std::move(item), client->id);
  } else if (*op == "patch") {
    const auto id = json::GetString(line, "id");
    if (!id || !ValidId(*id) || sink_ == nullptr) {
      return;
    }
    {
      std::lock_guard lock(mu_);
      const auto it = owners_.find(*id);
      if (it == owners_.end() || it->second != client->id) {
        return;
      }
    }
    auto prev = sink_->Get(*id);
    if (!prev) {
      return;
    }
    if (const auto segment = json::GetRaw(line, "segment")) {
      ApplySegmentV2(*segment, &*prev);
    }
    if (const auto panel = json::GetRaw(line, "panel")) {
      ApplyPanelPatch(*panel, &*prev);
    }
    PublishUpsert(std::move(*prev), client->id);
  }
}

bool PipeServer::AllowUpsertLocked(const std::string& id, uint64_t owner) {
  const auto it = owners_.find(id);
  if (it != owners_.end()) {
    return it->second == owner;
  }
  if (owners_.size() >= kStatusItemsMax) {
    if (!logged_total_limit_) {
      logged_total_limit_ = true;
      Log(L"status", L"total item limit %zu reached; dropping upsert", kStatusItemsMax);
    }
    return false;
  }
  size_t owned = 0;
  for (const auto& pair : owners_) {
    if (pair.second == owner) {
      ++owned;
    }
  }
  if (owned >= kStatusItemsPerClient) {
    if (!logged_client_limit_) {
      logged_client_limit_ = true;
      Log(L"status", L"per-client item limit %zu reached; dropping upsert", kStatusItemsPerClient);
    }
    return false;
  }
  return true;
}

void PipeServer::PublishUpsert(StatusItem item, uint64_t owner) {
  if (sink_ == nullptr) {
    return;
  }
  if (const auto prev = sink_->Get(item.id)) {
    item.revision = prev->revision + 1;
  } else {
    item.revision = 1;
  }
  {
    std::lock_guard lock(mu_);
    owners_[item.id] = owner;
  }
  sink_->Upsert(std::move(item));
}

void PipeServer::PublishRemove(const std::string& id, uint64_t owner) {
  {
    std::lock_guard lock(mu_);
    const auto it = owners_.find(id);
    if (it == owners_.end() || it->second != owner) {
      return;
    }
    owners_.erase(it);
  }
  if (sink_ != nullptr) {
    sink_->Remove(id);
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
