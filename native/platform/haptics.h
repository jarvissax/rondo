// The Y1's vibrator: what Android's Vibrator.vibrate(ms) did for 1.0.x's wheel buzz (WheelHaptics.kt).
#pragma once

namespace rondo::haptics {

void vibrate(int ms);  // on for ms; a new call replaces one still running

}  // namespace rondo::haptics
