# StackChan CC Monitor without a server: Mac ↔ robot over Bluetooth

Status: reviewed, waiting for approval, 2026-11-18. Branch: `mac-direct-ble`.

## Why

A second StackChan is for another person. They should be able to watch their Claude Code sessions on the
robot using only their own Mac: no Linux server, no Tailscale, no Gemini key. Anyone should
be able to set it up from this branch of the repo.

Today the robot connects over Wi-Fi to a server on a Linux host (`server/cc_monitor.py` in Docker, reached
through Tailscale Funnel). The server holds all the logic: moods, sentences, chimes, routing Approve and Reject
to the right Mac, and Talk transcription. `cc-watch` on each Mac only reports sessions and types keys into
Ghostty.

## Goals

- The robot and the Mac talk directly over Bluetooth Low Energy. No other machine is involved.
- The robot is display only: moods, faces, the session list and session details, chimes, and the head panel
  (tap to acknowledge, long press for quiet mode), as on our robot. The user decided there are no session
  controls: no Approve, Reject, focus-a-tab or Talk. The head panel stays because it only affects the robot
  itself (my call).
- Set up from this branch with one script on the Mac, plus one flash over USB.
- Only the Mac the robot was paired with can connect to it and see session text.

## Non-goals

- Any session control from the robot: Approve, Reject, focus a tab, Talk. Their terminal doesn't matter, since
  nothing is typed into it.
- Feature 5 (answering multiple-choice questions). It resumes on `main` after this work and gets ported here
  later.
- Wi-Fi, or several Macs per robot.
- Merging this into `main`. This branch is a separate variant; it gets rebased on `main` when needed.

## Anti-goals (a failure even if it works)

- Anything that changes how our robot, our Linux server or `main` behave.
- Any device within Bluetooth range being able to read session text or drive the robot without pairing.
- Any path, however indirect, by which the robot can type into a terminal.
- A second copy of the server's rules (moods, chimes, sentences) that has to be kept in step with Python.

## Constraints and facts found

- **Range:** the robot sits in the same room as the Mac (about 10 m).
- **The robot already has Bluetooth.** The stock firmware (StackChan v1.6.2, the base in `firmware/build.sh`)
  has NimBLE on and a GATT service `e2e5e5e0-1234-5678-1234-56789abcdef0` with JSON characteristics for
  motion, avatar, config and RGB (`main/hal/utils/bleprph/gatt_svr.c`, `main/hal/hal_ble.cpp`). It is only
  advertised after an app calls `GetHAL().startBleServer()` (the Dance app does).
- **Stock message splitting:** `hal_ble.cpp` already splits long JSON into packets with a 10-byte header (magic
  `AA 55 C3`, version 1, packet index, packet count, total length, all 16-bit) and joins them on receipt. The
  stock characteristics cap a message at 2 KB (`STACKCHAN_MAX_JSON_LEN`).
- **Message sizes** (measured from `Monitor.display()`): 1 session 0.8 KB, 3 sessions 1.7–2.0 KB, 6 sessions
  3.2–3.7 KB, 12 sessions (the most the robot lists) 6.1–7.1 KB. The server sends one when something changes and
  at most every 5 s otherwise.
- **Stock pairing is weak:** `sm_io_cap` is "no input, no output" (Just Works), the passkey is hard-coded to
  123456 in the display case, and bonds are not saved (`CONFIG_BT_NIMBLE_NVS_PERSIST` is off), so they're lost
  on every restart. Secure Connections, bonding and MITM flags are on. At most 3 bonds and 3 connections.
- **USB:** the robot's console is the ESP32-S3 USB Serial/JTAG port. The new robot shows up as
  `/dev/cu.usbmodem1101`, USB serial number `7C:4F:AD:AF:93:0C`. Opening the port resets the robot unless DTR and
  RTS are both held.
- **macOS:** a background program that uses Bluetooth must carry an Info.plist with
  `NSBluetoothAlwaysUsageDescription`, or macOS kills it. The user is asked once to allow it. CoreBluetooth gives
  each robot a per-Mac UUID, not its hardware address.
- **Mac software:** Xcode Command Line Tools provide `swiftc` and Python 3.9. The server already runs on 3.9:
  it uses `from __future__ import annotations`, and its only dependency is `aiohttp>=3.9`.

## Approaches considered

