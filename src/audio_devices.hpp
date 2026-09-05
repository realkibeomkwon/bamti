#pragma once

#include <string>
#include <vector>

namespace bamti {

enum class AudioForm { kUnknown, kSpeakers, kHeadphones, kHeadset, kDisplay, kDigital };

struct AudioEndpoint {
  std::wstring id;
  std::wstring name;
  AudioForm form = AudioForm::kUnknown;
  bool is_default = false;
  bool bluetooth = false;
};

std::vector<AudioEndpoint> EnumAudioOutputs();
bool SetDefaultAudioOutput(const std::wstring& device_id);
const wchar_t* AudioFormGlyph(AudioForm form);

}  // namespace bamti
