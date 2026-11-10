// The microSD card as a USB drive for a computer (UsbDrive in 1.0.x): how music goes onto a Y1 running
// Rondo. Android's MountService did this for 1.0.x; Rondo talks to vold itself. While the card is
// shared, the computer has all of it: the library and anything playing from the card wait.
#pragma once

namespace rondo {

namespace usb {

// A computer has the Y1 on the cable (the USB gadget is configured), not just a charger.
bool computerConnected();
// The card is the computer's (vold's Shared state).
bool shared();
// A thread that mounts whatever vold lists as idle (a card put in while running, the boot's mounts
// still to come), as Android's MountService did: when vold says a volume became idle, except while
// setShared hands the card over (it passes through idle then).
// vold answers one command at a time, a mount's check included, so never from the drawing thread.
void watchCard();
// Hands the card to the computer or takes it back, waiting until it has changed hands (up to 15 s
// each way). Blocks: call off the drawing thread. Whatever still has a file open on the card when it
// is unmounted is killed by vold after several seconds, so close them first.
bool setShared(bool on);

}  // namespace usb

}  // namespace rondo
