# StackChan Claude session monitor

Shows every Claude Code session on your Macs on an M5Stack StackChan robot, and lets you answer them from it.

- **Screen**: the whole screen takes the mood's colour, with a pixel face, one big number (how many sessions
  are in the most urgent state) and one plain sentence about the session that matters most, for example
  "fast-agent-prod wants to run / npm run deploy:staging". With several such sessions the sentence moves to the
  next one every 4 s. Tap the sentence to open that session; tap anywhere else for the session list.

  | Mood | When | Screen and face | Side lights |
  |---|---|---|---|
  | Working | nothing needs you, at least one session is busy | dark screen with a green grid, green eyes glancing | slow colour cycle |
  | Needs you | a session just asked for your answer | red, sweating | flashing red |
  | Still waiting | it has waited 2 min, or you are away from that Mac | orange, gritted teeth | flashing orange |
  | Just finished | a few seconds after a session finishes | yellow smiley with a heart | green |
  | All ready | every session waits for a new prompt | green smiley | green |
  | Nothing running | no Claude sessions | dim blue, asleep | off |
  | Offline | the robot can't reach the server | grey, crossed-out eyes | dim white |

- **Head**: tilted up towards you in every mood except nothing running and offline, where it looks down. A
  light nod whenever the mood changes, and small random "thinking" movements while working (none in quiet
  mode or quiet hours, and none while you hold Talk). While the session list or a session screen is open the
  head holds still, so the screen doesn't move under your finger; it takes up the current pose when you
  close it.
- **Session list** (tap the screen): up to 12 sessions, most urgent first, each with a small face in its
  state's colour. "view only" marks a session that isn't running in Ghostty. The X leaves the app.
