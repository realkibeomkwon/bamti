#include "log.hpp"

#include "paths.hpp"

#include <windows.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <mutex>
#include <share.h>

namespace bamti {
namespace {

constexpr long kMaxLogBytes = 2 * 1024 * 1024;

std::mutex g_lock;
std::atomic<int> g_log_depth{0};
FILE* g_file = nullptr;
std::wstring g_path;

void CloseFile() {
  if (g_file != nullptr) {
    fclose(g_file);
    g_file = nullptr;
  }
}

void OpenFile(bool append) {
  CloseFile();
  if (g_path.empty()) {
    return;
  }
  const wchar_t* mode = append ? L"a, ccs=UTF-8" : L"w, ccs=UTF-8";
  g_file = _wfsopen(g_path.c_str(), mode, _SH_DENYNO);
}

void RotateIfNeeded() {
  if (g_file == nullptr) {
    return;
  }
  const long pos = ftell(g_file);
  if (pos < kMaxLogBytes) {
    return;
  }
  CloseFile();
  const std::wstring bak = g_path + L".old";
  DeleteFileW(bak.c_str());
  MoveFileW(g_path.c_str(), bak.c_str());
  OpenFile(false);
}

void FormatLogLine(wchar_t (&line)[1280], const wchar_t* area, const wchar_t* fmt, va_list args) {
  wchar_t body[1024]{};
  _vsnwprintf_s(body, _TRUNCATE, fmt, args);
  SYSTEMTIME st{};
  GetLocalTime(&st);
  swprintf_s(line, L"%04u-%02u-%02u %02u:%02u:%02u.%03u [%s] %s\n", st.wYear, st.wMonth, st.wDay, st.wHour,
             st.wMinute, st.wSecond, st.wMilliseconds, area != nullptr ? area : L"", body);
}

void WriteLogFile(const wchar_t* line) {
  if (g_file == nullptr) {
    return;
  }
  RotateIfNeeded();
  if (g_file == nullptr) {
    return;
  }
  fputws(line, g_file);
  fflush(g_file);
}

}  // namespace

void LogInit() {
  std::lock_guard lock(g_lock);
  g_path = LogFilePath();
  OpenFile(true);
}

void LogShutdown() {
  std::lock_guard lock(g_lock);
  CloseFile();
  g_path.clear();
}

void Log(const wchar_t* area, const wchar_t* fmt, ...) {
  wchar_t line[1280]{};
  va_list args;
  va_start(args, fmt);
  FormatLogLine(line, area, fmt, args);
  va_end(args);

  OutputDebugStringW(line);

  std::lock_guard lock(g_lock);
  g_log_depth.fetch_add(1, std::memory_order_acq_rel);
  WriteLogFile(line);
  g_log_depth.fetch_sub(1, std::memory_order_acq_rel);
}

bool LogBusy() {
  return g_log_depth.load(std::memory_order_acquire) > 0;
}

}  // namespace bamti
