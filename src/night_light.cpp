#include "night_light.hpp"

#include "log.hpp"
#include "paths.hpp"

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <ctime>
#include <vector>

namespace bamti {
namespace {

constexpr wchar_t kStateKey[] =
    L"Software\\Microsoft\\Windows\\CurrentVersion\\CloudStore\\Store\\DefaultAccount\\Current"
    L"\\default$windows.data.bluelightreduction.bluelightreductionstate"
    L"\\windows.data.bluelightreduction.bluelightreductionstate";
constexpr wchar_t kDataName[] = L"Data";
constexpr size_t kMinBlob = 35;
constexpr size_t kTsOff = 10;
constexpr BYTE kHdr[] = {0x43, 0x42, 0x01, 0x00};
constexpr BYTE kOnFlag[] = {0x10, 0x00};

struct ParsedBlob {
  bool ok = false;
  bool on = false;
  size_t ts_len = 0;
  size_t inner_len_off = 0;
  size_t flag_off = 0;
};

bool ReadVarint(const BYTE* p, size_t n, size_t* consumed, uint64_t* value) {
  if (p == nullptr || consumed == nullptr || value == nullptr) {
    return false;
  }
  uint64_t v = 0;
  size_t i = 0;
  while (i < n && i < 10) {
    const BYTE b = p[i];
    v |= static_cast<uint64_t>(b & 0x7f) << (7 * i);
    ++i;
    if ((b & 0x80) == 0) {
      *consumed = i;
      *value = v;
      return true;
    }
  }
  return false;
}

void AppendVarint(uint64_t v, std::vector<BYTE>* out) {
  while (v >= 0x80) {
    out->push_back(static_cast<BYTE>((v & 0x7f) | 0x80));
    v >>= 7;
  }
  out->push_back(static_cast<BYTE>(v));
}

ParsedBlob ParseBlob(const std::vector<BYTE>& data) {
  ParsedBlob parsed;
  if (data.size() < kMinBlob) {
    return parsed;
  }
  if (data[0] != kHdr[0] || data[1] != kHdr[1] || data[2] != kHdr[2] || data[3] != kHdr[3]) {
    return parsed;
  }
  if (kTsOff >= data.size()) {
    return parsed;
  }
  size_t ts_len = 0;
  uint64_t ts = 0;
  if (!ReadVarint(data.data() + kTsOff, data.size() - kTsOff, &ts_len, &ts)) {
    return parsed;
  }
  (void)ts;
  const size_t after_ts = kTsOff + ts_len;
  // 5바이트 varint이면 after_ts == 15. 지시서의 "오프셋 15의 2a" 검사와 같다.
  if (after_ts >= data.size() || data[after_ts] != 0x2a) {
    return parsed;
  }
  if (after_ts + 8 > data.size()) {
    return parsed;
  }
  if (data[after_ts + 1] != 0x2b || data[after_ts + 2] != 0x0e) {
    return parsed;
  }
  if (data[after_ts + 4] != kHdr[0] || data[after_ts + 5] != kHdr[1] || data[after_ts + 6] != kHdr[2] ||
      data[after_ts + 7] != kHdr[3]) {
    return parsed;
  }
  parsed.ts_len = ts_len;
  parsed.inner_len_off = after_ts + 3;
  parsed.flag_off = after_ts + 8;
  parsed.on = parsed.flag_off + 1 < data.size() && data[parsed.flag_off] == kOnFlag[0] &&
              data[parsed.flag_off + 1] == kOnFlag[1];
  parsed.ok = true;
  return parsed;
}

bool ReadData(std::vector<BYTE>* out) {
  if (out == nullptr) {
    return false;
  }
  out->clear();
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kStateKey, 0, KEY_QUERY_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  DWORD type = 0;
  DWORD size = 0;
  const LSTATUS probe = RegQueryValueExW(key, kDataName, nullptr, &type, nullptr, &size);
  if (probe != ERROR_SUCCESS || type != REG_BINARY || size == 0) {
    RegCloseKey(key);
    return false;
  }
  out->resize(size);
  DWORD read = size;
  const LSTATUS got = RegQueryValueExW(key, kDataName, nullptr, &type, out->data(), &read);
  RegCloseKey(key);
  if (got != ERROR_SUCCESS || type != REG_BINARY || read != size) {
    out->clear();
    return false;
  }
  return true;
}

bool WriteData(const std::vector<BYTE>& data) {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, kStateKey, 0, KEY_SET_VALUE, &key) != ERROR_SUCCESS) {
    return false;
  }
  const LSTATUS st =
      RegSetValueExW(key, kDataName, 0, REG_BINARY, data.data(), static_cast<DWORD>(data.size()));
  RegCloseKey(key);
  return st == ERROR_SUCCESS;
}

