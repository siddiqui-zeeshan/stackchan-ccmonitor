# Mac ↔ robot over Bluetooth Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A display-only StackChan that talks to one paired Mac over Bluetooth, set up by anyone from this branch.

**Architecture:** On the Mac, the unchanged `server/cc_monitor.py` runs on 127.0.0.1 and the unchanged
`cc-watch` reports sessions to it with Ghostty lookups off. A new Swift program, `cc-ble`, connects to the server
as the robot and relays JSON to and from the robot over one Bluetooth characteristic, split into packets. The
robot firmware gets a Bluetooth build (`build.sh --ble`). In that build the CC Monitor app runs its own NimBLE
service with bonded, passkey-protected pairing instead of the Wi-Fi WebSocket.

**Tech Stack:** ESP-IDF 6.1 + NimBLE (C++20), Swift + CoreBluetooth + URLSessionWebSocketTask, Python 3.9+ /
aiohttp (unchanged), POSIX sh.

**Spec:** `docs/superpowers/specs/2026-11-18-mac-direct-ble-design.md`

## Global Constraints

- Work only in this worktree, on branch `mac-direct-ble`. `main`, our robot and the Linux server must not change.
- **Robot is display only.** No `act`, `send`, `focus` or `ptt_*` message may leave the robot or pass `cc-ble`.
- **Packet header** (stock format, 10 bytes): `AA 55 C3 01`, then packet index, packet count and total message
  length as big-endian `uint16`. Every packet carries the header, even a one-packet message.
- **Message cap:** 16384 bytes.
- **Service UUID:** `e2e5e5e0-1234-5678-1234-56789abcdef0` (the stock one, which this build doesn't otherwise
  register).
- **Characteristic UUID:** `e2e5e5e5-1234-5678-1234-56789abcdef0`. It is Write (encrypted and authenticated), plus
  Notify.
- **Pairing window:** USB host connected AND under 120000 ms since the app started. One bond at a time.
- **Passkey:** random 6 digits, shown on the robot. IO capability: display only.
- **Advertised name:** `StackChan CC`. While pairing is open, the advertisement carries service data byte `0x01`
  for the service UUID.
- **Mac paths:**
  - config in `~/.config/cc-monitor/`;
  - venv and server copy in `~/Library/Application Support/cc-monitor/`;
  - logs `~/Library/Logs/cc-monitor.log` and `~/Library/Logs/cc-ble.log`.
- **LaunchAgent labels:** `dev.stackchan.cc-monitor`, `dev.stackchan.cc-ble`, plus the existing
  `dev.stackchan.cc-watch`.
- **Firmware release tag:** `ble-v1`, asset `stackchan-cc-ble.zip`. Its SHA-256 is committed in
  `firmware/ble-firmware.sha256`.
- **Talk:** off. Setup never asks for a Gemini key.
- **Approvals:** ask the user before flashing the new robot, pushing, or publishing the release.

## Review Focus

1. **The local server must not be reachable from the network.** `config_from_env` defaults `LISTEN_HOST` to
   `0.0.0.0`, so setup must set `127.0.0.1`. The test is in Task 7.
2. **A robot that reconnects must get the current screen straight away**, not on the next change, which may be
   minutes away. The server only sends a full state when a robot socket opens, and the bridge's server socket
   stays open across Bluetooth drops. The test is in Task 5 (`Relay` keeps the last `display`).
3. **A corrupt or swapped firmware download must not be flashed.** The test is in Task 7 (SHA-256 mismatch stops
   setup).
4. **Quiet hours must follow the other person's clock.** The server defaults `TZ_OFFSET_MINUTES` to 330 (India).
   The test is in Task 7 (offset taken from `date +%z`).
5. **A Mac that wasn't paired must not get data.** A nearby device that connects outside the window, or without
   bonding, must get no notifications, and its writes must be refused. This is checked in Task 2 (`may_pair`),
   Task 3 (notify only on a secure link) and Task 8 (a second device).

---

### Task 1: Spike (throwaway, not committed)

Checks the unknowns on the Mac side before real work starts: a LaunchAgent binary with an embedded Info.plist may
use Bluetooth, and a 7 KB message gets through quickly. It uses the robot's stock firmware, whose Dance app
advertises `StackChan` with the stock motion characteristic `e2e5e5e1-…`, capped at 2 KB per message. Whether a
bond made in the foreground works from the LaunchAgent can only be checked once our firmware exists. That check
moves to Task 8.

