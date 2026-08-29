#pragma once

#include "tray_backend.hpp"

namespace bamti {

std::unique_ptr<TrayBackend> MakeInterceptTrayBackend();

}  // namespace bamti
