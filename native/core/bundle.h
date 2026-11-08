// Rondo's update bundle (private/native-plan.md, "Updates and packaging"): the binary, its libraries,
// fonts, tables and the dashboard's page, as one file, rondo-<version>.rondo:
//
//   "RONDOB1\n"   8 bytes
//   signature    64 bytes, Ed25519 over the SHA-256 of what follows (the key's public half is below;
//                its private half never leaves the release machine)
//   .tar.gz      the files, with VERSION holding the version
//
// An update unpacks into <data>/<version>; <data>/current names it, and /system/rondo/start runs it,
// falling back to the firmware's own copy after three crashes in a row.
#pragma once
#include <stdint.h>

#include <string>

namespace rondo::bundle {

// The bundle key's public half (tools/native/bundle.sh signs with the private one)
constexpr uint8_t kPublicKey[32] = {0x18, 0x09, 0x4e, 0xed, 0xd5, 0xb4, 0x27, 0x38, 0x00, 0x27, 0x36,
                                    0xa8, 0xad, 0xa5, 0x11, 0xf2, 0x1d, 0x46, 0x1a, 0x83, 0xa5, 0xde,
                                    0x77, 0x35, 0x38, 0x6a, 0x5c, 0xeb, 0x74, 0xe2, 0x8e, 0x86};

constexpr size_t kHeader = 8 + 64;

// Whether the file is a bundle signed with the key
bool verify(const std::string& file, std::string& why);
// Verified, unpacked into <dataDir>/<version> (its VERSION), and named in <dataDir>/current: Rondo
// runs it from its next start. Older versions are removed, but for the one that ran last.
bool install(const std::string& file, const std::string& dataDir, std::string& version, std::string& why);

}  // namespace rondo::bundle
