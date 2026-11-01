# Installing Rondo

There are three parts. You put the community's Android 4.2 firmware on the Y1, install a small
tool called adb on your computer, and then run Rondo's install script. You only do this once;
after that, updates are a single command.

## Before you start

- **Flashing replaces the Y1's software.** Your microSD card isn't touched, but anything stored
  on the Y1's small internal storage may be erased. Copy off anything you want to keep.
- **Charge the Y1** to at least half.
- Use a **USB-C cable that carries data**. Some charging cables don't, and nothing will work
  with them.

## Step 1: Put Android 4.2 (Dev) on the Y1

Rondo needs the Y1's Android 4.2 firmware with USB debugging switched on. The community
publishes exactly that as **"Android 4.2 (Dev)"**, and their Innioasis Updater installs it for you.

1. Download **Innioasis Updater** for your computer from its
   [releases page](https://github.com/y1-community/Innioasis-Updater/releases/latest).
   On Windows, let it install its drivers and restart the computer once before the first flash.
2. Open it and choose the **Y1** and your wheel type (**Type A** or **Type B**).
3. In the software list, choose **Android 4.2 (Dev)**. That's the version Rondo is tested on,
   and the one with USB debugging switched on, which the install needs.
4. Follow the Updater's instructions to connect the Y1 and flash it. Don't unplug anything until
   it says it's done.
5. The Y1 restarts into the new firmware. It may start in Rockbox, which comes with this
   firmware. That's fine; Rondo installs next to it.

## Step 2: Install adb on your computer

adb is Google's tool for talking to Android devices over USB.

- **macOS:** `brew install android-platform-tools` (with [Homebrew](https://brew.sh)), or
  download Google's [Platform Tools](https://developer.android.com/tools/releases/platform-tools).
- **Windows:** download [Platform Tools](https://developer.android.com/tools/releases/platform-tools)
  and unzip it. You'll also want [Git for Windows](https://gitforwindows.org), which brings the
  "Git Bash" terminal the install script runs in.
- **Linux:** `sudo apt install adb` (or your distribution's equivalent).

Close the Innioasis Updater first (it stops adb while it runs), plug in the Y1, and check that
your computer sees it:

```bash
adb devices
```

You should see one device listed as `device`. If it says `unauthorized` or nothing shows up, see
[Troubleshooting](troubleshooting.md#adb-doesnt-see-the-y1).

## Step 3: Install Rondo

1. Download **`rondo-<version>.apk`** from the
   [latest release](https://github.com/jarvissax/rondo/releases/latest).
2. Download the install script next to it. On macOS and Linux, use a terminal; on Windows, use Git Bash:

   ```bash
   curl -LO https://raw.githubusercontent.com/jarvissax/rondo/main/tools/install.sh
   ```

3. Run it with the APK you downloaded:

   ```bash
   bash install.sh rondo-1.0.0.apk
   ```

   If adb isn't on your PATH (on Windows it usually isn't), tell the script where it is:
   `ADB=/c/Users/you/platform-tools/adb.exe bash install.sh rondo-1.0.0.apk`.

The script checks the Y1, installs Rondo, and makes it the home screen by choosing Rondo and
**Always** in Android's "Complete action using" dialog. It doesn't change anything else.

When it's done, the Y1 shows Rondo's main menu, and it will start in Rondo from now on.

## Step 4: First steps on the Y1

1. **Wi-Fi:** Settings › Wi-Fi. Turn the wheel to pick your network, and type the password with
   the on-screen keyboard. Only 2.4 GHz networks show up.
2. **Your music:** copy music onto the microSD card (any folders) and open **Library**. Rondo
   reads the card at start; the dashboard has a **Rescan** button for later changes.
3. **YouTube Music and YouTube:** [set up a server](server.md), then connect the Y1 to it from
   the dashboard.

## Updating Rondo

Download the new APK and run the same script again, or just:

```bash
adb install -r rondo-1.1.0.apk
```

Your settings, saved stations and server connection stay.

## Using another launcher

Rondo doesn't remove anything, so the Y1's own player (and Rockbox, if it's there) are still
installed.

- **Open another launcher for a while:** Settings › **Other launchers** lists every other home
  screen on the Y1. Pick one to open it.
- **Come back to Rondo:** restart the Y1. Rondo is the home screen, so the Y1 always starts in it.

## Removing Rondo

```bash
adb uninstall com.jarvissax.rondo
```

That removes the app and its settings. Nothing else on the Y1 changes. If the Y1 then asks which
home screen to use and the wheel won't move through the list, see
[Troubleshooting](troubleshooting.md#the-y1-asks-complete-action-using-and-the-wheel-doesnt-move).
