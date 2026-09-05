#pragma once

#include <windows.h>
#include <bluetoothapis.h>

#include <string>
#include <vector>

namespace bamti {

struct BtRadioState {
  bool present = false;
  bool on = false;
  bool can_toggle = false;
};

struct BtDeviceInfo {
  std::wstring name;
  std::wstring address;
  bool connected = false;
  bool paired = false;
  int battery = -1;
  BLUETOOTH_DEVICE_INFO raw{};
};

BtRadioState QueryBtRadio();
bool SetBtRadio(bool on);
std::vector<BtDeviceInfo> EnumBtDevices();
// 주변을 실제로 훑는다. 5초 안팎이 걸리므로 UI 스레드에서 부르면 안 된다.
// 이미 짝지어졌거나 기억된 장치는 결과에서 뺀다.
std::vector<BtDeviceInfo> ScanBtDevices();
const wchar_t* BtClassGlyph(ULONG class_of_device);

}  // namespace bamti
