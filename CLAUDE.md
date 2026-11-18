# Helping someone set up their StackChan (branch `mac-direct-ble`)

This branch turns an M5Stack StackChan robot into a display for the Claude Code sessions on one Mac, over
Bluetooth. There is no server elsewhere and no Wi-Fi setup. The robot is **display only**: it shows sessions
but can't approve, reject or type anything. The README section "Bluetooth setup (this branch)" describes how it
works. This file is for you, Claude, when the person asks for help setting it up.

## What you need from the person

- A Mac (Apple silicon or Intel) where they run Claude Code. That Mac's sessions are the ones shown.
- The robot and a USB-C data cable, plugged into that Mac.
- About 10 minutes, and **being at the Mac for the pairing step**: they must type a 6-digit code within 30
  seconds.

## Steps

Do them in order, and tell the person what each one does before you run it.

1. **Check the tools.** Run `xcode-select -p`. If it fails, ask them to run `xcode-select --install` and finish
   the installer, which gives the compilers and Python 3 that setup needs. Don't install Homebrew, Docker or
   anything else; setup doesn't need them.

2. **Check the robot is connected.** Run `ls /dev/cu.usbmodem*`. Exactly one entry should be listed. If there is
   none, the cable may be charge-only, or the robot may be off. If there are several, ask them to unplug other
   USB serial devices.

3. **Check port 8810 is free.** Run `lsof -nP -iTCP:8810 -sTCP:LISTEN`. If something is listening, tell them
   what it is before going on. Setup stops if the port is taken, except by its own earlier install.

4. **Run setup.** It asks before flashing, and flashing replaces the robot's firmware (it can be flashed back
   later). Ask the person whether they agree. Only then run:

   ```sh
   printf 'y\n' | ./setup.sh
   ```

   Before it starts, tell them: "In about 3–5 minutes the robot restarts and shows **Code: ### ###**. macOS will
   pop up a dialog asking for that code; type it within 30 seconds. macOS may also ask whether the terminal may
   use Bluetooth; allow it."

   Setup:
   - downloads the firmware release and checks its checksum;
   - sets up the server, `cc-watch` and `cc-ble` as LaunchAgents;
   - flashes the robot;
   - pairs.

   The run takes several minutes, so give it a 10-minute timeout.
   - If the person prefers to answer the flash question themselves, have them type `! ./setup.sh` instead.

5. **Check it works.**
   - Run `tail -5 ~/Library/Logs/cc-ble.log`. It should end with `robot ready`.
   - Run `launchctl print gui/$(id -u)/dev.stackchan.cc-monitor | grep state`. It should say `running`.
   - Ask the person to look at the robot. Its screen should show their sessions, or "Nothing running" if none
     are open.
   - The browser preview at `http://127.0.0.1:8810/cc/preview` shows the same screen.
   - Ask them to tap the robot's head. `~/Library/Logs/cc-monitor.log` should log `touch: tap`.

6. **Explain the result in a few lines.**
   - The robot can stay on the Mac's USB for power, or be unplugged; either way it uses Bluetooth.
   - It comes back by itself after a power cut or a Mac restart.
   - Chimes are silent from 22:00 to 08:00 in the Mac's time zone.
   - A long press on the head toggles quiet mode; a tap stops a chime.

## When something goes wrong

| What you see | What to do |
|---|---|
| `pairing failed` or `Encryption is insufficient` after about 30 s | No code was typed in time. Ask them to watch the robot and the Mac, then run `./setup.sh --pair`. The robot must be plugged in by USB and restarted in the last 2 minutes (it shows "Ready to pair"). Restart it with its power button, or by unplugging and replugging it. |
| `no robot ready to pair found` | The pairing window (2 minutes after the robot starts, while plugged in) closed. Restart the robot, then run `./setup.sh --pair`. |
| Robot shows "Waiting for your Mac" forever | Check `~/Library/Logs/cc-ble.log`. If the robot was re-flashed, or the Mac forgot it: in System Settings > Bluetooth, remove "StackChan CC" if listed, restart the robot while plugged in, and run `./setup.sh --pair`. |
| `Bluetooth not allowed` in cc-ble.log | Ask them to allow `cc-ble` in System Settings > Privacy & Security > Bluetooth, then run `launchctl kickstart -k gui/$(id -u)/dev.stackchan.cc-ble`. |
| Robot shows "Not paired" | It isn't paired with any Mac. Plug it in by USB, restart it, and run `./setup.sh --pair`. |
| `curl: (35) ... Connection reset` or another download error | Some networks (company VPNs, filters) block GitHub's file host. Ask them to download `stackchan-cc-ble.zip` from https://github.com/siddiqui-zeeshan/stackchan-ccmonitor/releases/tag/ble-v1 in a browser. Check it with `shasum -a 256 ~/Downloads/stackchan-cc-ble.zip`: the hash must equal the one in `firmware/ble-firmware.sha256`. Only if it matches, run `printf 'y\n' \| ./setup.sh --firmware ~/Downloads/stackchan-cc-ble.zip`. |
| Checksum error during setup | The download is damaged or not the expected file. Try again. If it repeats, stop and tell them; don't bypass the check. |
| Port 8810 in use | Ask them what the program is, and whether it may be stopped. Don't kill it yourself. |
| Robot screen is blank or in the stock launcher | The flash didn't finish. Run `printf 'y\n' \| ./setup.sh` again (with their agreement to flash). |

## Things not to do

- Don't print or copy the contents of `~/.config/cc-monitor/server.env` or `ble.json`, or the LaunchAgent plists
  in `~/Library/LaunchAgents/dev.stackchan.*`. They hold the local tokens.
- Don't build firmware (`firmware/build.sh --ble`) unless they ask for their own build. It needs Docker and a
  several-GB image. Setup flashes the published build.
- Don't bypass the firmware checksum, and don't edit `server/` or `firmware/` to make setup pass.
- Don't commit, push or open pull requests in this repo for them.
- Removing it all: `./setup.sh --uninstall`. That keeps `~/.config/cc-monitor` (the pairing and tokens). Delete
  that folder too for a clean slate, but ask first.
