"""cc-monitor: merges Claude Code session state from Macs and drives the StackChan display.

  WS  /cc/mac                 Macs (cc-watch) send snapshots/updates   (Authorization: Bearer <mac token>)
  WS  /<secret>/cc/robot      the robot receives display states        (Authorization: Bearer <robot token>)
  GET /cc/preview             browser preview of the robot's display   (?token=<preview token> if set)
  GET /healthz

The robot draws only what display() returns: mood (needs_you, still_waiting, working, just_finished,
all_done, none), leds, chime, the big number + its caption, the other counts, and per row the wording of
the main-screen sentence ("say"/"cut"/"what", see sentence()). "spot" is that sentence when it is not
about one of the cycling rows.

Configuration (environment): LISTEN_HOST, LISTEN_PORT, MAC_TOKENS (comma-separated), ROBOT_TOKEN,
ROBOT_SECRET, PREVIEW_TOKEN (optional), QUIET_HOURS (e.g. "22-8", empty = none), TZ_OFFSET_MINUTES.
"""

from __future__ import annotations

import asyncio
import hmac
import json
import logging
import os
import re
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any

import aiohttp
from aiohttp import WSMsgType, web

log = logging.getLogger("cc-monitor")

AWAY_IDLE_S = 120          # Mac idle (no keyboard/mouse) longer than this = you are away
CHIME_REPEAT_S = 120
CHIME_MAX = 5
JUST_FINISHED_S = 20       # how long the "just finished" face (heart) stays once the last busy session finishes
STILL_WAITING_S = 120      # a prompt left unanswered this long (or while you are away) = "still waiting"
HEARTBEAT_TIMEOUT_S = 180  # three missed 60 s heartbeats
OFFLINE_DROP_S = 300
MAX_ROWS = 12

WAITING_TEXT = {
    "input needed": "needs your input",
    "dialog open": "dialog open",
    "sandbox request": "wants network access",
    "goal proposal": "goal to approve",
}
LEDS = {"needs_you": "flash", "still_waiting": "orange_flash", "working": "rainbow", "all_done": "green",
        "none": "off", "offline": "dim_white"}
RANK = {"needs_you": 0, "working": 1, "ready": 2}

# Tool -> wording for the robot's main-screen sentence: (waiting for your approval, working).
_EDIT = (" wants to edit", " is editing")
_SEARCH = (" wants to search", " is searching")
_AGENT = (" wants to start an agent", " started an agent")
VERBS = {
    "Bash": (" wants to run", " is running"),
    "Edit": _EDIT, "Write": _EDIT, "MultiEdit": _EDIT, "NotebookEdit": _EDIT,
    "Read": (" wants to read", " is reading"),
    "Grep": _SEARCH, "Glob": _SEARCH,
    "WebFetch": (" wants to open", " is opening"),
    "WebSearch": (" wants to search the web", " is searching the web"),
    "Agent": _AGENT, "Task": _AGENT,
    "AskUserQuestion": (" has a question", " has a question"),
    "ExitPlanMode": (" has a plan to approve", " is planning"),
}
FILE_TOOLS = {"Edit", "Write", "MultiEdit", "NotebookEdit", "Read"}
NO_ARG_HINT = {"AskUserQuestion": "answer it in the terminal", "ExitPlanMode": "review it in the terminal"}
_TOOL_ASK = re.compile(r"^([A-Za-z_][\w-]*):(?: (.*))?$", re.S)


def sentence(state: str, ask: str, detail: str) -> dict[str, Any]:
    """Wording for the robot's main screen: "<name><say>" on one line, then what it is about.

    `ask` is cc-watch's summary: "Tool: argument" for a tool call, otherwise plain text. The second line
    is `ask` minus its first `cut` bytes (the "Tool: " prefix), unless `what` replaces it.
    """
    if state == "ready":
        return {"say": " is ready", "cut": 0}
    waiting = state == "needs_you"
    m = _TOOL_ASK.match(ask)
    tool, arg = (m.group(1), m.group(2) or "") if m else ("", "")
    if tool in VERBS:
        say = VERBS[tool][0 if waiting else 1]
    elif tool.startswith("mcp__"):
        say = (" wants to use " if waiting else " is using ") + tool.rsplit("__", 1)[-1]
    else:   # plain text (or a word followed by a colon that is not a tool)
        out: dict[str, Any] = {"say": (" is asking" if ask else " needs you") if waiting else " is working", "cut": 0}
        if waiting and not ask:
            out["what"] = detail
        return out
    out = {"say": say, "cut": len(tool) + 2 if arg else len(ask.encode())}
    if not arg and tool in NO_ARG_HINT:
        out["what"] = NO_ARG_HINT[tool]
    elif tool in FILE_TOOLS and arg.startswith(("/", "~/")) and arg.count("/") > 2:
        out["what"] = "/".join(arg.split("/")[-2:])   # long path: keep the end, which is what you recognise
    return out