**Files (scratchpad only):** `spike/spike.swift`, `spike/Info.plist`, `spike/dev.stackchan.spike.plist`

- [ ] **Step 1:** Write `spike.swift`. It scans for a peripheral named `StackChan` and connects. It writes 4
  messages of 2000 bytes each, split with the packet header at `maximumWriteValueLength(for: .withResponse)`,
  with response, to `e2e5e5e1-1234-5678-1234-56789abcdef0`. It logs the time from first write to last callback.
  The payload is a valid motion JSON padded with spaces.
- [ ] **Step 2:** Build with the Info.plist embedded. The plist has `CFBundleIdentifier dev.stackchan.spike` and
  an `NSBluetoothAlwaysUsageDescription`.
  `swiftc -O spike.swift -o spike -Xlinker -sectcreate -Xlinker __TEXT -Xlinker __info_plist -Xlinker Info.plist`
- [ ] **Step 3:** Ask the user to open the Dance app on the new robot. Run `./spike` in Terminal once. Expected:
  macOS asks for Bluetooth permission, the robot is found, and the log shows the time per 2 KB.
- [ ] **Step 4:** Load it as a LaunchAgent (`launchctl bootstrap gui/$(id -u) dev.stackchan.spike.plist`, with
  `StandardErrorPath` in the scratchpad). Expected: same result, no crash, no new prompt. Then `launchctl bootout`.
- [ ] **Step 5:** Report the timings to the user. The test passes if 7 KB takes under 1 s. If not, stop and
  rethink before Task 2. Delete the LaunchAgent plist from `~/Library/LaunchAgents` if any copy was made.

### Task 2: Packet splitting and pairing rule (robot side, host-testable)

**Files:**
- Create: `firmware/app_cc_monitor/frames.h` (header-only, no ESP-IDF), `firmware/tests/frames_test.cpp`,
  `firmware/tests/run.sh`
- Modify: `README.md` (test section)

**Interfaces:**
- Produces:
  - `namespace frames { constexpr size_t HEADER = 10; constexpr size_t MAX_MESSAGE = 16384; }`
  - `std::vector<std::string> frames::split(std::string_view msg, size_t packet_size)`. It returns an empty vector
    if the message is empty, if `msg.size() > MAX_MESSAGE`, or if `packet_size <= HEADER`.
  - `class frames::Joiner { public: enum class Got { Partial, Message, Error }; Got push(const uint8_t* p, size_t n); std::string take(); }`.
    `take()` returns the finished message once.
  - `inline bool frames::may_pair(bool usb_host, uint32_t ms_since_start)`: true when `usb_host` is set and
    `ms_since_start < 120000`.

- [ ] **Step 1: Write the failing tests** in `frames_test.cpp`. They use plain `assert` and print `ok`, like the
  repo's other host tools.
  - `split("{\"a\":1}", 20)`: one packet, bytes `AA 55 C3 01 00 00 00 01 00 07` + payload.
  - 7000 bytes at `packet_size` 244 gives 30 packets (234 payload bytes each, the last one 214). Every header has
    count 30 and length 7000 (`1B 58`).
  - A joiner fed the 30 packets returns `Partial` 29 times, then `Message`. `take()` equals the original.
  - Index 2 after index 0 gives `Error`. The next packet with index 0 starts over and succeeds.
  - Total length 16385 in a header gives `Error`.
  - A wrong magic byte, a packet shorter than 10 bytes, or a total length that differs from the first packet's,
    gives `Error`.
  - The payload byte sum exceeding the stated length gives `Error`.
  - `split` of 16385 bytes gives an empty vector.
  - `may_pair(true, 0)` and `may_pair(true, 119999)` are true. `may_pair(true, 120000)` and
    `may_pair(false, 10)` are false.
- [ ] **Step 2:** `firmware/tests/run.sh` compiles with `c++ -std=c++20 -Wall -Wextra -Werror`, then runs. Run it.
  Expected: compile error, because `frames.h` doesn't exist yet.
- [ ] **Step 3:** Implement `frames.h`. After `Error` the joiner resets, so the next index-0 packet starts a new
  message.
