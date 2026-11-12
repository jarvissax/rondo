# Installing Rondo

Rondo is a firmware for the Innioasis Y1: you install it the way you'd install Rockbox or the
Y1's original software, with **Innioasis Updater CE**. The Y1 then starts straight into Rondo.
After that, updates take a minute and keep everything (see [Updating](#updating-rondo)).

## Before you start

- **Your Y1 has to be a Type A.** That's a Y1 that shipped with Innioasis's own software 2.0.0 or
  later (not to be confused with Rondo's version); the Updater asks which type yours is. If yours
  came with software older than 2.0, or you bought it between 2024 and May 2025, treat it as a
  **Type B**: Rondo isn't for it yet, and a Type A firmware can leave a Type B unable to start.
- **Installing replaces everything on the Y1 itself**: the original player, Rockbox, settings.
  **Your microSD card isn't touched**, so your music stays where it is.
- **Charge the Y1** to at least half.
- Use a **USB-C cable that carries data**. Some charging cables don't, and nothing will work
  with them.

## Install with Innioasis Updater CE

1. Download **Innioasis Updater CE** for your computer from its
   [guide page](https://www.innioasis.app/guide.html), which shows the download and drivers for
   your system. On Windows, let it install its drivers and restart the computer once before the
   first install.
2. Open it, choose **Y1** and **Type A** (not sure? see [Before you start](#before-you-start)).
3. In the software list, choose **Rondo**, then **Install**.
4. Do what the Updater asks: switch the Y1 completely off, then plug it in. The Updater installs
   Rondo by itself in about 7 minutes. Don't unplug anything until it says it's done.
5. **Press and hold the Y1's center button** (it's also the power button) to switch it on. Rondo's
   logo shows until the main menu appears: Rondo starts by itself, without Android's apps in
   between.

### From a file

The same thing, with the file downloaded by hand:

1. Download **`rom.zip`** from Rondo's [latest release](https://github.com/jarvissax/rondo/releases/latest).
2. In Innioasis Updater CE, open **Settings**, choose **Install from a file…**, and pick the
   `rom.zip` you downloaded.
3. Press **Install / Restore** and follow steps 4 and 5 above.

The Updater deletes the `rom.zip` once it has unpacked it, so download it again if you need it later.

### Moving from Rondo 2.0

Rondo 3.0 comes with a new firmware, without the root backdoors Innioasis's own firmware (and so
Rondo 2.0's) ran, so it installs with Innioasis Updater CE like the first time: choose **Rondo**,
version 3.0.0. If the Updater offers **Keep my settings**, choose it, and your settings, Wi-Fi networks,
server connection and paired headphones stay. Software update on 2.0 shows 3.0.0 but can't install
it, because the firmware has to change too. Your music and playlists on the card are never touched.

### Moving from Rondo 1.0

Rondo 2.0 is a new firmware, so it installs with Innioasis Updater CE like the first time (an
update over Wi-Fi can't do it; Rondo 1.0's dashboard says so, and its Software update with a
server). If the Updater offers **Keep my settings**, choose it: on its first start Rondo 2.0 takes over
1.0's settings, server connection, theme and radio stations, and your Wi-Fi networks and paired
Bluetooth headphones stay. Without it they start fresh. Your music and playlists on the card are
never touched.

Rondo 1.0 needed a server for YouTube; 2.0 doesn't. If you had one, Rondo 2.0 keeps using it
(**My server**). To go without it, choose **Directly to YouTube** in **Settings › YouTube ›
Connection**.

## First start

1. **Wi-Fi:** Settings › Connections › Wi-Fi. Turn the wheel to pick your network and type the
   password on the on-screen keyboard. Only 2.4 GHz networks show up.
2. **Your music:** plug the Y1 into your computer. Rondo asks whether to use it as a **USB drive**;
   choose it, and the microSD card shows up on your computer. Copy music, videos and photos onto it
   (any folders), eject it on the computer, then choose **Turn off USB drive** on the Y1. Music
   from any music player works, in every common format (see
   [Using Rondo](using.md#library-music-videos-photos-and-files)).
3. **YouTube Music and YouTube** work as soon as the Y1 is on Wi-Fi: Rondo talks to YouTube
   itself, with nothing to set up. For your own home, subscriptions, history and playlists, sign
   in with a code in **Settings › YouTube › YouTube account** (or on the dashboard's YouTube page).
4. **A server** is optional. If you want one, [set it up](server.md), then connect the Y1 to it in
   **Settings › YouTube › Connection › Connect to a server**.

Then see [Using Rondo](using.md) for what the wheel and each button do.

## Updating Rondo

A new Rondo comes as a small update you install over the one you have, without flashing the Y1
again. Your settings, Wi-Fi, server connection, playlists and music all stay. Use whichever is
handiest:

<p align="center"><img src="images/software-update.png" width="360" alt="Settings › System › Software update, up to date"></p>

- **On the Y1, over Wi-Fi:** **Settings › System › Software update** shows when a newer Rondo is
  out (the System row in Settings says so too). Choose **Install now**; Rondo downloads it from
  GitHub, installs it and restarts by itself. No server needed.
- **From the dashboard, over Wi-Fi:** open **Settings › Dashboard** on the Y1 and the page on
  your phone or computer. The Overview says when a newer Rondo is out; press **Install**, and your
  browser downloads it from GitHub and sends it to the Y1. **Update from a file**, under the Rondo
  row, installs a `rondo-<version>.rondo` you downloaded from the
  [releases page](https://github.com/jarvissax/rondo/releases).
- **Over USB:** open **[jarvissax.github.io/rondo](https://jarvissax.github.io/rondo/)** in
  Chrome or Edge, plug the Y1 in, press **Connect Y1**, then **Update**. Nothing to install on the
  computer.

Only Rondo's own releases install this way: Rondo checks that an update carries Rondo's
signature. An update that fails to start three times in a row is set aside, and the Rondo that
came with the firmware starts instead.

Now and then a release also changes the firmware around Rondo (the system it runs on, or the boot
screen, for example). The Y1, the dashboard and the update page recognize such a
release: instead of **Install**, they say it needs a new firmware. Install it with Innioasis
Updater CE, [the same way as the first time](#install-with-innioasis-updater-ce). If the Updater
offers **Keep my settings**, choose it and your settings, Wi-Fi and server connection stay too; without
it they're reset (your music on the card is never touched).

Still on Rondo 1.0? Its updates are `rondo-<version>.apk` files, and its Software update asks your
server. Rondo 2.0 itself installs with Innioasis Updater CE ([Moving from Rondo 1.0](#moving-from-rondo-10)).

## Going back

Rondo doesn't lock anything. To return to the Y1's original software, or to try Rockbox or
another firmware, choose it in Innioasis Updater CE and install it the same way. **Original
Software** brings the Y1 back to how it came. Your microSD card stays as it is.
