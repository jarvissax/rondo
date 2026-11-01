# Troubleshooting

## Installing

### The web installer doesn't list the Y1, or can't connect

- Use Chrome or Edge. Firefox and Safari can't talk to USB devices.
- Only one program can use the Y1's USB connection: close the Innioasis Updater, and run
  `adb kill-server` if you have adb. Then unplug the Y1 and plug it back in.
- **Windows:** install Google's [USB driver](https://developer.android.com/studio/run/win-usb)
  for the Y1; without it, the browser can't see it.
- **Linux:** give your user access to USB ID `0bb4:0c03` with a udev rule.
- The tips below for adb apply too: cable, firmware.

### adb doesn't see the Y1

- Try another cable. Many USB-C cables only charge.
- Close the Innioasis Updater. It stops adb while it's open.
- Restart adb: `adb kill-server`, then `adb devices`.
- Make sure the Y1 runs **Android 4.2 (Dev)**. Other firmware doesn't have USB debugging on.
- **Windows:** if Device Manager shows the Y1 with a warning sign, install Google's
  [USB driver](https://developer.android.com/studio/run/win-usb) for it.
- **Linux:** add a udev rule for the device, or try `sudo adb devices` once to confirm that's
  the problem.

### The Y1 asks "Complete action using" and the wheel doesn't move

Android's own dialogs ignore the wheel. Connect the Y1 to your computer and run the installer
again, from [the web page](https://jarvissax.github.io/rondo/) or the script. It finds the dialog,
picks Rondo and taps **Always** for you.

## YouTube Music and YouTube

### "No music server yet"

The Y1 isn't connected to a server. Set one up and connect it from the dashboard
([server guide](server.md#finish-the-setup)).

### Settings › Server says "Unreachable"

- **At home:** the Y1 and the server must be on the same network, and the server's address must
  not have changed. Give the server a fixed address in your router.
- **On a VPS:** check the HTTPS from a computer with the `openssl` command in the
  [server guide](server.md#on-a-vps). If you use Cloudflare, the record must be "DNS only".
- Check the address in the dashboard's **Server** page: `http://` for home, `https://` for a VPS,
  and the right port.

### "Token refused"

The server no longer knows this Y1. Another Rondo may have been connected, or the server was set
up again. Connect again with a new code from the admin page (**Device › Connect a Rondo**).

### Songs or videos won't play, or stop partway

YouTube changes things often. On the admin page, open **Resolver** and press **Update now**,
then **Test playback**. **Activity** shows the errors behind it. Settings › Server on the Y1 says
"Playback failing" while this is the problem.

### Library, Subscriptions or History are missing

They only appear when the server has a YouTube account. Sign in with a code on the admin page
([details](server.md#your-youtube-account-optional)).

### YouTube Music's home looks generic

Your own YouTube Music home and history need a cookies.txt on the server. If you uploaded one and
it stopped working, the admin page's **Activity** says the cookies were signed out. Export new ones
in a private window, and close the window right after.

## The Y1

### The FM radio says "Plug in wired headphones"

The radio needs wired headphones plugged in, because their cable is the antenna. Bluetooth
headphones don't work for FM.

### The radio plays but I hear nothing

The radio has its own volume, separate from music. Turn the wheel up on the FM screen. If it's
set to **Speaker** in the options, the sound comes from the Y1's speaker instead.

### My Wi-Fi network isn't listed

The Y1 only sees 2.4 GHz networks. Many routers can switch on 2.4 GHz alongside 5 GHz, or give it
its own network name.

### The dashboard won't open

- The phone or computer must be on the same Wi-Fi as the Y1, and **Settings › Dashboard** must
  stay open on the Y1.
- **macOS:** browsers need permission to reach devices on your network. Check System Settings ›
  Privacy & Security › Local Network, and switch it on for your browser.
- Some guest and office networks keep devices from seeing each other. Use a home network.

### Bluetooth headphones aren't found

Put them in pairing mode first (usually by holding the power button until a light flashes), then
search on the Y1 or in the dashboard.

### The wheel feels too fast or too slow

Try **Settings › Wheel acceleration**: Off moves one step per wheel pulse, Fast jumps further on
long spins. **Wheel vibration** and **Click sound** change how each step feels.

### Getting back to the Y1's own player

Settings › **Other launchers** opens it. Restart the Y1 to come back to Rondo. To remove Rondo
completely, see [Removing Rondo](install.md#removing-rondo).

## Still stuck?

[Open an issue](https://github.com/jarvissax/rondo/issues). Say what you did and what
happened. If you can, include Rondo's log:

```bash
adb logcat -d -v time -s Y1 > rondo-log.txt
```