- [ ] **Step 4:** Run `firmware/tests/run.sh`. Expected: `ok`.
- [ ] **Step 5:** README: add `firmware/tests/run.sh` next to `render.sh --check`. Commit:
  `Robot: Bluetooth packet splitting and the pairing rule, with host tests`.

### Task 3: Robot Bluetooth link

**Files:**
- Create: `firmware/app_cc_monitor/cc_ble.h` (no NimBLE includes), `firmware/app_cc_monitor/cc_ble.cpp`. The whole
  file is inside `#ifdef CC_MONITOR_BLE`.

**Interfaces:**
- Consumes: `frames::split`, `frames::Joiner`, `frames::may_pair` (Task 2).
- Produces:
  - `namespace cc_ble { enum class State { NotPaired, Pairing, Waiting, Connected }; }`
  - `struct Events { std::function<void(std::string)> on_message; std::function<void(uint32_t)> on_passkey; std::function<void(State)> on_state; };`
  - `void start(Events ev)`, safe to call once. `bool send(const std::string& json)`, which returns false
    without a secure link.
  - The callbacks run on the NimBLE host task, so callers must not touch LVGL in them.

- [ ] **Step 1:** Write `cc_ble.cpp`, modelled on the stock `main/hal/utils/bleprph/bleprph.c` and `gatt_svr.c`,
  but registering only the CC service.
  - `ble_hs_cfg`: `sm_io_cap = BLE_HS_IO_DISPLAY_ONLY`, `sm_bonding = 1`, `sm_mitm = 1`, `sm_sc = 1`. Key
    distribution is ENC and ID both ways. `store_status_cb = ble_store_util_status_rr`.
  - Device name `StackChan CC`. Call `ble_store_config_init()`.
  - Advertising carries the service UUID. While `may_pair(usb_serial_jtag_is_connected(), now - start)` holds,
    it adds service data `0x01`. Re-check every second and restart advertising when that value changes.
  - **On connect:** if the peer has no bond (`ble_store_read_peer_sec` misses) and the window is closed,
    disconnect with `BLE_ERR_REM_USER_CONN_TERM`. Otherwise call `ble_gap_security_initiate`.
  - **On `BLE_GAP_EVENT_PASSKEY_ACTION`:** for `BLE_SM_IOACT_DISP`, inject `esp_random() % 1000000`, call
    `on_passkey`, and set state `Pairing`.
  - **On `BLE_GAP_EVENT_REPEAT_PAIRING`:** delete the old bond and retry.
  - **Write callback:** feed `Joiner`; call `on_message` on `Message`.
  - **`send()`:** splits at the negotiated MTU − 3 and notifies. It does nothing unless the connection is
    `encrypted && authenticated && bonded`.
  - **States:**
    - `NotPaired`: no bond stored, window closed.
    - `Pairing`: window open, no bond.
    - `Waiting`: bond stored, nobody connected.
    - `Connected`: secure link up.
- [ ] **Step 2:** Compile check happens in Task 4. No host test here. The rules it relies on are tested in Task 2,
  and the link itself is tested on the robot in Task 8.
- [ ] **Step 3:** Commit: `Robot: Bluetooth link for the CC Monitor app (BLE build only)`.

### Task 4: Bluetooth build of the firmware and the app changes

**Files:**
- Create: `firmware/stackchan-ble.patch`
- Modify:
  - `firmware/build.sh`
  - `firmware/app_cc_monitor/app_cc_monitor.cpp` (`ws_task` ~line 112, `send_json` ~167, `onOpen` start ~683,
    offline text ~909, `renderSession` ~1303)
  - `.gitignore`
  - `README.md`

**Interfaces:**
- Consumes: `cc_ble::start`, `cc_ble::send`, `cc_ble::State` (Task 3).
- Produces:
  - `./build.sh --ble [--flash] [--package <zip>]` builds in `firmware/.work-ble`.
  - The zip holds the five files listed in `build/flash_args` plus `flash_args`.