def state_of(status: str | None) -> str:
    """Session file status -> robot state. Unknown values count as working (format may change)."""
    if status == "waiting":
        return "needs_you"
    if status in ("idle", "shell"):
        # "shell": Claude itself is idle, but a background command it started (a dev server, a watcher, a
        # long build) is still running. Showing that as working would suggest the agent is doing something.
        return "ready"
    return "working"  # busy, or anything new


@dataclass
class Session:
    host: str
    id: str
    data: dict[str, Any]
    state: str
    since: float                       # epoch seconds the current state started
    acked: bool = False
    chimes: int = 0
    last_chime: float = 0.0
    short_chimed: bool = False         # brief "input needed" chime played for this prompt


@dataclass
class Mac:
    host: str
    last_seen: float
    ws: web.WebSocketResponse | None = None
    idle_sec: int = 0                  # last reported keyboard/mouse idle time (cc-watch reports it while waiting)


@dataclass
class Monitor:
    quiet_hours: tuple[int, int] | None = None
    tz_offset_min: int = 0
    sessions: dict[tuple[str, str], Session] = field(default_factory=dict)
    macs: dict[str, Mac] = field(default_factory=dict)
    finished_at: float = 0.0           # last busy -> idle transition, for the "just finished" nod
    finished_key: tuple[str, str] | None = None   # the session that made that transition

    # ---------------------------------------------------------------- input from Macs

    def apply(self, host: str, msg: dict[str, Any], now: float | None = None) -> None:
        now = time.time() if now is None else now
        mac = self.macs.setdefault(host, Mac(host, now))
        mac.last_seen = now
        kind = msg.get("type")
        if kind == "snapshot":
            live = {s["id"] for s in msg.get("sessions", []) if s.get("id")}
            for key in [k for k in self.sessions if k[0] == host and k[1] not in live]:
                del self.sessions[key]
            for s in msg.get("sessions", []):
                self._upsert(host, s, now)
        elif kind == "update":
            for s in msg.get("sessions", []):
                self._upsert(host, s, now)
        elif kind == "gone":
            self.sessions.pop((host, str(msg.get("id"))), None)
        elif kind == "idle":
            mac.idle_sec = int(msg.get("idleSec") or 0)
        elif kind == "heartbeat":
            live = set(msg.get("ids", []))
            for key in [k for k in self.sessions if k[0] == host and k[1] not in live]:
                del self.sessions[key]

    def _upsert(self, host: str, s: dict[str, Any], now: float) -> None:
        if not s.get("id") or s.get("kind") in ("daemon", "daemon-worker"):
            return
        key = (host, s["id"])
        if s.get("idleSec") is not None:
            self.macs[host].idle_sec = int(s["idleSec"])
        new_state = state_of(s.get("status"))
        since = (s.get("since") or now * 1000) / 1000
        old = self.sessions.get(key)
        if old is None or old.state != new_state:
            if old is not None and old.state == "working" and new_state == "ready":
                self.finished_at = now
                self.finished_key = key
            self.sessions[key] = Session(host, s["id"], s, new_state, since)
        else:
            old.data = s

    def expire(self, now: float | None = None) -> None:
        now = time.time() if now is None else now
        for host, mac in list(self.macs.items()):
            if now - mac.last_seen > HEARTBEAT_TIMEOUT_S + OFFLINE_DROP_S:
                for key in [k for k in self.sessions if k[0] == host]:
                    del self.sessions[key]
                del self.macs[host]

    def offline(self, host: str, now: float) -> bool:
        mac = self.macs.get(host)
        return mac is None or now - mac.last_seen > HEARTBEAT_TIMEOUT_S

    # ---------------------------------------------------------------- output to the robot

    def _quiet(self, now: float) -> bool:
        if not self.quiet_hours:
            return False
        hour = int(((now / 60 + self.tz_offset_min) // 60) % 24)
        start, end = self.quiet_hours
        return (start <= hour or hour < end) if start > end else (start <= hour < end)

    def display(self, now: float | None = None, quiet_mode: bool = False) -> dict[str, Any]:
        """The whole display state the robot draws (also used by the preview)."""
        now = time.time() if now is None else now
        rows = sorted(self.sessions.values(),
                      key=lambda s: (RANK[s.state], s.since if s.state == "needs_you" else -s.since))
        long_wait = {(s.host, s.id) for s in rows if self._long_wait(s, now)}
        if any(s.state == "needs_you" for s in rows):
            mood = "still_waiting" if long_wait else "needs_you"
        elif any(s.state == "working" for s in rows):
            mood = "working"
        elif rows:
            mood = "just_finished" if now - self.finished_at < JUST_FINISHED_S else "all_done"
        else:
            mood = "none"
        # "short": once when a session starts needing input. "long": while you're away from that Mac
        # (no keyboard/mouse for AWAY_IDLE_S), repeating every CHIME_REPEAT_S, up to CHIME_MAX, until acked.
        chime = ""
        if quiet_mode or self._quiet(now):
            for s in rows:   # a prompt that arrives while quiet doesn't chime later
                if s.state == "needs_you":
                    s.short_chimed = True
        else:
            for s in rows:
                if s.state != "needs_you":
                    continue
                mac = self.macs.get(s.host)
                away = mac is not None and mac.idle_sec > AWAY_IDLE_S
                if (away and not s.acked and s.chimes < CHIME_MAX
                        and now - s.last_chime >= CHIME_REPEAT_S):
                    chime = "long"
                    s.chimes += 1
                    s.last_chime = now
                    s.short_chimed = True
                elif not s.short_chimed:
                    chime = chime or "short"
                    s.short_chimed = True
        led_mood = "all_done" if mood == "just_finished" else mood
        counts = {k: sum(1 for s in rows if s.state == k) for k in ("needs_you", "working", "ready")}
        n, w, r = counts["needs_you"], counts["working"], counts["ready"]
        # The big number is the most urgent group; the rest are listed small beside it.
        if n:
            big, rest = [n, "needs you" if n == 1 else "need you"], [f"{w} working"] * bool(w) + [f"{r} ready"] * bool(r)
        elif w:
            big, rest = [w, "working"], [f"{r} ready"] * bool(r)
        else:
            big, rest = ([r, "ready"] if r else None), []
        return {
            "type": "display",
            "mood": mood,
            "leds": LEDS[led_mood],
            "chime": chime,
            "rows": [self._row(s, now, (s.host, s.id) in long_wait) for s in rows[:MAX_ROWS]],
            "more": max(0, len(rows) - MAX_ROWS),
            "count": len(rows),
            "counts": counts,
            "big": big,
            "rest": rest,
            "spot": self._spot(mood, rows),
            "quiet": quiet_mode,
            "hush": quiet_mode or self._quiet(now),
        }

    def _long_wait(self, s: Session, now: float) -> bool:
        """A prompt you have left for a while, or that arrived while you are away from that Mac."""
        if s.state != "needs_you":
            return False
        mac = self.macs.get(s.host)
        return now - s.since >= STILL_WAITING_S or (mac is not None and mac.idle_sec > AWAY_IDLE_S)

    def _spot(self, mood: str, rows: list[Session]) -> dict[str, str] | None:
        """The main-screen sentence when it is not about one of the cycling rows (those use row["say"])."""
        if mood == "none":
            return {"bold": "Nothing running", "text": "", "sub": "", "session": ""}
        if mood == "just_finished":
            s = self.sessions.get(self.finished_key) if self.finished_key else None
            if s is not None:
                return {"bold": _title(s), "text": " just finished", "sub": s.data.get("ask") or "", "session": s.id}
        if mood in ("just_finished", "all_done"):
            if len(rows) == 1:
                s = rows[0]
                return {"bold": _title(s), "text": " is ready",
                        "sub": s.data.get("ask") or "for your next prompt", "session": s.id}
            return {"bold": f"All {len(rows)}", "text": " are ready", "sub": "for your next prompt", "session": ""}
        return None

    def _row(self, s: Session, now: float, long_wait: bool = False) -> dict[str, Any]:
        d = s.data
        detail = {"needs_you": WAITING_TEXT.get(d.get("waitingFor") or "", "needs you"),
                  "working": "working", "ready": "ready"}[s.state]
        age = max(0, int(now - s.since))
        words = sentence(s.state, d.get("ask") or "", detail)
        if long_wait:
            words["say"] = f" has waited {age // 60} min" if age >= STILL_WAITING_S else " is waiting for you"
        return {
            "session": s.id, "host": s.host, "project": d.get("project") or "?",
            "name": d.get("name") or "", "tab": d.get("tab") or "", "state": s.state,
            "detail": detail, "since": int(s.since * 1000), "ghostty": bool(d.get("terminal")),
            "offline": self.offline(s.host, now), "ageSec": age,
            "ask": d.get("ask") or "", "waitingLong": long_wait, **words,
        }

    def action_for(self, session_id: str, action: str, text: str = "") -> tuple[str | None, Any]:
        """Robot action -> (host, message for that Mac) or (None, reason). The Mac re-checks `since`."""
        s = next((s for s in self.sessions.values() if s.id == session_id), None)
        if s is None:
            return None, "unknown session"
        if action in ("approve", "reject") and s.state != "needs_you":
            return None, "not waiting"
        if not s.data.get("terminal"):
            return None, "not in Ghostty"
        return s.host, {"type": "act", "action": action, "session": s.id, "terminal": s.data["terminal"],
                        "since": s.data.get("since"), "text": text}

    def ack(self) -> None:
        for s in self.sessions.values():
            if s.state == "needs_you":
                s.acked = True


def _title(s: Session) -> str:
    return s.data.get("name") or s.data.get("project") or "session"


# -------------------------------------------------------------------- web app

MON = web.AppKey("monitor", Monitor)
ROBOTS = web.AppKey("robots", set)
CFG = web.AppKey("cfg", dict)


def _bearer(request: web.Request) -> str:
    h = request.headers.get("Authorization", "")
    return h[7:] if h.startswith("Bearer ") else (request.query.get("token") or "")


def _ok(token: str, allowed: list[str]) -> bool:
    return any(hmac.compare_digest(token.encode(), a.encode()) for a in allowed if a)


async def push(app: web.Application) -> None:
    """Send the current display state to every robot and preview, if it changed."""
    state = app[MON].display(quiet_mode=app[CFG]["quiet_mode"])
    text = json.dumps(state, sort_keys=True)
    if text == app[CFG].get("last_sent") and not state["chime"]:
        return
    app[CFG]["last_sent"] = text
    for ws in list(app[ROBOTS]):
        if not ws.closed:
            await ws.send_str(text)


async def mac_ws(request: web.Request) -> web.StreamResponse:
    cfg = request.app[CFG]
    if not _ok(_bearer(request), cfg["mac_tokens"]):
        raise web.HTTPNotFound()
    ws = web.WebSocketResponse(heartbeat=30)
    await ws.prepare(request)
    host = "?"
    async for msg in ws:
        if msg.type != WSMsgType.TEXT:
            continue
        try:
            data = json.loads(msg.data)
        except json.JSONDecodeError:
            continue
        if data.get("type") == "result":   # outcome of an approve/reject/send, shown on the robot
            await _to_robots(request.app, data)
            continue
        host = str(data.get("host") or host)
        mac = request.app[MON].macs.setdefault(host, Mac(host, time.time()))
        mac.ws = ws
        request.app[MON].apply(host, data)
        log.info("mac %s: %s (%d sessions)", host, data.get("type"), len(data.get("sessions", [])))
        await push(request.app)
    log.info("mac %s disconnected", host)
    return ws


async def robot_ws(request: web.Request) -> web.StreamResponse:
    cfg = request.app[CFG]
    if not _ok(_bearer(request), [cfg["robot_token"]]):
        raise web.HTTPNotFound()
    return await _display_socket(request, can_touch=True, can_act=True)


async def preview_ws(request: web.Request) -> web.StreamResponse:
    cfg = request.app[CFG]
    if cfg["preview_token"] and not _ok(_bearer(request), [cfg["preview_token"]]):
        raise web.HTTPNotFound()
    return await _display_socket(request, can_touch=True)


async def _display_socket(request: web.Request, can_touch: bool, can_act: bool = False) -> web.StreamResponse:
    """Robot and preview socket. Only the robot (can_act) may approve, reject, type, focus or talk."""
    ws = web.WebSocketResponse(heartbeat=45)
    await ws.prepare(request)
    app = request.app
    app[ROBOTS].add(ws)
    await ws.send_str(json.dumps(app[MON].display(quiet_mode=app[CFG]["quiet_mode"]), sort_keys=True))
    audio = bytearray()
    ptt: dict[str, Any] = {}
    try:
        async for msg in ws:
            if msg.type == WSMsgType.BINARY:
                if can_act and ptt and len(audio) < 60 * 2 * int(ptt.get("rate", 16000)):   # 60 s cap
                    audio.extend(msg.data)
                continue
            if msg.type != WSMsgType.TEXT or not can_touch:
                continue
            data = json.loads(msg.data)
            kind = data.get("type")
            if kind == "touch":
                await handle_touch(app, data)
            elif not can_act:
                continue
            elif kind in ("act", "send", "focus"):
                action = {"send": "send", "focus": "focus"}.get(kind, data.get("action", ""))
                await route_action(app, ws, str(data.get("session")), action, str(data.get("text") or ""))
            elif kind == "ptt_start":
                audio.clear()
                ptt = {"session": data.get("session"), "rate": int(data.get("rate") or 16000)}
            elif kind == "ptt_end" and ptt:
                pcm, info = bytes(audio), ptt
                audio.clear()
                ptt = {}
                asyncio.create_task(transcribe_and_reply(app, ws, info, pcm))
    finally:
        app[ROBOTS].discard(ws)
    return ws


async def _to_robots(app: web.Application, data: dict[str, Any]) -> None:
    text = json.dumps(data)
    for ws in list(app[ROBOTS]):
        if not ws.closed:
            await ws.send_str(text)


async def route_action(app: web.Application, ws, session: str, action: str, text: str) -> None:
    mon = app[MON]
    host, msg = mon.action_for(session, "send" if action == "focus" else action, text)
    if host is None:
        await ws.send_str(json.dumps({"type": "result", "session": session, "ok": False, "error": msg}))
        return
    if action == "focus":
        msg = {"type": "focus", "terminal": msg["terminal"]}
    mac = mon.macs.get(host)
    if not mac or not mac.ws or mac.ws.closed:
        await ws.send_str(json.dumps({"type": "result", "session": session, "ok": False, "error": "Mac offline"}))
        return
    log.info("robot -> %s: %s %s", host, action, session)
    await mac.ws.send_str(json.dumps(msg))


def _wav(pcm: bytes, rate: int) -> bytes:
    import struct
    return (b"RIFF" + struct.pack("<I", 36 + len(pcm)) + b"WAVEfmt " +
            struct.pack("<IHHIIHH", 16, 1, 1, rate, rate * 2, 2, 16) + b"data" + struct.pack("<I", len(pcm)) + pcm)


async def transcribe(pcm: bytes, rate: int) -> str:
    """Speech -> text with Gemini (GEMINI_API_KEY). Returns "" when nothing was said."""
    import base64
    key = os.environ.get("GEMINI_API_KEY", "")
    if not key:
        raise RuntimeError("GEMINI_API_KEY not set")
    body = {"contents": [{"parts": [
        {"text": "You are a speech-to-text engine. Transcribe the spoken words in this audio verbatim. "
                 "Never invent, complete or summarise anything. If there is no clearly spoken human speech "
                 "(silence, noise, tones, music), output exactly NO_SPEECH. Output only the transcript."},
        {"inline_data": {"mime_type": "audio/wav", "data": base64.b64encode(_wav(pcm, rate)).decode()}}]}]}
    # Fallback chain: preview/new models are often overloaded, so move on rather than fail.
    models = [m.strip() for m in os.environ.get(
        "STT_MODELS", "gemini-3.5-flash,gemini-flash-lite-latest,gemini-3.8-flash,gemini-flash-latest").split(",") if m.strip()]
    last_err = "no model available"
    j: dict[str, Any] = {}
    async with aiohttp.ClientSession() as s:
        for model in models:
            url = f"https://generativelanguage.googleapis.com/v1beta/models/{model}:generateContent"
            async with s.post(url, json=body, headers={"x-goog-api-key": key},
                              timeout=aiohttp.ClientTimeout(total=30)) as r:
                j = await r.json()
                if r.status == 200:
                    break
                last_err = f"{model}: {j.get('error', {}).get('message') or r.status}"
                log.warning("stt %s", last_err)
                if r.status not in (404, 429, 500, 503):
                    raise RuntimeError(last_err)
        else:
            raise RuntimeError(last_err)
    parts = j.get("candidates", [{}])[0].get("content", {}).get("parts", [])
    text = "".join(p.get("text", "") for p in parts).strip()
    return "" if "NO_SPEECH" in text else text


async def transcribe_and_reply(app: web.Application, ws, info: dict[str, Any], pcm: bytes) -> None:
    session = info.get("session")
    try:
        if len(pcm) < info["rate"] // 2:   # under 0.25 s
            raise RuntimeError(f"too short ({len(pcm)} bytes)")
        text = await transcribe(pcm, info["rate"])
        log.info("transcribed %.1f s for %s: %r", len(pcm) / 2 / info["rate"], session, text[:80])
        reply = {"type": "transcript", "session": session, "text": text}
    except Exception as e:  # noqa: BLE001 - report any failure to the robot
        log.warning("transcription failed: %s", e)
        reply = {"type": "transcript", "session": session, "text": "", "error": str(e)}
    if not ws.closed:
        await ws.send_str(json.dumps(reply))


async def handle_touch(app: web.Application, data: dict[str, Any]) -> None:
    mon, gesture = app[MON], data.get("gesture")
    if gesture == "tap":
        mon.ack()
    elif gesture == "long_press":
        app[CFG]["quiet_mode"] = not app[CFG]["quiet_mode"]
    elif gesture == "double_tap":
        rows = mon.display()["rows"]
        idx = int(data.get("row", 0))
        if 0 <= idx < len(rows):
            row = rows[idx]
            sess = mon.sessions.get((row["host"], row["session"]))
            mac = mon.macs.get(row["host"])
            if sess and mac and mac.ws and not mac.ws.closed and sess.data.get("terminal"):
                await mac.ws.send_str(json.dumps({"type": "focus", "terminal": sess.data["terminal"]}))
    log.info("touch: %s (quiet mode %s)", gesture, "on" if app[CFG]["quiet_mode"] else "off")
    app[CFG]["last_sent"] = None
    await push(app)


async def preview(request: web.Request) -> web.Response:
    cfg = request.app[CFG]
    if cfg["preview_token"] and not _ok(_bearer(request), [cfg["preview_token"]]):
        raise web.HTTPNotFound()
    return web.FileResponse(Path(__file__).with_name("preview.html"))


async def healthz(_: web.Request) -> web.Response:
    return web.json_response({"ok": True})


def make_app(cfg: dict[str, Any]) -> web.Application:
    app = web.Application()
    app[CFG] = {**cfg, "quiet_mode": False, "last_sent": None}
    app[MON] = Monitor(quiet_hours=cfg.get("quiet_hours"), tz_offset_min=cfg.get("tz_offset_min", 0))
    app[ROBOTS] = set()

    async def ticker(app: web.Application):
        async def loop():
            while True:
                await asyncio.sleep(5)
                app[MON].expire()
                await push(app)   # time-based changes: chime repeats, just-finished fade, offline
        task = asyncio.create_task(loop())
        yield
        task.cancel()

    app.cleanup_ctx.append(ticker)
    app.router.add_get("/cc/mac", mac_ws)
    app.router.add_get(f"/{cfg['robot_secret']}/cc/robot", robot_ws)
    app.router.add_get("/cc/preview", preview)
    app.router.add_get("/cc/preview/ws", preview_ws)
    app.router.add_get("/healthz", healthz)
    return app


def config_from_env() -> dict[str, Any]:
    e = os.environ
    qh = e.get("QUIET_HOURS", "22-8").strip()
    quiet = tuple(int(x) for x in qh.split("-")) if qh else None
    cfg = {
        "host": e.get("LISTEN_HOST", "0.0.0.0"),
        "port": int(e.get("LISTEN_PORT", "8810")),
        "mac_tokens": [t.strip() for t in e.get("MAC_TOKENS", "").split(",") if t.strip()],
        "robot_token": e.get("ROBOT_TOKEN", ""),
        "robot_secret": e.get("ROBOT_SECRET", ""),
        "preview_token": e.get("PREVIEW_TOKEN", ""),
        "quiet_hours": quiet,
        "tz_offset_min": int(e.get("TZ_OFFSET_MINUTES", "330")),
    }
    if not cfg["mac_tokens"] or not cfg["robot_token"] or not cfg["robot_secret"]:
        raise SystemExit("set MAC_TOKENS, ROBOT_TOKEN and ROBOT_SECRET")
    return cfg


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s")
    cfg = config_from_env()
    web.run_app(make_app(cfg), host=cfg["host"], port=cfg["port"], print=None)
