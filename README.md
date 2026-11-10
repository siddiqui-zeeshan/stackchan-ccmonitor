# StackChan Claude session monitor

Shows every Claude Code session on your Macs on an M5Stack StackChan robot, and lets you answer them from it.

- **Face** follows your sessions: puzzled with a sweat drop when one needs you, calm while working,
  happy with hearts when one finishes, sleepy when there are none.
- **LEDs**: flashing red = needs you, breathing yellow = working, green = all done, dim white = offline.
- **Screen**: status chip, a bottom card cycling through sessions, a slide-up session list (tap the face).
- **Session screen** (tap a session): what it's asking, **Approve** (hold 1 s) / **Reject**, **Talk**
  (hold to speak a prompt, confirm, then it's typed into the session), **Mac** (bring its tab to front).
  Actions work for sessions running in Ghostty; other sessions are view-only.
- **Chimes**: a brief chime when a session needs your input; a longer one, repeating every 2 min (up to 5×,
  tap the head to stop), when that Mac has had no keyboard/mouse activity for 2+ min. Silent in quiet hours.
- **List stripes**: state colour for Ghostty sessions, blue for sessions not in Ghostty (view only).
- **Head panel**: tap = acknowledge, long press = quiet mode.

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
| `server/` | Server: merges Macs, decides mood/LEDs/chime, routes actions, transcribes Talk (Gemini) |
| `firmware/` | Robot app source, patches on top of official StackChan v1.6.2, `build.sh` |
| `tests/` | Server rule tests (`pytest tests`) |

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
- Approve sends **Enter** and Reject sends **Esc** to the session's Ghostty tab. Before sending, the Mac
  re-checks that the session is still waiting on the same prompt in the same tab, otherwise it refuses.
- Talk audio goes robot → server → Gemini (`GEMINI_API_KEY`); models are tried in order (`STT_MODELS`) and
  overloaded ones are skipped. You always see the transcript and confirm before it's sent.
- Tokens/keys live only in `server/.env.local`, `~/.config/cc-watch/config.json` and the robot build
  (`firmware/app_cc_monitor/cc_monitor_config.h`); all are git-ignored.
