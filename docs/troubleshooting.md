# Troubleshooting

## Installing and updating

### Innioasis Updater CE doesn't find the Y1

- Try another cable. Many USB-C cables only charge.
- Follow the Updater's steps exactly: the Y1 has to be **completely off** before you plug it in.
  If it's on, hold Power and choose Shut down, or use the reset hole between the headphone and
  USB ports with a paperclip.
- **Windows:** let the Updater install its drivers, and restart the computer once.
- Close other programs that use the Y1's USB connection (adb, Android Studio).

### The Y1 stays dark after installing

It doesn't switch itself on after an install. Press and hold the center button (it's also the
power button). Rondo's logo shows until the main menu appears.

### Rondo isn't in the Updater's list, or has nothing to install

Rondo is for the **Y1, Type A**. Check that the Updater is set to Y1 and Type A, or
[install from a file](install.md#from-a-file). Rondo isn't built for the Type B, so with Type B
there's nothing to install.

### Software update says Rondo "needs a new firmware"

That release changes the firmware around Rondo, not just Rondo, so it can't install over Wi-Fi or
USB. Install it with Innioasis Updater CE, [the same way as the first time](install.md#install-with-innioasis-updater-ce).
If the Updater offers **Keep Data**, choose it to keep your settings, Wi-Fi and server connection.
(This is how Rondo 1.0 moves to 2.0.)

### Software update says "Couldn't check"

Rondo asks GitHub (the update page's site) for the newest release, so it needs Wi-Fi that reaches
the internet; no server is involved. "No Wi-Fi connection" means the Y1 isn't online (Settings ›
Connections › Wi-Fi); "GitHub didn't answer" usually passes, so choose **Try again** in a moment.
Meanwhile you can update [from the dashboard or over USB](install.md#updating-rondo).

### The update page doesn't list the Y1, or can't connect

- Use Chrome or Edge. Firefox and Safari can't talk to USB devices.
- Only one program can use the Y1's USB connection: close the Innioasis Updater, and run
  `adb kill-server` if you have adb. If the Y1 offers the USB drive, choose **Not now**. Then unplug
  the Y1 and plug it back in.
- **Windows:** install Google's [USB driver](https://developer.android.com/studio/run/win-usb)
  for the Y1; without it, the browser can't see it.
- **Linux:** give your user access to USB ID `0bb4:0c03` with a udev rule.

### "The update's signature isn't Rondo's"

Only Rondo's own releases install over the Rondo in the firmware: each update is signed, and Rondo
checks the signature before it installs anything. A build you made yourself is signed with your
own key; put it in a firmware of your own instead ([Building Rondo](building.md)). (Rondo 1.0
says "That APK isn't signed by Rondo's key" for the same reason.)

### Rondo went back to an older version after an update

An update that ends three times in a row within half a minute is set aside, and the Rondo that
came with the firmware runs instead, so the Y1 stays usable. Install the update again from
Settings › System › Software update, or report it with Rondo's log ([how](#still-stuck)).

## YouTube Music and YouTube

### "Couldn't load Home: couldn't reach YouTube" (or "No Wi-Fi connection")

Rondo talks to YouTube itself, so it only needs Wi-Fi that reaches the internet. Check that the Y1
is on Wi-Fi (Settings › Connections › Wi-Fi), then press center to try again. **Settings ›
YouTube › Connection › Connection test** asks YouTube and says how long it took and whether
playback works. Some guest and office networks block YouTube.

### Songs or videos won't play, or stop partway

YouTube changes things often.

- **Directly to YouTube:** try again in a moment, and run the **Connection test** in Settings ›
  YouTube › Connection. If playback keeps failing, look for a newer Rondo in Settings › System ›
  Software update.
- **With a server:** on the admin page, open **Resolver** and press **Update now**, then **Test
  playback**. **Activity** shows the errors behind it. Settings › YouTube on the Y1 says "Playback
  failing" while this is the problem.

### Library, Subscriptions or History are missing

They only appear when you're signed in to YouTube. Directly to YouTube, sign in with a code in
**Settings › YouTube › YouTube account** (or on the dashboard's YouTube page). With a server, sign
the server in on its admin page ([details](server.md#your-youtube-account-optional)).

### YouTube Music's home looks generic

Your own YouTube Music home and history need a cookies.txt: on the dashboard's YouTube page
directly to YouTube, or on the server's admin page with a server. If you uploaded one and it
stopped working, the dashboard (or the admin page's **Activity**) says the cookies were signed
out. Export new ones in a private window, and close the window right after.

### Headphone match says "The server didn't answer"

Headphone match looks your headphone model up through a [Rondo server](server.md), so it needs one
connected (**My server**). Everything else in Settings › Sound works without one.

## With a server

### Settings › YouTube says "Unreachable"

- **At home:** the Y1 and the server must be on the same network, and the server's address must
  not have changed. Give the server a fixed address in your router.
- **On a VPS:** check the HTTPS from a computer with the `openssl` command in the
  [server guide](server.md#on-a-vps). If you use Cloudflare, the record must be "DNS only".
- Check the address in **Settings › YouTube › Connection**: `http://` for home, `https://` for a VPS,
  and the right port.
- Away from home with a home server, choose **Directly to YouTube** there instead.

### "Token refused"

The server no longer knows this Y1. Another Rondo may have been connected, or the server was set
up again. Connect again with a new code from the admin page (**Device › Connect a Rondo**).

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
  stay open on the Y1. After 15 minutes without use it stops by itself; press center on the Y1 to
  start it again.
- **macOS:** browsers need permission to reach devices on your network. Check System Settings ›
  Privacy & Security › Local Network, and switch it on for your browser.
- Some guest and office networks keep devices from seeing each other. Use a home network.

### Bluetooth headphones aren't found

Put them in pairing mode first (usually by holding the power button until a light flashes), then
search on the Y1 or in the dashboard.

### The wheel feels too fast or too slow

Try **Settings › Controls › Wheel acceleration**: Off moves one step per wheel pulse, Fast jumps
further on long spins. **Wheel vibration** and **Click sound** change how each step feels.

### Getting back to the Y1's original software

Install **Original Software** (or Rockbox, or another firmware) with Innioasis Updater CE, the
same way you installed Rondo. See [Going back](install.md#going-back).

## Still stuck?

[Open an issue](https://github.com/jarvissax/rondo/issues). Say what you did and what
happened. If you can, include Rondo's log. adb works on the Rondo firmware: plug the Y1 in (choose
**Not now** when it offers the USB drive) and run

```bash
adb pull /data/rondo/rondo.log rondo-log.txt
```

That's the log since Rondo last started; `/data/rondo/rondo.log.1` is the one before it (after a
restart or a crash). Have a quick look before you post it: it can name your Wi-Fi network.
