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
const wchar_t* BtClassGlyph(ULONG class_of_device);

}  // namespace bamti
