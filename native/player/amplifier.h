// The Y1's own amplifier (Settings › Sound › Amplifier, Emin's go-ahead of Nov 8): the analog gain
// of the headphone buffer, which also feeds the speaker's amplifier. MediaTek's HAL sets it from the
// "ver1" volume table in NVRAM (/data/nvram/APCFG/APRDCL/Audio_ver1_Vol_custom): for media, byte
// 546 (wired headphones) and byte 552 (the speaker), a gain index of (152 - byte) / 4 at full master
// volume, written as register 0x70a's 7 - index / 2 (3 bits, 7 the loudest; AudioMTKVolumeController,
// AudioMachineDevice::SetHeadPhoneGain). The Y1 ships 124 and 118 (registers 4 and 3); MediaTek's
// reference design uses 148 and 144 (7 and 6): three steps louder, about 6 dB. Changing them goes
// through the HAL's own SetAudioData(0x101), which writes NVRAM and applies them at once; the table as
// it was is kept first, so Standard puts the shipped values back.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace rondo::amp {

constexpr int kHeadset = 546, kSpeaker = 552;
constexpr uint8_t kLouderHeadset = 148, kLouderSpeaker = 144;

// the volume table now (GetAudioData 0x100); false when the HAL won't say
bool read(std::vector<uint8_t>& table);
bool louder(const std::vector<uint8_t>& table);
// Louder or Standard; backup: where the table as shipped is kept (written once, before the first change)
bool set(bool louder, const std::string& backup, std::string& why);

}  // namespace rondo::amp