- **Session screen** (tap a session): what it's asking, **Approve** (hold 1 s) / **Reject**, **Talk**
  (hold to speak a prompt, confirm, then it's typed into the session), **Mac** (bring its tab to front).
  Actions work for sessions running in Ghostty; other sessions are view-only.
- **Chimes**: a brief chime when a session needs your input; a longer one, repeating every 2 min (up to 5×,
  tap the head to stop), when that Mac has had no keyboard/mouse activity for 2+ min. Silent in quiet hours.
- **Head panel**: tap = acknowledge, long press = quiet mode (a mute mark shows top-left).

No Claude Code config is changed: the Mac monitor only reads `~/.claude/sessions/*.json` (and the tail of a
waiting session's transcript).

```
Mac(s): cc-watch ──ws──▶ server: cc-monitor ◀──ws── robot: CC MONITOR app
        (Rust, ~2 MB)        (Python/aiohttp)           (StackChan firmware app)
```

| Path | What |
|---|---|
| `mac-rs/` | Mac monitor (Rust, default, ~1.8 MB RAM) |
| `mac/` | Swift version of the same monitor (~19 MB RAM) and `install.sh` |
| `server/` | Server: merges Macs, decides mood/LEDs/chime and the sentence wording, routes actions, transcribes Talk (Gemini) |
| `firmware/` | Robot app source (draws its own pixel faces), patches on top of official StackChan v1.6.2, `build.sh` |
| `tests/` | Server rule tests (`pytest tests`) |

Mac monitor tests live in `mac-rs/src/main.rs` (`cd mac-rs && cargo test`): transcript summaries, Ghostty
tab matching, and connecting (token header, giving up on a server that never answers).

## Setup

### 1. Server (run it on an always-on machine; any host reachable by the Macs and the robot)
```sh
git clone https://github.com/siddiqui-zeeshan/stackchan-ccmonitor.git && cd stackchan-ccmonitor/server
cp .env.example .env.local        # then fill it in (see comments; generate tokens as shown there)
chmod 600 .env.local
```
Run with Docker:
```sh
docker compose up -d --build      # listens on :8810
```
or directly:
```sh
python3 -m venv .venv && .venv/bin/pip install -r requirements.txt
set -a; . ./.env.local; set +a; .venv/bin/python cc_monitor.py
```
Check: `curl http://<host>:8810/healthz` → `{"ok": true}`. Browser preview: `http://<host>:8810/cc/preview`.

The server speaks plain `ws://`. To reach it from outside your network, put TLS in front (reverse proxy,
Tailscale Funnel, Cloudflare Tunnel, …) and use `wss://` URLs below.

### 2. Each Mac
```sh
brew install rust                 # once; or skip it to build the Swift version instead
git clone https://github.com/siddiqui-zeeshan/stackchan-ccmonitor.git && cd stackchan-ccmonitor/mac
./install.sh ws://<server-host>:8810/cc/mac <one of MAC_TOKENS>
```
This builds `cc-watch`, installs it to `~/Applications/cc-watch`, writes `~/.config/cc-watch/config.json`, and
starts it at login (LaunchAgent `dev.stackchan.cc-watch`). Log: `~/Library/Logs/cc-watch.log`.
macOS asks once to let it control **Ghostty** (needed for tab names and Approve/Reject/Talk).
Uninstall: `./install.sh --uninstall`.

If the server can't be reached, `cc-watch` retries every 5 s. Each attempt gives up after 10 s per step
(TCP connect, TLS, WebSocket upgrade), so a link that dies mid-connect (the Mac going back to sleep, a
network change) is retried instead of hanging. The log shows `connected` after each successful connect and
`connect failed: …` / `disconnected: …` otherwise. If the robot says "No sessions" while sessions are
running, check that the last of those lines in the log is `connected`.

### 3. Robot firmware
Needs Docker, git, python3, and `pip install esptool`. Connect the robot by USB-C, then:
```sh
cd firmware
./build.sh ws://<server-host>:8810/<ROBOT_SECRET>/cc/robot <ROBOT_TOKEN> --flash
```
`build.sh` clones the official firmware (m5stack/StackChan, v1.6.2), fetches its dependencies, applies
`stackchan.patch` + `xiaozhi.patch`, adds the app with your server URL/token compiled in, builds with the
ESP-IDF Docker image and flashes. Wi-Fi and settings on the robot are kept. Open **CC MONITOR** from the
robot's launcher.

The patches also turn off the firmware's automatic self-update (so the robot doesn't replace this build) and
raise the audio-decode task priority (fixes voice stutter). To go back to stock, flash the official firmware.

## Notes
- The robot only draws what the server sends, so the rules live in `server/cc_monitor.py` and are covered by
  `tests/`: which mood applies, when "needs you" becomes "still waiting" (`STILL_WAITING_S`, or the Mac idle
  longer than `AWAY_IDLE_S`), the big number, and the sentence wording per tool (`VERBS`, `sentence()`).
- A session whose status is `shell` (Claude is idle, but a background command it started, such as a dev
  server, is still running) counts as ready, not working: "working" means the agent itself is busy.
- Faces, colours and head movement are in `firmware/app_cc_monitor/app_cc_monitor.cpp`: `look_of()` (colours),
  `pix::face()` (one face per mood, drawn as 4 px cells into an LVGL canvas), `setMood()` / `updateHead()`
  (head). The firmware has no automated tests; check changes by building and running them on the robot.
  After the first `build.sh` run, a rebuild is quicker with the app files copied into
  `firmware/.work/StackChan/firmware/main/apps/app_cc_monitor/` and `idf.py build` run in the same Docker image.
- Each frame, only the part of the screen that changed is sent to the display, so the faces cost little.
  The working screen can be switched from the green grid to a drifting rainbow (`WORKING_GRID = false` in
  `app_cc_monitor.cpp`); that one redraws the whole display about 11 times a second.
- The app logs each mood change, nod and "thinking" move with the head's position (`[CC-MONITOR]` lines on
  the robot's USB serial console). If the head doesn't move and the log shows
  `[ScsServo] ... invalid current pos: -1` after every move, the servos aren't replying: turn the robot fully
  off and on (a USB reset restarts the main chip but not the servos).
- Browser preview (`/cc/preview`): a rough view of the same state (mood colour, number, sentence, rows). It
  does not draw the pixel faces.
- Approve sends **Enter** and Reject sends **Esc** to the session's Ghostty tab. Before sending, the Mac
  re-checks that the session is still waiting on the same prompt in the same tab, otherwise it refuses.
- Talk audio goes robot → server → Gemini (`GEMINI_API_KEY`); models are tried in order (`STT_MODELS`) and
  overloaded ones are skipped. You always see the transcript and confirm before it's sent.
- Tokens/keys live only in `server/.env.local`, `~/.config/cc-watch/config.json` and the robot build
  (`firmware/app_cc_monitor/cc_monitor_config.h`); all are git-ignored.
