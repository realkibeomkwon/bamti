#pragma once

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace bamti::json {
namespace {

inline void SkipWs(std::string_view& s) {
  while (!s.empty() && static_cast<unsigned char>(s.front()) <= ' ') {
    s.remove_prefix(1);
  }
}

inline bool HexNibble(char c, uint32_t& out) {
  if (c >= '0' && c <= '9') {
    out = static_cast<uint32_t>(c - '0');
    return true;
  }
  if (c >= 'a' && c <= 'f') {
    out = static_cast<uint32_t>(c - 'a' + 10);
    return true;
  }
  if (c >= 'A' && c <= 'F') {
    out = static_cast<uint32_t>(c - 'A' + 10);
    return true;
  }
  return false;
}

inline void AppendUtf8(std::string& out, uint32_t cp) {
  if (cp < 0x80) {
    out.push_back(static_cast<char>(cp));
  } else if (cp < 0x800) {
    out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else if (cp < 0x10000) {
    out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  } else {
    out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
    out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
  }
}

inline std::optional<std::string> ParseString(std::string_view& s) {
  SkipWs(s);
  if (s.empty() || s.front() != '"') {
    return std::nullopt;
  }
  s.remove_prefix(1);
  std::string out;
  while (!s.empty()) {
    const char c = s.front();
    s.remove_prefix(1);
    if (c == '"') {
      return out;
    }
    if (c == '\\') {
      if (s.empty()) {
        return std::nullopt;
      }
      const char e = s.front();
      s.remove_prefix(1);
      switch (e) {
        case '"':
        case '\\':
        case '/':
          out.push_back(e);
          break;
        case 'b':
          out.push_back('\b');
          break;
        case 'f':
          out.push_back('\f');
          break;
        case 'n':
          out.push_back('\n');
          break;
        case 'r':
          out.push_back('\r');
          break;
        case 't':
          out.push_back('\t');
          break;
        case 'u': {
          if (s.size() < 4) {
            return std::nullopt;
          }
          uint32_t cp = 0;
          for (int i = 0; i < 4; ++i) {
            uint32_t nibble = 0;
            if (!HexNibble(s[static_cast<size_t>(i)], nibble)) {
              return std::nullopt;
            }
            cp = (cp << 4) | nibble;
          }
          s.remove_prefix(4);
          AppendUtf8(out, cp);
          break;
        }
        default:
          return std::nullopt;
      }
    } else {
      out.push_back(c);
    }
  }
  return std::nullopt;
}

inline bool SkipValue(std::string_view& s);

inline bool SkipObjectOrArray(std::string_view& s, char open, char close) {
  SkipWs(s);
  if (s.empty() || s.front() != open) {
    return false;
  }
  s.remove_prefix(1);
  int depth = 1;
  bool in_string = false;
  bool escape = false;
  while (!s.empty() && depth > 0) {
    const char c = s.front();
    s.remove_prefix(1);
    if (in_string) {
      if (escape) {
        escape = false;
      } else if (c == '\\') {
        escape = true;
      } else if (c == '"') {
        in_string = false;
      }
      continue;
    }
    if (c == '"') {
      in_string = true;
    } else if (c == open) {
      ++depth;
    } else if (c == close) {
      --depth;
    }
  }
  return depth == 0;
}

inline bool SkipValue(std::string_view& s) {
  SkipWs(s);
  if (s.empty()) {
    return false;
  }
  const char c = s.front();
  if (c == '"') {
    return ParseString(s).has_value();
  }
  if (c == '{') {
    return SkipObjectOrArray(s, '{', '}');
  }
  if (c == '[') {
    return SkipObjectOrArray(s, '[', ']');
  }
  if (c == '-' || (c >= '0' && c <= '9')) {
    if (c == '-') {
      s.remove_prefix(1);
    }
    while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.front()))) {
      s.remove_prefix(1);
    }
    if (!s.empty() && s.front() == '.') {
      s.remove_prefix(1);
      while (!s.empty() && std::isdigit(static_cast<unsigned char>(s.front()))) {
        s.remove_prefix(1);
      }
    }
    return true;
  }
  auto consume_lit = [&](std::string_view lit) {
    if (s.size() >= lit.size() && s.substr(0, lit.size()) == lit) {
      s.remove_prefix(lit.size());
      return true;
    }
    return false;
  };
  return consume_lit("true") || consume_lit("false") || consume_lit("null");
}

inline bool ForEachField(std::string_view json,
                         const auto& fn) {
  SkipWs(json);
  if (json.empty() || json.front() != '{') {
    return false;
  }
  json.remove_prefix(1);
  for (;;) {
    SkipWs(json);
    if (json.empty()) {
      return false;
    }
    if (json.front() == '}') {
      return true;
    }
    auto key = ParseString(json);
    if (!key) {
      return false;
    }
    SkipWs(json);
    if (json.empty() || json.front() != ':') {
      return false;
    }
    json.remove_prefix(1);
    SkipWs(json);
    std::string_view value = json;
    if (!SkipValue(json)) {
      return false;
    }
    const size_t n = static_cast<size_t>(json.data() - value.data());
    if (!fn(*key, value.substr(0, n))) {
      return false;
    }
    SkipWs(json);
    if (!json.empty() && json.front() == ',') {
      json.remove_prefix(1);
      continue;
    }
    SkipWs(json);
    return !json.empty() && json.front() == '}';
  }
}

}  // namespace

