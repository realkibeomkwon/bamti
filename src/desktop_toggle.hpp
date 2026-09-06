#pragma once

#include <windows.h>

#include <vector>

namespace bamti {

// Ctrl + 코너 호버로 부르는 바탕 화면 전환.
//
// 상태를 기억하지 않는다. 전환할 때마다 지금 화면에 무엇이 떠 있는지
// 세어서 방향을 정한다. 셸의 MinimizeAll 은 성공을 돌려주고도 아무 일도
// 하지 않는 경우가 있어서 쓰지 않는다.
class DesktopToggle {
 public:
  // 지금 눌러야 할 방향으로 한 번 전환한다.
  // 드러나 있으면 되살리고, 창이 떠 있으면 감춘다.
  void Toggle();

  // 창이 하나도 떠 있지 않은가.
  static bool Revealed();

  // 꺼 놓은 창의 효과를 모두 되돌린다. 여러 번 불러도 안전하다.
  void RestoreTransitions();

  ~DesktopToggle();

 private:
  void Conceal(std::vector<HWND> windows);
  void Reveal();
  // 창 하나의 DWM 전환 효과를 끄거나 되돌린다.
  // 다른 프로세스의 창에도 걸린다. 실패는 묶음 로그의 fail 로 센다.
  HRESULT SetTransitions(HWND hwnd, bool enabled);
  void DisableTransitions(const std::vector<HWND>& windows);

  // Conceal 이 최소화한 창. Z 순서 앞에서 뒤 순서로 담는다.
  std::vector<HWND> concealed_;
  // 우리가 전환 효과를 꺼 놓은 창. 되돌리면 비운다.
  std::vector<HWND> transitions_off_;
};

}  // namespace bamti