- [ ] **Step 1: `stackchan-ble.patch`** (applied on top of `stackchan.patch`, BLE tree only):
  - `Hal::startBleServer()` and `Hal::startAppConfigServer()` return straight away, with a log line "disabled in
    the CC Monitor Bluetooth build".
  - In `main.cpp`, store the id from `installApp(std::make_unique<AppCcMonitor>())` in
    `int g_cc_monitor_app_id` (declared in `apps.h`).
  - In `AppLauncher::onLauncherOpen`, when `!_startup_checked`: set `_startup_checked = true`,
    `create_launcher_view()`, then `openApp(g_cc_monitor_app_id)`. This skips the stock startup worker.
- [ ] **Step 2: `build.sh`:**
  - The `--ble` argument form takes no URL or token. It uses `WORK=${WORK:-$HERE/.work-ble}`, applies
    `stackchan-ble.patch` after the usual patches on first clone, and copies `cc_ble.h`, `cc_ble.cpp` and
    `frames.h`.
  - It writes a config with `#define CC_MONITOR_BLE 1` and no URL or token.
  - Before building, it sets `CONFIG_BT_NIMBLE_NVS_PERSIST=y` and `CONFIG_BT_NIMBLE_MAX_BONDS=1` in the tree's
    `sdkconfig`.
  - `--package <zip>` zips the flash files with paths as in `flash_args`.
  - The Wi-Fi form is unchanged.
  - `.gitignore`: add `firmware/.work-ble/`.
- [ ] **Step 3: app**, all under `#ifdef CC_MONITOR_BLE`:
  - `ble_task` replaces `ws_task`. It calls `cc_ble::start` with `on_message` pushing into `_rx_queue`, the same
    8-deep queue. `on_state` and `on_passkey` set `_net_msg` / a `_passkey` atomic. It doesn't start Wi-Fi.
  - `send_json` drops anything not starting with `{"type":"touch"` and otherwise calls `cc_ble::send`.
  - `renderSession` never shows reject, approve, talk or focus, and `_ss_note` is empty.
  - **Offline screen text** (line1 / line2), exact:
    - NotPaired: `Not paired` / `Plug into your Mac by USB, then restart me`
    - Pairing, no passkey yet: `Ready to pair` / `Run ./setup.sh --pair on your Mac`
    - Passkey shown: `Code: 123 456` (the real code, grouped 3 + 3) / `Type it on your Mac`
    - Waiting: `Waiting for your Mac` / `Is it on and nearby?`
- [ ] **Step 4:** Run `firmware/build.sh --ble`. Expected: `Project build complete`, no new warnings in
  `cc_ble.cpp` or `app_cc_monitor.cpp`. Also run the Wi-Fi build (`firmware/build.sh <url> <token>` with dummy
  values, in a throwaway `WORK`). Expected: it still builds, which proves the `#ifdef`s keep our robot's build as
  it was.
- [ ] **Step 5:** README: a section for this branch: what the Bluetooth build is, `build.sh --ble`, and that the
  stock Bluetooth features and setup page are off in it. Commit:
  `Robot: Bluetooth build (build.sh --ble): opens CC Monitor at boot, display only`.

### Task 5: Packet splitting and relay rules (Mac side)

**Files:**
- Create: `mac/ble/Frames.swift`, `mac/ble/Relay.swift`, `mac/ble/Tests.swift` (a test `main`),
  `mac/ble/test.sh`

**Interfaces:**
- Produces:
  - `enum Frames { static let header = 10; static let maxMessage = 16384; static func split(_ msg: Data, packetSize: Int) -> [Data] }`
  - `final class Joiner { enum Got { case partial, message(Data), error }; func push(_ p: Data) -> Got }`
  - `struct Relay { mutating func fromServer(_ text: String) -> String?; func fromRobot(_ text: String) -> String?; mutating func robotConnected() -> String? }`:
    - `fromServer` remembers the last message whose `type` is `display` and returns the text to forward.
    - `fromRobot` returns the text only when its JSON `type` is `touch`, otherwise nil.
    - `robotConnected` returns the last `display` text, or nil.

- [ ] **Step 1: Write the failing tests** in `Tests.swift`:
  - The same byte examples as Task 2: one 7-byte message at size 20, and 7000 bytes at size 244 giving 30 packets
    with a header length of `1B 58`.
  - Joiner round trip and error cases, mirroring Task 2.
  - `Relay`:
    - `fromRobot` on a `touch` message passes. `act`, `send`, `focus`, `ptt_start` and invalid JSON give nil.
    - `robotConnected()` is nil before any display, and the latest display text after two displays and a
      `result` in between.
