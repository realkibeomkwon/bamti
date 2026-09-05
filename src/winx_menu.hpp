#pragma once

#include <string>
#include <vector>

namespace bamti {

struct WinXEntry {
  std::wstring label;
  std::wstring lnk_path;    // 비어 있으면 아래 action이 쓰인다
  int group = 0;            // 3, 2, 1
  bool admin = false;
};

// Group3 → Group2 → Group1 순으로, 그룹 안에서는 파일 이름 내림차순.
// 실패하면 빈 벡터를 돌려준다.
std::vector<WinXEntry> LoadWinXEntries();

enum class PowerAction { kLogoff, kSleep, kHibernate, kShutdown, kRestart };

bool HibernateAvailable();
void InvokePowerAction(PowerAction action);

// 바로 가기를 그대로 실행한다.
void LaunchWinXEntry(const WinXEntry& entry);

}  // namespace bamti
