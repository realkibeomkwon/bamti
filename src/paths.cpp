#include "paths.hpp"

#include <windows.h>
#include <knownfolders.h>
#include <shlobj.h>

#include <mutex>

namespace bamti {
namespace {

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
  PWSTR root = nullptr;
  if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &root)) || root == nullptr) {
    return {};
  }
  std::wstring dir = root;
  CoTaskMemFree(root);
  return dir;
}

std::wstring LegacyDataDir() {
  const std::wstring local = KnownFolder(FOLDERID_LocalAppData);
  if (local.empty()) {
    return {};
  }
  return JoinPath(local, L"bamti");
}

void MigrateFile(const std::wstring& from, const std::wstring& to) {
  if (from.empty() || to.empty() || from == to) {
    return;
  }
  if (GetFileAttributesW(to.c_str()) != INVALID_FILE_ATTRIBUTES) {
    return;
  }
  if (GetFileAttributesW(from.c_str()) == INVALID_FILE_ATTRIBUTES) {
    return;
  }
  MoveFileW(from.c_str(), to.c_str());
}

}  // namespace

std::wstring JoinPath(const std::wstring& dir, const wchar_t* file) {
  std::wstring path = dir;
  if (!path.empty() && path.back() != L'\\') {
    path.push_back(L'\\');
  }
  path += file;
  return path;
}

std::wstring DataDir() {
  static std::once_flag once;
  static std::wstring dir;
  std::call_once(once, [] {
    std::wstring profile = KnownFolder(FOLDERID_Profile);
    if (profile.empty()) {
      wchar_t env[MAX_PATH]{};
      const DWORD n = GetEnvironmentVariableW(L"USERPROFILE", env, MAX_PATH);
      if (n > 0 && n < MAX_PATH) {
        profile = env;
      }
    }
    if (profile.empty()) {
      return;
    }
    dir = JoinPath(profile, L".bamti");
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring legacy = LegacyDataDir();
    if (!legacy.empty()) {
      MigrateFile(JoinPath(legacy, L"dock-pins.txt"), JoinPath(dir, L"dock-pins.txt"));
      MigrateFile(JoinPath(legacy, L"taskbar.guard"), JoinPath(dir, L"taskbar.guard"));
    }
  });
  return dir;
}

std::wstring DockPinsPath() {
  const std::wstring dir = DataDir();
  if (dir.empty()) {
    return {};
  }
  return JoinPath(dir, L"dock-pins.txt");
}

std::wstring TaskbarGuardPath() {
  const std::wstring dir = DataDir();
  if (dir.empty()) {
    return {};
  }
  return JoinPath(dir, L"taskbar.guard");
}

std::wstring LogFilePath() {
  const std::wstring dir = DataDir();
  if (dir.empty()) {
    return {};
  }
  return JoinPath(dir, L"bamti.log");
}

}  // namespace bamti