- [ ] **Step 2:** `mac/ble/test.sh`: `swiftc Frames.swift Relay.swift Tests.swift -o "$tmp/t" && "$tmp/t"`. Run it.
  Expected: compile failure.
- [ ] **Step 3:** Implement `Frames.swift` and `Relay.swift` (Foundation `JSONSerialization` to read `type`).
- [ ] **Step 4:** Run `mac/ble/test.sh`. Expected: `ok`.
- [ ] **Step 5:** Commit: `Mac: Bluetooth packet splitting and relay rules for cc-ble, with tests`.

### Task 6: `cc-ble` bridge

**Files:**
- Create: `mac/ble/main.swift`, `mac/ble/Info.plist`, `mac/ble/build.sh`

**Interfaces:**
- Consumes: `Frames`, `Joiner`, `Relay` (Task 5).
- Produces:
  - The binary `cc-ble`, built by `mac/ble/build.sh <out path>` with the Info.plist embedded as in Task 1. The
    plist has `CFBundleIdentifier dev.stackchan.cc-ble`.
  - Config `~/.config/cc-monitor/ble.json`: `{"server": "ws://127.0.0.1:8810/<ROBOT_SECRET>/cc/robot", "token": "<ROBOT_TOKEN>", "robot": "<peripheral UUID or empty>"}`.
  - `cc-ble --pair` exits 0 after saving `robot`, or 1 with a message after 120 s.

- [ ] **Step 1: Write `main.swift`.**
  - **Server side:** `URLSessionWebSocketTask` with `Authorization: Bearer <token>`. It reconnects 5 s after any
    failure. Each text goes through `relay.fromServer`, then to the robot if one is connected.
  - **Robot side, `--pair`:** scan for the service UUID. Take the first advertisement whose service data is
    `0x01`. Connect, discover the characteristic, subscribe. The robot starts pairing and macOS shows the
    passkey prompt. Write `{"type":"hello"}`, which the robot ignores. A successful write means the bond is in
    place: save `peripheral.identifier`, print `Paired with StackChan CC`, exit 0.
  - **Robot side, normal:** `retrievePeripherals(withIdentifiers:)` with the saved UUID, then `connect`. A
    pending connect waits for the robot to come in range, so no polling is needed. On subscribe, send
    `relay.robotConnected()` if any. On disconnect, connect again.
  - **Notifications:** go through `Joiner`. A finished message goes through `relay.fromRobot`, then to the server.
  - **Writes:** `.withResponse`, packets from `Frames.split` at `maximumWriteValueLength(for: .withResponse)`. The
    next packet goes after `didWriteValueFor` (one in flight).
  - **No `robot` saved:** log `not paired: run ./setup.sh --pair` and exit 1.
  - **Logging:** log lines go to stderr with a timestamp, like `cc-watch`.
- [ ] **Step 2:** `mac/ble/build.sh` compiles `Frames.swift Relay.swift main.swift` with `-O`. Run it, then
  `mac/ble/test.sh`. Expected: both succeed.
- [ ] **Step 3:** Commit: `Mac: cc-ble, the Bluetooth bridge between the local server and the robot`.

### Task 7: `setup.sh`

**Files:**
- Create: `setup.sh` (repo root), `tests/test_setup.sh`
- Modify:
  - `mac/install.sh`: `GHOSTTY=${GHOSTTY:-true}` is written into `config.json` instead of the literal `true`.
  - `README.md`: setup section for the other person.

**Interfaces:**
- Consumes: `cc-ble` (Task 6), the firmware zip (Task 4), `mac/install.sh`.
- Produces: `./setup.sh` (full install), `./setup.sh --pair`, `./setup.sh --uninstall`,
  `./setup.sh --firmware <zip>` (use a local zip instead of the release), `./setup.sh --dry-run` (prints the
  files it would write to a temp `HOME`, flashes and loads nothing).

