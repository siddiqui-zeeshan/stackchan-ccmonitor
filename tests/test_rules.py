import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from cc_monitor import Monitor  # noqa: E402

NOW = 1_800_000_000.0


def sess(i, status, **kw):
    return {"id": f"s{i}", "project": "p", "name": f"n{i}", "kind": "interactive", "status": status,
            "since": NOW * 1000, **kw}


def test_leds_follow_most_urgent_state():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "idle")]}, now=NOW)
    assert m.display(now=NOW)["leds"] == "green"
    m.apply("mac", {"type": "update", "sessions": [sess(2, "busy")]}, now=NOW)
    assert m.display(now=NOW)["leds"] == "yellow_breathe"
    m.apply("mac", {"type": "update", "sessions": [sess(3, "waiting", waitingFor="input needed")]}, now=NOW)
    d = m.display(now=NOW)
    assert d["leds"] == "flash" and d["mood"] == "needs_you"
    assert d["rows"][0]["detail"] == "needs your input", "waiting sessions sort first"


def test_unknown_status_counts_as_working_and_daemons_are_hidden():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "new-thing"), sess(2, "busy", kind="daemon")]}, now=NOW)
    d = m.display(now=NOW)
    assert d["count"] == 1 and d["rows"][0]["state"] == "working"


def test_chime_only_when_away_repeats_then_stops_on_ack():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=30)]}, now=NOW)
    assert not m.display(now=NOW)["chime"], "present: silent nudge only"
    m.apply("mac", {"type": "update", "sessions": [sess(1, "busy")]}, now=NOW)
    m.apply("mac", {"type": "update", "sessions": [sess(1, "waiting", idleSec=300)]}, now=NOW)
    assert m.display(now=NOW)["chime"]
    assert not m.display(now=NOW + 60)["chime"], "repeats only every 2 minutes"
    assert m.display(now=NOW + 121)["chime"]
    m.ack()
    assert not m.display(now=NOW + 300)["chime"]


def test_quiet_hours_suppress_chime():
    m = Monitor(quiet_hours=(22, 8), tz_offset_min=0)
    night = 1_800_000_000 - (1_800_000_000 % 86400) + 23 * 3600   # 23:00 UTC
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=999)]}, now=night)
    assert not m.display(now=night)["chime"]


def test_just_finished_then_all_done_and_gone():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "busy")]}, now=NOW)
    m.apply("mac", {"type": "update", "sessions": [sess(1, "idle")]}, now=NOW)
    assert m.display(now=NOW + 1)["mood"] == "just_finished"
    assert m.display(now=NOW + 10)["mood"] == "all_done"
    m.apply("mac", {"type": "gone", "id": "s1"}, now=NOW)
    assert m.display(now=NOW)["mood"] == "none"


def test_snapshot_drops_sessions_that_closed_while_disconnected():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "busy"), sess(2, "idle")]}, now=NOW)
    m.apply("mac", {"type": "snapshot", "sessions": [sess(2, "idle")]}, now=NOW)
    assert [r["session"] for r in m.display(now=NOW)["rows"]] == ["s2"]


def test_display_has_counts_ages_and_quiet_flag_for_robot_ui():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [
        sess(1, "waiting", since=(NOW - 90) * 1000), sess(2, "busy"), sess(3, "busy"),
        sess(4, "idle"), sess(5, "idle")]}, now=NOW)
    d = m.display(now=NOW, quiet_mode=True)
    assert d["counts"] == {"needs_you": 1, "working": 2, "ready": 2}
    assert len(d["rows"]) == 4 and d["more"] == 1
    assert d["rows"][0]["ageSec"] == 90
    assert d["quiet"] is True


def test_robot_action_routes_to_owning_mac_with_prompt_timestamp():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [
        sess(1, "waiting", terminal="ghostty:T1", ask="Bash: npm test"), sess(2, "busy")]}, now=NOW)
    host, msg = m.action_for("s1", "approve")
    assert host == "mac" and msg == {"type": "act", "action": "approve", "session": "s1",
                                     "terminal": "ghostty:T1", "since": NOW * 1000, "text": ""}
    assert m.display(now=NOW)["rows"][0]["ask"] == "Bash: npm test"
    assert m.action_for("s2", "approve") == (None, "not waiting"), "only waiting sessions can be approved"
    assert m.action_for("s2", "send", "hi") == (None, "not in Ghostty")
    assert m.action_for("nope", "reject") == (None, "unknown session")


def test_preview_socket_cannot_approve_or_type():
    """The browser preview is view-only: actions from it must never reach a Mac."""
    import asyncio
    import json as _json
    from aiohttp.test_utils import TestClient, TestServer
    from cc_monitor import make_app

    async def run():
        cfg = {"mac_tokens": ["m"], "robot_token": "r", "robot_secret": "sec", "preview_token": ""}
        async with TestClient(TestServer(make_app(cfg))) as c:
            mac = await c.ws_connect("/cc/mac", headers={"Authorization": "Bearer m"})
            await mac.send_str(_json.dumps({"type": "snapshot", "host": "h", "sessions": [
                sess(1, "waiting", terminal="ghostty:T1")]}))
            await asyncio.sleep(0.1)
            prev = await c.ws_connect("/cc/preview/ws")
            await prev.receive()
            for m in ({"type": "act", "action": "approve", "session": "s1"},
                      {"type": "send", "session": "s1", "text": "rm -rf ~"}):
                await prev.send_str(_json.dumps(m))
            got = []
            try:
                while True:
                    got.append(await asyncio.wait_for(mac.receive(), 0.3))
            except asyncio.TimeoutError:
                pass
            assert not [g for g in got if '"act"' in str(g.data)], got
            robot = await c.ws_connect("/sec/cc/robot", headers={"Authorization": "Bearer r"})
            await robot.receive()
            await robot.send_str(_json.dumps({"type": "act", "action": "approve", "session": "s1"}))
            m = await asyncio.wait_for(mac.receive(), 2)
            assert _json.loads(m.data)["action"] == "approve", "robot actions still work"
    asyncio.run(run())