1. **Wi-Fi, server on their Mac.** The firmware connects to the Mac's `.local` name, since the robot's DNS can
   resolve those (`CONFIG_LWIP_DNS_SUPPORT_MDNS_QUERIES=y`). This is the least work, but it fails on networks
   that block multicast, and the user said a stable address may be difficult.
2. **USB cable.** There's no network at all, but the robot is tied to the Mac by a cable, the same port carries
   console logs, and opening it can reset the robot.
3. **Bluetooth (chosen).** It works on any network or none, and the robot can sit anywhere in the room. The cost
   is more work: a robot Bluetooth channel, a Mac Bluetooth bridge, and proper pairing.

Rebuilding the server's logic inside `cc-watch` was rejected under every option: it means two copies of the
rules.

The user picked Bluetooth, with pairing done once while the robot is plugged in by USB.

## Design

```
their Mac                                                        robot
cc-watch ──ws──▶ cc_monitor.py (127.0.0.1) ◀──ws── cc-ble ◀──BLE──▶ CC Monitor app
(unchanged)      (unchanged, local)                 (new bridge)        (BLE build)
```

### Mac

- **`server/cc_monitor.py`, unchanged,** runs locally on `127.0.0.1:8810` as a LaunchAgent from a Python venv
  with `aiohttp`. Setup generates `MAC_TOKENS`, `ROBOT_TOKEN` and `ROBOT_SECRET` at random and keeps them in
  `~/.config/cc-monitor/`. `GEMINI_API_KEY` is empty. The browser preview works on the Mac as on our server.
- **`cc-watch`, unchanged,** points at `ws://127.0.0.1:8810/cc/mac` with the local token and `"ghostty": false`
  in its config. It then never looks up Ghostty tabs, so there's no Automation prompt, and every session reaches
  the server without a terminal, which the server already treats as view-only (`action_for` refuses it).
- **New `cc-ble` (Swift, CoreBluetooth),** a LaunchAgent with an embedded Info.plist. It:
  - connects to the local server as the robot (`/<ROBOT_SECRET>/cc/robot`, Bearer `ROBOT_TOKEN`), exactly as the
    robot does today;
  - finds and connects to the paired robot over Bluetooth, and reconnects by itself when either side drops;
  - forwards server → robot messages (`display`, `result`) as JSON written to the robot's characteristic, split
    into packets;
  - forwards robot → server messages from the robot's notifications, joined from packets, but only `touch`.
    It drops anything else (`act`, `send`, `focus`, `ptt_*`), so no session action can come from the robot even
    if its firmware were changed.
  - When it has no robot it keeps its server socket open, so the server and preview behave normally.
  - It logs to `~/Library/Logs/cc-ble.log`.

  Swift rather than Rust because CoreBluetooth is native to it. Adding a Rust Bluetooth library (`btleplug`)
  would pull in an async runtime and roughly double `cc-watch`'s size.

### Robot

- **Build variant:** `firmware/build.sh --ble` builds the CC Monitor app in Bluetooth mode (a compile-time
  define). No URL or token is compiled in. Since this firmware holds no secrets, one binary fits every robot.
  The Wi-Fi build for our robot stays the default and is unchanged.
- **Channel:** one new characteristic on the existing StackChan service, for example
  `e2e5e5e5-1234-5678-1234-56789abcdef0`. The Mac writes to it (encrypted, authenticated link only), and the
  robot notifies on it. It uses the stock packet header, with the cap raised to 16 KB, the robot's current
  WebSocket receive buffer. The added code goes into `firmware/stackchan.patch` next to the existing changes.
- **Messages:** the same JSON the app exchanges with the server today, so `onData` / `send_json` keep their
  meaning and only the transport changes. The app's connection task (`ws_task`) gets a Bluetooth twin that
  starts the Bluetooth server instead of Wi-Fi.
- **Security:**
  - The robot accepts writes, and sends notifications, only on a link that is encrypted and bonded.
  - Bonds are saved in NVS (turn on `CONFIG_BT_NIMBLE_NVS_PERSIST`).
  - Pairing uses a random 6-digit passkey shown on the robot's screen, which the user types into the macOS
    prompt (IO capability "display only").