- [ ] **Step 1: Write the failing test** `tests/test_setup.sh`. It runs `HOME=$tmp ./setup.sh --dry-run
  --firmware tests/fixtures/fake-firmware.zip` with a matching `CC_FIRMWARE_SHA256` override, then checks:
  - The server plist has `LISTEN_HOST` `127.0.0.1`, `LISTEN_PORT` `8810`, an empty `GEMINI_API_KEY`, and a
    `TZ_OFFSET_MINUTES` equal to the minutes from `date +%z` (for example `+0100` → 60, `-0530` → -330).
  - The plist files are mode 600.
  - The tokens in `ble.json`, the cc-watch config and the server plist agree, and are 32+ hex characters.
  - The cc-watch config has `"ghostty":false`.
  - A wrong `CC_FIRMWARE_SHA256` makes setup exit non-zero before anything is flashed.

  Create the fixture zip in the test itself.
- [ ] **Step 2:** Run `sh tests/test_setup.sh`. Expected: FAIL, because `setup.sh` doesn't exist yet.
- [ ] **Step 3: Implement `setup.sh`.**
  - It checks for `xcode-select -p` and stops with `Run xcode-select --install first` if missing.
  - It generates tokens with `openssl rand -hex 16` and keeps existing ones on re-run.
  - It creates the venv with `aiohttp` and `esptool` and copies `server/` into the support dir.
  - It writes the `dev.stackchan.cc-monitor` LaunchAgent, then builds and installs `cc-watch` via
    `GHOSTTY=false mac/install.sh ws://127.0.0.1:8810/cc/mac <mac token>`, then `cc-ble` and its LaunchAgent.
  - **Firmware:** it downloads
    `https://github.com/siddiqui-zeeshan/stackchan-ccmonitor/releases/download/ble-v1/stackchan-cc-ble.zip`
    (or `--firmware`) and checks it against `firmware/ble-firmware.sha256`. It finds the robot as
    `/dev/cu.usbmodem*` and stops if there are zero or several. It asks `Flash the robot on <port>? [y/N]` and
    flashes with the venv's esptool using the zip's `flash_args`.
  - **Pairing:** it runs `cc-ble --pair` in the foreground, then loads the `cc-ble` LaunchAgent.
  - **Before starting:** if port 8810 is taken (`lsof -iTCP:8810 -sTCP:LISTEN`), it stops with a message naming
    the program on it.
  - `--uninstall` boots out and removes all three LaunchAgents and binaries, keeping `~/.config/cc-monitor`.
- [ ] **Step 4:** Run `sh tests/test_setup.sh` and `sh -n setup.sh`. Expected: PASS.
- [ ] **Step 5:** Commit: `setup.sh: one-step install of the Bluetooth setup on a Mac`.

### Task 8: End to end on the new robot, then the release

- [ ] **Step 1:** Ask the user before flashing. Build `firmware/build.sh --ble --package stackchan-cc-ble.zip` and
  write its SHA-256 to `firmware/ble-firmware.sha256`. On our Mac, run
  `./setup.sh --firmware stackchan-cc-ble.zip`. Our own `dev.stackchan.cc-watch` is replaced by this, so first
  copy `~/.config/cc-watch/config.json` aside, and restore it with `mac/install.sh` at the end.
- [ ] **Step 2: Check each, and record the result:**
  - The robot boots straight into CC Monitor.
  - The pairing code shows, macOS asks for it, and pairing succeeds.
  - Sessions show, with no action buttons.
  - Tap and long press work; the server log shows `touch:`.
  - An `act` message typed into `cc-ble`'s input by a test hook is dropped. Use a temporary log line, not
    committed.
  - After `launchctl kickstart -k` of `cc-ble`, the screen comes back at once (Review Focus 2).
  - After a robot power cycle, it reconnects without a touch.
  - After unplugging USB and restarting, `./setup.sh --pair` fails, and the robot shows `Waiting for your Mac`.
  - With a phone's BLE scanner app, connecting gets no data, and the robot drops the link.
  - With 12 fake sessions (`fake_mac.py`-style feed into the local server), all rows show.
- [ ] **Step 3:** Restore our own `cc-watch` config, check that our robot still shows our sessions from the Linux
  server, and check that `git diff main -- server/` is empty.
- [ ] **Step 4:** Commit the SHA file and README updates. Ask the user, then push the branch through the server
  host and run `gh release create ble-v1 stackchan-cc-ble.zip --target mac-direct-ble` there. Verify with
  `./setup.sh --dry-run` that the download URL works and the checksum matches.
