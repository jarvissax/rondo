// A QR code for a short text, as 1.0.x's ZXing QRCodeWriter made it for the dashboard's address: byte
// mode (ISO-8859-1, no ECI), error correction L, the smallest version that fits (1-9 here), and the
// mask ZXing's penalty rules pick. Pixels as QRCodeWriter.renderResult() lays them out: whole modules
// of `size / modules` pixels, centered, no quiet zone.
#pragma once
#include <stdint.h>

#include <string>
#include <vector>

namespace rondo::ui {

struct QrCode {
  int modules = 0;            // 21 for version 1, 4 more a version
  std::vector<uint8_t> dark;  // modules x modules, 1 = dark
};

// false when the text doesn't fit a version up to 6 (106 bytes)
bool encodeQr(const std::string& text, QrCode& out);
// size x size, one byte a pixel: 0 dark, 255 light
std::vector<uint8_t> renderQr(const QrCode& qr, int size);

}  // namespace rondo::ui