bool BackupOnce(const std::vector<BYTE>& data) {
  const std::wstring path = NightLightBackupPath();
  if (path.empty()) {
    return false;
  }
  if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) {
    return GetLastError() == ERROR_FILE_EXISTS;
  }
  DWORD written = 0;
  const BOOL ok = WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
  CloseHandle(file);
  return ok == TRUE && written == data.size();
}

}  // namespace

NightLightState QueryNightLight() {
  NightLightState state;
  std::vector<BYTE> data;
  if (!ReadData(&data)) {
    return state;
  }
  const ParsedBlob parsed = ParseBlob(data);
  if (!parsed.ok) {
    return state;
  }
  state.known = true;
  state.on = parsed.on;
  return state;
}

bool SetNightLight(bool on) {
  std::vector<BYTE> data;
  if (!ReadData(&data)) {
    Log(L"night", L"SetNightLight read failed");
    return false;
  }
  const ParsedBlob parsed = ParseBlob(data);
  if (!parsed.ok) {
    Log(L"night", L"SetNightLight unexpected blob size=%u", static_cast<unsigned>(data.size()));
    return false;
  }
  if (parsed.on == on) {
    return true;
  }
  if (!BackupOnce(data)) {
    Log(L"night", L"SetNightLight backup failed");
    return false;
  }
  std::vector<BYTE> next;
  next.insert(next.end(), data.begin(), data.begin() + static_cast<std::ptrdiff_t>(kTsOff));
  const std::time_t raw = std::time(nullptr);
  if (raw == static_cast<std::time_t>(-1)) {
    return false;
  }
  AppendVarint(static_cast<uint64_t>(raw), &next);
  std::vector<BYTE> tail(data.begin() + static_cast<std::ptrdiff_t>(kTsOff + parsed.ts_len), data.end());
  const size_t len_off = parsed.inner_len_off - (kTsOff + parsed.ts_len);
  const size_t flag_off = parsed.flag_off - (kTsOff + parsed.ts_len);
  if (len_off >= tail.size() || flag_off > tail.size()) {
    return false;
  }
  if (tail[len_off] >= 0x80) {
    return false;
  }
  if (on) {
    tail[len_off] = static_cast<BYTE>(tail[len_off] + 2);
    tail.insert(tail.begin() + static_cast<std::ptrdiff_t>(flag_off), kOnFlag, kOnFlag + 2);
  } else {
    if (flag_off + 1 >= tail.size() || tail[flag_off] != kOnFlag[0] || tail[flag_off + 1] != kOnFlag[1] ||
        tail[len_off] < 2) {
      return false;
    }
    tail[len_off] = static_cast<BYTE>(tail[len_off] - 2);
    tail.erase(tail.begin() + static_cast<std::ptrdiff_t>(flag_off),
               tail.begin() + static_cast<std::ptrdiff_t>(flag_off + 2));
  }
  next.insert(next.end(), tail.begin(), tail.end());
  const ParsedBlob check = ParseBlob(next);
  if (!check.ok || check.on != on) {
    Log(L"night", L"SetNightLight rebuilt blob failed check on=%d", on ? 1 : 0);
    return false;
  }
  if (!WriteData(next)) {
    Log(L"night", L"SetNightLight write failed");
    return false;
  }
  Log(L"night", L"SetNightLight on=%d size=%u", on ? 1 : 0, static_cast<unsigned>(next.size()));
  return true;
}

}  // namespace bamti
