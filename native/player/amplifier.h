// The Y1's own amplifier (Settings › Sound › Amplifier): the analog gain
// of the headphone buffer, which also feeds the speaker's amplifier. MediaTek's HAL sets it from the
// "ver1" volume table in NVRAM (/data/nvram/APCFG/APRDCL/Audio_ver1_Vol_custom): for media, byte
// 546 (wired headphones) and byte 552 (the speaker), a gain index of (152 - byte) / 4 at full master
// volume, written as register 0x70a's 7 - index / 2 (3 bits, 7 the loudest; AudioMTKVolumeController,
// AudioMachineDevice::SetHeadPhoneGain). The Y1 ships 124 and 118 (registers 4 and 3); MediaTek's
// reference design uses 148 and 144 (7 and 6). Four levels: as shipped, then one, two and three
// register steps up (about 2 dB each). The top one is loud but raises the hiss and can distort with
// sensitive headphones. Changing them goes through the HAL's own SetAudioData(0x101),
// which writes NVRAM and applies them at once; the table as it was is kept first, so level 0 puts the
// shipped values back.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace rondo::amp {

constexpr int kHeadset = 546, kSpeaker = 552;
constexpr int kLevels = 4;
// each level's bytes: registers 4..7 for headphones, 3..6 for the speaker
constexpr uint8_t kHeadsetAt[kLevels] = {124, 132, 140, 148};
constexpr uint8_t kSpeakerAt[kLevels] = {118, 126, 134, 144};

// the volume table now (GetAudioData 0x100); false when the HAL won't say
bool read(std::vector<uint8_t>& table);
int level(const std::vector<uint8_t>& table);  // 0..3, by the headphones' register
// a level; backup: where the table as shipped is kept (written once, before the first change)
bool set(int level, const std::string& backup, std::string& why);

}  // namespace rondo::amp
