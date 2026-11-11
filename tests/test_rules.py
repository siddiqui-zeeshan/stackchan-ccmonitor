import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "server"))
from cc_monitor import Monitor, sentence  # noqa: E402

NOW = 1_800_000_000.0


def sess(i, status, **kw):
    return {"id": f"s{i}", "project": "p", "name": f"n{i}", "kind": "interactive", "status": status,
            "since": NOW * 1000, **kw}


def test_leds_follow_most_urgent_state():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "idle")]}, now=NOW)
    assert m.display(now=NOW)["leds"] == "green"
    m.apply("mac", {"type": "update", "sessions": [sess(2, "busy")]}, now=NOW)
    assert m.display(now=NOW)["leds"] == "rainbow"
    m.apply("mac", {"type": "update", "sessions": [sess(3, "waiting", waitingFor="input needed")]}, now=NOW)
    d = m.display(now=NOW)
    assert d["leds"] == "flash" and d["mood"] == "needs_you"
    assert d["rows"][0]["detail"] == "needs your input", "waiting sessions sort first"


def test_unknown_status_counts_as_working_and_daemons_are_hidden():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "new-thing"), sess(2, "busy", kind="daemon")]}, now=NOW)
    d = m.display(now=NOW)
    assert d["count"] == 1 and d["rows"][0]["state"] == "working"


def test_brief_chime_once_when_input_needed_while_present():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=5)]}, now=NOW)
    assert m.display(now=NOW)["chime"] == "short"
    assert not m.display(now=NOW + 5)["chime"], "brief chime plays once per prompt"
    assert not m.display(now=NOW + 300)["chime"], "no long chime while you're at the Mac"
    m.apply("mac", {"type": "update", "sessions": [sess(1, "busy")]}, now=NOW + 301)
    m.apply("mac", {"type": "update", "sessions": [sess(1, "waiting", idleSec=3)]}, now=NOW + 302)
    assert m.display(now=NOW + 302)["chime"] == "short", "a new prompt chimes again"


def test_long_chime_when_away_repeats_then_stops_on_ack():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=300)]}, now=NOW)
    assert m.display(now=NOW)["chime"] == "long", "already away: long chime straight away"
    assert not m.display(now=NOW + 60)["chime"], "repeats only every 2 minutes"
    assert m.display(now=NOW + 121)["chime"] == "long"
    m.ack()
    assert not m.display(now=NOW + 300)["chime"]


def test_walking_away_after_prompt_triggers_long_chime():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=2)]}, now=NOW)
    assert m.display(now=NOW)["chime"] == "short"
    m.apply("mac", {"type": "idle", "idleSec": 60}, now=NOW + 60)
    assert not m.display(now=NOW + 60)["chime"]
    m.apply("mac", {"type": "idle", "idleSec": 150}, now=NOW + 150)
    assert m.display(now=NOW + 150)["chime"] == "long", "Mac idle report shows you left"
    m.apply("mac", {"type": "idle", "idleSec": 1}, now=NOW + 400)
    assert not m.display(now=NOW + 400)["chime"], "back at the Mac: no more long chimes"


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
        sess(1, "waiting", since=(NOW - 90) * 1000), sess(2, "busy"), sess(3, "busy")]
        + [sess(i, "idle") for i in range(4, 14)]}, now=NOW)
    d = m.display(now=NOW, quiet_mode=True)
    assert d["counts"] == {"needs_you": 1, "working": 2, "ready": 10}
    assert len(d["rows"]) == 12 and d["more"] == 1, "robot list scrolls through up to 12 sessions"
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


def test_prompt_during_quiet_hours_does_not_chime_after_they_end():
    m = Monitor(quiet_hours=(22, 8), tz_offset_min=0)
    night = 1_800_000_000 - (1_800_000_000 % 86400) + 7 * 3600 + 3500   # 07:58 UTC
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=5)]}, now=night)
    assert not m.display(now=night)["chime"]
    assert not m.display(now=night + 300)["chime"], "08:03: stale prompt stays silent"


# ---------------------------------------------------------------- main screen: stages, number, sentence

def test_needs_you_becomes_still_waiting_after_two_minutes():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=5, ask="Bash: npm test")]}, now=NOW)
    d = m.display(now=NOW)
    assert (d["mood"], d["leds"]) == ("needs_you", "flash")
    assert d["rows"][0]["waitingLong"] is False and d["rows"][0]["say"] == " wants to run"
    d = m.display(now=NOW + 119)
    assert d["mood"] == "needs_you", "under two minutes: still the first stage"
    d = m.display(now=NOW + 125)
    assert (d["mood"], d["leds"]) == ("still_waiting", "orange_flash")
    assert d["rows"][0]["waitingLong"] is True and d["rows"][0]["say"] == " has waited 2 min"
    assert d["rows"][0]["state"] == "needs_you", "still approvable: the row state does not change"


