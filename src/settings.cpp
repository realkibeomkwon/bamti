#include "settings.hpp"

#include "json_line.hpp"
#include "log.hpp"
#include "paths.hpp"

#include <windows.h>

#include <string>
#include <string_view>

namespace bamti::json {

bool ExtraFields(std::string_view obj, std::string_view skip, std::string_view prefix, std::string* extra) {
  if (extra == nullptr) {
    return false;
  }
  extra->clear();
  return ForEachField(obj, [&](const std::string& k, std::string_view raw) {
    if (k == skip) {
      return true;
    }
    extra->append(prefix.data(), prefix.size());
    extra->push_back('"');
    extra->append(Escape(k));
    extra->append("\": ");
    extra->append(raw.data(), raw.size());
    return true;
  });
}

bool ParsePreserve(std::string_view text, std::string* extra_topbar, std::string* extra_root) {
  if (extra_topbar == nullptr || extra_root == nullptr) {
    return false;
  }
  extra_topbar->clear();
  extra_root->clear();
  return ForEachField(text, [&](const std::string& k, std::string_view raw) {
    if (k == "topbar") {
      std::string_view inner = raw;
      SkipWs(inner);
      if (inner.empty() || inner.front() != '{') {
        return false;
      }
      return ExtraFields(raw, "widgets", ",\n    ", extra_topbar);
    }
    extra_root->append(",\n  \"");
    extra_root->append(Escape(k));
    extra_root->append("\": ");
    extra_root->append(raw.data(), raw.size());
    return true;
  });
}

}  // namespace bamti::json

namespace bamti {
namespace {

constexpr size_t kSettingsMaxBytes = 64 * 1024;

std::string ReadFileUtf8(const std::wstring& path, bool* too_large) {
  if (too_large != nullptr) {
    *too_large = false;
  }
  const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return {};
  }
  LARGE_INTEGER size{};
  if (!GetFileSizeEx(file, &size) || size.QuadPart < 0) {
    CloseHandle(file);
    return {};
  }
  if (static_cast<ULONGLONG>(size.QuadPart) > kSettingsMaxBytes) {
    if (too_large != nullptr) {
      *too_large = true;
    }
    CloseHandle(file);
    return {};
  }
  std::string text(static_cast<size_t>(size.QuadPart), '\0');
  DWORD read = 0;
  const BOOL ok =
      text.empty() || ReadFile(file, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
  CloseHandle(file);
  if (!ok) {
    return {};
  }
  text.resize(read);
  if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
      static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF) {
    text.erase(0, 3);
  }
  return text;
}

bool WriteFileUtf8Atomic(const std::wstring& path, const std::string& text) {
  const std::wstring tmp = path + L".tmp";
  const HANDLE file = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return false;
  }
  DWORD written = 0;
  const BOOL ok =
      text.empty() || WriteFile(file, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
  if (ok) {
    FlushFileBuffers(file);
  }
  CloseHandle(file);
  if (!ok || written != static_cast<DWORD>(text.size())) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  if (!MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
    DeleteFileW(tmp.c_str());
    return false;
  }
  return true;
}

std::string FormatSettings(const WidgetSettings& s, std::string_view extra_topbar, std::string_view extra_root) {
  std::string out = "{\n  \"topbar\": {\n    \"widgets\": {\"battery\": ";
  out += s.battery ? "true" : "false";
  out += ", \"cpu\": ";
  out += s.cpu ? "true" : "false";
  out += ", \"network\": ";
  out += s.network ? "true" : "false";
  out += ", \"widget_board_button\": ";
  out += s.widget_board ? "true" : "false";
  out += '}';
  out.append(extra_topbar.data(), extra_topbar.size());
  out += "\n  }";
  out.append(extra_root.data(), extra_root.size());
  out += "\n}\n";
  return out;
}

void BackupInvalidSettings(const std::wstring& path) {
  const std::wstring bak = path + L".bak";
  DeleteFileW(bak.c_str());
  if (MoveFileW(path.c_str(), bak.c_str())) {
    Log(L"settings", L"invalid settings.json moved to settings.json.bak");
    return;
  }
  if (CopyFileW(path.c_str(), bak.c_str(), FALSE)) {
    Log(L"settings", L"invalid settings.json copied to settings.json.bak");
  } else {
    Log(L"settings", L"could not back up invalid settings.json");
  }
}

}  // namespace

std::wstring SettingsPath() {
  const std::wstring dir = DataDir();
  if (dir.empty()) {
    return {};
  }
  return JoinPath(dir, L"settings.json");
}

WidgetSettings LoadWidgetSettings() {
  WidgetSettings s;
  const std::wstring path = SettingsPath();
  if (path.empty()) {
    return s;
  }
  bool too_large = false;
  const std::string text = ReadFileUtf8(path, &too_large);
  if (too_large) {
    static bool logged = false;
    if (!logged) {
      logged = true;
      Log(L"settings", L"settings.json larger than 64KB; using defaults");
    }
    return s;
  }
  if (text.empty()) {
    return s;
  }
  const auto topbar = json::GetRaw(text, "topbar");
  const auto widgets = topbar ? json::GetRaw(*topbar, "widgets") : std::nullopt;
  if (!widgets) {
    return s;
  }
  s.battery = json::GetBool(*widgets, "battery").value_or(false);
  s.cpu = json::GetBool(*widgets, "cpu").value_or(false);
  s.network = json::GetBool(*widgets, "network").value_or(false);
  if (const auto board = json::GetBool(*widgets, "widget_board_button")) {
    s.widget_board = *board;
  } else {
    s.widget_board = json::GetBool(*widgets, "widget_board").value_or(false);
  }
  return s;
}

bool SaveWidgetSettings(const WidgetSettings& s) {
  const std::wstring path = SettingsPath();
  if (path.empty()) {
    return false;
  }

  bool too_large = false;
  const std::string existing = ReadFileUtf8(path, &too_large);
  std::string extra_topbar;
  std::string extra_root;
  const bool file_exists = GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
  bool parsed = false;
  if (!too_large && !existing.empty()) {
    parsed = json::ParsePreserve(existing, &extra_topbar, &extra_root);
  }
  if (file_exists && !parsed && !too_large) {
    BackupInvalidSettings(path);
    extra_topbar.clear();
    extra_root.clear();
  }

  const std::string text = FormatSettings(s, extra_topbar, extra_root);
  if (!WriteFileUtf8Atomic(path, text)) {
    Log(L"settings", L"failed to write settings.json");
    return false;
  }
  return true;
}

}  // namespace bamti
