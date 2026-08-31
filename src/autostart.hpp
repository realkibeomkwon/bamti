#pragma once

namespace bamti {

// HKCU\Software\Microsoft\Windows\CurrentVersion\Run 의 "bamti" 값이 있고,
// StartupApproved 로 꺼져 있지 않으면 true 를 돌려준다.
bool AutostartEnabled();

// 값을 만들거나 지운다. 성공하면 true.
bool SetAutostart(bool on);

}  // namespace bamti
