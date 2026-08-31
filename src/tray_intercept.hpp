#pragma once

#include "tray_backend.hpp"

namespace bamti {

std::unique_ptr<TrayBackend> MakeInterceptTrayBackend();

// 프로세스 진입점에서 부른다. 설정의 tray_backend 가 "intercept" 일 때만
// 스파이를 띄우고, 그 밖에는 아무 일도 하지 않는다.
void PrestartInterceptTrayBackend();

// 선기동한 인스턴스의 소유권을 넘긴다. 선기동하지 않았으면 nullptr.
std::unique_ptr<TrayBackend> TakePrestartedInterceptTrayBackend();

}  // namespace bamti