def test_needs_you_becomes_still_waiting_when_you_are_away():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "waiting", idleSec=5, ask="Bash: npm test")]}, now=NOW)
    m.apply("mac", {"type": "idle", "idleSec": 150}, now=NOW + 30)
    d = m.display(now=NOW + 30)
    assert d["mood"] == "still_waiting" and d["rows"][0]["say"] == " is waiting for you"
    m.apply("mac", {"type": "idle", "idleSec": 1}, now=NOW + 40)
    assert m.display(now=NOW + 40)["mood"] == "needs_you", "back at the Mac and under two minutes"


@pytest.mark.parametrize("state, ask, expected", [
    ("needs_you", "Bash: npm run deploy:staging", {"say": " wants to run", "cut": 6}),
    ("working", "Bash: cargo test", {"say": " is running", "cut": 6}),
    ("needs_you", "Edit: /Users/z/proj/src/policy/rules.ts", {"say": " wants to edit", "cut": 6, "what": "policy/rules.ts"}),
    ("working", "Read: controller/replay.go", {"say": " is reading", "cut": 6}),
    ("working", "Grep: handleTimeout", {"say": " is searching", "cut": 6}),
    ("working", "Bash: /usr/bin/make all", {"say": " is running", "cut": 6}),
    ("needs_you", "mcp__slack__send_message: #general", {"say": " wants to use send_message", "cut": 26}),
    ("working", "mcp__slack__send_message: #general", {"say": " is using send_message", "cut": 26}),
    ("needs_you", "AskUserQuestion:", {"say": " has a question", "cut": 16, "what": "answer it in the terminal"}),
    ("needs_you", "ExitPlanMode:", {"say": " has a plan to approve", "cut": 13, "what": "review it in the terminal"}),
    ("needs_you", "Which database should I use?", {"say": " is asking", "cut": 0}),
    ("working", "Note: checking the logs next", {"say": " is working", "cut": 0}),
    ("working", "", {"say": " is working", "cut": 0}),
    ("needs_you", "", {"say": " needs you", "cut": 0, "what": "needs your input"}),
    ("ready", "…Tests pass. Open the PR?", {"say": " is ready", "cut": 0}),
])
def test_sentence_wording(state, ask, expected):
    assert sentence(state, ask, "needs your input") == expected


def test_rows_carry_the_sentence_for_the_robot():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [
        sess(1, "busy", ask="Edit: /Users/z/proj/src/policy/rules.ts"), sess(2, "idle", ask="Done.")]}, now=NOW)
    rows = {r["session"]: r for r in m.display(now=NOW)["rows"]}
    assert (rows["s1"]["say"], rows["s1"]["cut"], rows["s1"]["what"]) == (" is editing", 6, "policy/rules.ts")
    assert rows["s2"]["say"] == " is ready" and "what" not in rows["s2"]


@pytest.mark.parametrize("statuses, big, rest", [
    (["waiting", "waiting", "busy", "busy", "busy", "idle"], [2, "need you"], ["3 working", "1 ready"]),
    (["waiting", "idle"], [1, "needs you"], ["1 ready"]),
    (["waiting"], [1, "needs you"], []),
    (["busy", "busy", "busy", "idle", "idle"], [3, "working"], ["2 ready"]),
    (["busy"], [1, "working"], []),
    (["idle"] * 5, [5, "ready"], []),
    ([], None, []),
])
def test_big_number_shows_the_most_urgent_group(statuses, big, rest):
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(i, st) for i, st in enumerate(statuses)]}, now=NOW)
    d = m.display(now=NOW + 60)
    assert (d["big"], d["rest"]) == (big, rest)


def test_spot_sentence_when_no_single_session_is_cycling():
    m = Monitor()
    assert m.display(now=NOW)["spot"] == {"bold": "Nothing running", "text": "", "sub": "", "session": ""}
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "busy"), sess(2, "idle"), sess(3, "idle")]}, now=NOW)
    assert m.display(now=NOW)["spot"] is None, "working: the robot cycles through the rows itself"
    m.apply("mac", {"type": "update", "sessions": [sess(1, "idle", ask="Tests pass. Open the PR?")]}, now=NOW + 10)
    assert m.display(now=NOW + 11)["spot"] == {
        "bold": "n1", "text": " just finished", "sub": "Tests pass. Open the PR?", "session": "s1"}
    assert m.display(now=NOW + 30)["spot"] == {
        "bold": "All 3", "text": " are ready", "sub": "for your next prompt", "session": ""}


def test_spot_sentence_for_a_single_ready_session():
    m = Monitor()
    m.apply("mac", {"type": "snapshot", "sessions": [sess(1, "idle", ask="Deployed to staging.")]}, now=NOW)
    assert m.display(now=NOW + 30)["spot"] == {
        "bold": "n1", "text": " is ready", "sub": "Deployed to staging.", "session": "s1"}


def test_hush_is_set_in_quiet_mode_and_quiet_hours():
    """The robot holds its head still (no "thinking" moves) whenever chimes are silenced."""
    m = Monitor(quiet_hours=(22, 8), tz_offset_min=0)
    day = 1_800_000_000 - (1_800_000_000 % 86400) + 12 * 3600
    night = day + 11 * 3600
    assert m.display(now=day)["hush"] is False
    assert m.display(now=day, quiet_mode=True)["hush"] is True
    assert m.display(now=night)["hush"] is True
