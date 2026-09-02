#pragma once

namespace bamti {

// 작업 스케줄러에 bamti 작업이 있고 사용 가능하면 true.
// 작업이 없으면 HKCU Run 값과 StartupApproved 로 판정한다.
bool AutostartEnabled();

// 작업을 만들거나 지운다. 작업 등록이 실패하면 Run 키로 폴백한다.
bool SetAutostart(bool on);

// Run 키로 걸려 있던 자동 시작을 작업 스케줄러로 옮긴다. 이미 옮겼거나 자동 시작이
// 꺼져 있으면 아무 일도 하지 않는다.
void AutostartMigrate();

}  // namespace bamti
