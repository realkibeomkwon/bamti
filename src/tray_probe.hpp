#pragma once

namespace bamti {

// 알림 영역 구조를 측정해 보고서를 파일로 남긴다. 시스템 상태를 바꾸지 않는다.
// 반환값은 프로세스 종료 코드다. 0은 보고서 생성 성공을 뜻하며, 측정 결과의 좋고 나쁨과는 무관하다.
int RunTrayProbe();

// kParkedVisible 주차를 흉내 내 PrintWindow가 XAML 알림 영역을 그리는지 측정한다.
// 태스크바를 잠시 화면 밖으로 옮긴 뒤 반드시 되돌린다.
int RunTrayProbeParked();

}  // namespace bamti