- **Pairing window (the user's idea, plus a time limit as my call):** the robot accepts a new pairing only while
  it is plugged into a computer by USB (`usb_serial_jtag_is_connected()`), and only within 2 minutes of the CC
  MONITOR app starting while plugged in. The time limit is there because the robot may well live on the Mac's
  USB port for power, and then the USB check alone would never close the window. A new pairing replaces the old
  one, so a robot belongs to one Mac at a time.
- **Pairing runs in the foreground (my call):** `setup.sh` runs `cc-ble --pair` in the Terminal window, so the
  macOS passkey prompt and the Bluetooth permission prompt belong to a visible program. The background
  LaunchAgent only reconnects using the saved bond. This avoids depending on macOS showing pairing prompts for a
  background program. The spike still checks that the bond made in the foreground works from the LaunchAgent.
- **Starts by itself (my call):** on the Bluetooth build the robot opens CC MONITOR at boot, skipping the
  launcher and the stock first-run "Welcome" page, so it reconnects after a power cut without anyone touching it.
- **Stock Bluetooth features off (my call):** on the Bluetooth build the stock open characteristics (motion,
  avatar, config, RGB), the Dance app's Bluetooth use and the phone-app Wi-Fi setup are left out. Otherwise anyone
  in range could move the head or change the face without pairing. Leaving them out also avoids two known stock
  problems: NimBLE being started twice, and other connections overwriting the single notify connection handle.
  Only the CC Monitor service is registered and advertised.
- **Screens:**
  - Not paired: the offline face with "Plug into your Mac by USB to pair".
  - Paired but the Mac is away: the offline face with "Waiting for your Mac".
  - While pairing: the passkey in large digits.
  - Otherwise the same screens as our robot. The session screen shows the session's details with no Approve,
    Reject, Talk or focus buttons, and the app never sends `act`, `send`, `focus` or `ptt_*` on this build.

### Setup (README on this branch)

1. Install Xcode Command Line Tools (`xcode-select --install`).
2. Run `./setup.sh`. It:
   - builds `cc-watch` (Rust if `cargo` is present, else the existing Swift version) and `cc-ble`;
   - creates the venv;
   - writes configs and installs the three LaunchAgents;
   - flashes the robot over USB with `esptool` from the venv.
3. While the robot is still plugged in, `setup.sh` runs `cc-ble --pair`. CC MONITOR opens by itself after the
   flash, the robot shows a passkey, and macOS asks for it.
4. The robot can then be unplugged, or stay on the Mac's USB for power. Either way it talks over Bluetooth only.

`./setup.sh --uninstall` removes the LaunchAgents and binaries.

**Firmware binary:** building firmware needs Docker and the ESP-IDF image (several GB), which is too much for
"easy". Because the Bluetooth build holds no secrets, we publish one prebuilt firmware as a GitHub release asset
on this branch, and `setup.sh` downloads and flashes it. `firmware/build.sh --ble` remains for anyone who wants to
build it themselves.

## Testing

- **Spike first, thrown away:** a Swift program pairs with the robot using a passkey from the foreground. The same
  binary then runs as a LaunchAgent with the embedded Info.plist, reconnects using that bond, and sends a 7 KB
  message. This checks the three unknowns: the macOS permission under launchd, whether the bond carries over to
  the LaunchAgent, and the speed.
- **Packet splitting:** a host-buildable header shared by the firmware (like `pix.h`), with a C++ test run on the
  Mac, and the Swift side tested against the same packet examples.
- **Server:** no change is expected. If one is needed, it gets pytest tests as usual.
- **End to end with the real robot:**
  - pairing works only while it's plugged in;
  - a second Mac can't connect once the robot is unplugged;
  - the session screen has no action buttons, and `cc-ble` drops an `act` message sent to it by hand;
  - the robot reconnects after a Mac sleep and after a robot restart;
  - 12 sessions show correctly.

## Left to the builder

- The exact characteristic UUID, and the module layout inside the firmware patch.
- How `cc-ble` finds the robot on first pairing: the robot advertises a "pairing" flag while plugged in. After
  that, `cc-ble` uses the remembered CoreBluetooth identifier.
- Reconnect timing, and log wording.
- Whether `setup.sh` lives at the repo root or in `mac/`.

## Decided after review

- The robot is display only (the user's decision). This also settles which terminal they use: it doesn't matter.
- The firmware binary is published as a GitHub release asset (the user's decision), through the server host's
  `gh` login, the same route as our pushes.
- My calls, from the review: pairing runs in the foreground from `setup.sh`; the pairing window also closes 2
  minutes after the app starts; the robot opens CC MONITOR at boot; the stock Bluetooth features are left out of
  this build.