inline std::optional<std::string> GetString(std::string_view json, std::string_view key) {
  std::optional<std::string> found;
  const bool ok = ForEachField(json, [&](const std::string& k, std::string_view raw) {
    if (k == key) {
      std::string_view cur = raw;
      found = ParseString(cur);
    }
    return true;
  });
  if (!ok) {
    return std::nullopt;
  }
  return found;
}

inline std::optional<int> GetInt(std::string_view json, std::string_view key) {
  std::optional<int> found;
  const bool ok = ForEachField(json, [&](const std::string& k, std::string_view raw) {
    if (k == key) {
      std::string_view cur = raw;
      SkipWs(cur);
      if (cur.empty()) {
        return true;
      }
      char* end = nullptr;
      const long value = std::strtol(cur.data(), &end, 10);
      if (end != cur.data()) {
        found = static_cast<int>(value);
      }
    }
    return true;
  });
  if (!ok) {
    return std::nullopt;
  }
  return found;
}

inline std::optional<std::string_view> GetRaw(std::string_view json, std::string_view key) {
  std::optional<std::string_view> found;
  const bool ok = ForEachField(json, [&](const std::string& k, std::string_view raw) {
    if (k == key) {
      found = raw;
    }
    return true;
  });
  if (!ok) {
    return std::nullopt;
  }
  return found;
}

inline std::optional<double> GetDouble(std::string_view json, std::string_view key) {
  std::optional<double> found;
  const bool ok = ForEachField(json, [&](const std::string& k, std::string_view raw) {
    if (k == key) {
      std::string_view cur = raw;
      SkipWs(cur);
      if (cur.empty()) {
        return true;
      }
      char* end = nullptr;
      const double value = std::strtod(cur.data(), &end);
      if (end != cur.data()) {
        found = value;
      }
    }
    return true;
  });
  if (!ok) {
    return std::nullopt;
  }
  return found;
}

inline std::optional<uint32_t> GetUint32(std::string_view json, std::string_view key) {
  std::optional<uint32_t> found;
  const bool ok = ForEachField(json, [&](const std::string& k, std::string_view raw) {
    if (k == key) {
      std::string_view cur = raw;
      SkipWs(cur);
      if (cur.empty()) {
        return true;
      }
      char* end = nullptr;
      const unsigned long value = std::strtoul(cur.data(), &end, 10);
      if (end != cur.data()) {
        found = static_cast<uint32_t>(value);
      }
    }
    return true;
  });
  if (!ok) {
    return std::nullopt;
  }
  return found;
}

inline bool ForEachArray(std::string_view json, const auto& fn) {
  SkipWs(json);
  if (json.empty() || json.front() != '[') {
    return false;
  }
  json.remove_prefix(1);
  SkipWs(json);
  if (!json.empty() && json.front() == ']') {
    return true;
  }
  for (;;) {
    SkipWs(json);
    if (json.empty()) {
      return false;
    }
    std::string_view value = json;
    if (!SkipValue(json)) {
      return false;
    }
    const size_t n = static_cast<size_t>(json.data() - value.data());
    if (!fn(value.substr(0, n))) {
      return false;
    }
    SkipWs(json);
    if (!json.empty() && json.front() == ',') {
      json.remove_prefix(1);
      continue;
    }
    SkipWs(json);
    return !json.empty() && json.front() == ']';
  }
}

inline std::vector<std::string> GetStringArray(std::string_view json, std::string_view key) {
  std::vector<std::string> out;
  const auto raw = GetRaw(json, key);
  if (!raw) {
    return out;
  }
  ForEachArray(*raw, [&](std::string_view one) {
    std::string_view cur = one;
    SkipWs(cur);
    if (const auto s = ParseString(cur)) {
      out.push_back(*s);
    }
    return true;
  });
  return out;
}

inline std::string Escape(std::string_view s) {
  std::string out;
  out.reserve(s.size() + 8);
  for (const unsigned char c : s) {
    switch (c) {
      case '"':
        out += "\\\"";
        break;
      case '\\':
        out += "\\\\";
        break;
      case '\n':
        out += "\\n";
        break;
      case '\r':
        out += "\\r";
        break;
      case '\t':
        out += "\\t";
        break;
      default:
        if (c < 0x20) {
          char buf[8];
          std::snprintf(buf, sizeof(buf), "\\u%04x", c);
          out += buf;
        } else {
          out.push_back(static_cast<char>(c));
        }
        break;
    }
  }
  return out;
}

}  // namespace bamti::json
