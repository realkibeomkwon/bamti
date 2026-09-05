#pragma once

namespace bamti {

struct NightLightState {
  bool known = false;
  bool on = false;
};

NightLightState QueryNightLight();
// 성공하면 참을 돌려준다. 블롭 형식이 예상과 다르면 아무것도 쓰지 않고 거짓을 돌려준다.
bool SetNightLight(bool on);

}  // namespace bamti
