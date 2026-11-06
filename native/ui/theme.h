// The look, from the settings (core/config.h): the iPod-style shell in light or dark with the accent's
// selection bar, the YouTube sections' fixed dark colors, and the text scale (today's Theme.kt).
#pragma once
#include "../core/config.h"
#include "gfx.h"

namespace rondo::ui {

// The accent's lit top for the selection bar (Theme.kt lit(), shared with the dashboard).
Color lit(Color accent);

struct Palette {
  float scale = 1.4f;  // mockup units to pixels (the text scale)
  bool darkShell = false;
  // the shell
  Color shellBg, barTop, barBottom, barLine, rowLine, header, text, muted, disabled, chevron, icon, titleShadow;
  Color accent, accentTop;
  // YouTube and YouTube Music: always dark
  Color dark = rgb(0x0f0f0f), raised = rgb(0x232323), darkBar = rgb(0x181818), darkPanel = rgb(0x212121);
  Color darkPanelSelected = rgb(0x3a3a3a), darkMuted = rgb(0xaaaaaa), darkFaint = rgb(0x717171);
  Color darkTrack = rgb(0x4a4a4a), focus = rgb(0xf1f1f1), focusText = rgb(0x0f0f0f), focusMuted = rgb(0x4d4d4d);
  Color red = rgb(0xff0033), overlay = 0xb8000000, white = rgb(0xffffff);

  float u(float v) const { return v * scale; }
};

Palette paletteFor(const Theme& t);

// Wheel acceleration (Behavior.wheelAcceleration, today's WheelInput.kt): pulses less than 154 ms apart
// in one direction are a spin; a long spin moves several rows a pulse.
struct WheelSpin {
  int64_t last = 0;
  int dir = 0, spin = 0;
  // The rows this pulse moves (+ down, - up).
  int pulse(int direction, int64_t now, const std::string& acceleration) {
    spin = direction == dir && now - last < 154000000LL ? spin + 1 : 1;
    last = now;
    dir = direction;
    int steps = 1;
    if (acceleration == "fast") steps = spin >= 6 ? 3 : 1;
    else if (acceleration != "off") steps = spin >= 12 ? 2 : 1;
    return direction * steps;
  }
};

}  // namespace rondo::ui
