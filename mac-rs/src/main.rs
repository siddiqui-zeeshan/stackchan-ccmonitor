//! cc-watch (Rust): reports live Claude Code session state to cc-monitor with a minimal footprint.
//!
//! Same behaviour and wire format as the Swift version: watches ~/.claude/sessions/<pid>.json with
//! kqueue (no polling of Claude), drops dead PIDs, labels Ghostty sessions, adds the Mac's idle time
//! when a session starts waiting, and sends only changes over one WebSocket. Ghostty and idle-time
//! lookups run as short-lived `osascript` / `ioreg` processes, so no scripting framework stays loaded.
//! Config: ~/.config/cc-watch/config.json  {"server": "wss://host/cc/mac", "token": "...", "ghostty": true}

use serde_json::{json, Map, Value};
use std::collections::HashMap;
use std::ffi::CString;
use std::io::ErrorKind;
use std::net::{TcpStream, ToSocketAddrs};
use std::path::{Path, PathBuf};
use std::process::Command;
use std::sync::mpsc::{channel, Receiver, Sender};
use std::time::{Duration, Instant};
use tungstenite::client::{uri_mode, IntoClientRequest};
use tungstenite::handshake::client::Request;
use tungstenite::handshake::HandshakeError;
use tungstenite::stream::{MaybeTlsStream, Mode};
use tungstenite::Message;

fn log(s: &str) {
    eprintln!("[cc-watch] {s}");
}

fn home() -> PathBuf {
    PathBuf::from(std::env::var("HOME").unwrap_or_else(|_| "/".into()))
}

fn hostname() -> String {
    let mut buf = [0u8; 256];
    unsafe { libc::gethostname(buf.as_mut_ptr() as *mut libc::c_char, buf.len()) };
    let s = String::from_utf8_lossy(&buf).trim_end_matches('\0').to_string();
    s.strip_suffix(".local").unwrap_or(&s).to_lowercase()
}

fn alive(pid: i32) -> bool {
    unsafe { libc::kill(pid, 0) == 0 || *libc::__error() != libc::ESRCH }
}

#[derive(Clone, Debug)]
struct Term {
    id: String,
    pid: i64,     // Ghostty reports the terminal's foreground process
    tty: String,  // e.g. /dev/ttys003
    name: String, // tab title
}

/// All Ghostty terminals, or an error (Ghostty missing/too old, or Automation permission not granted).
fn ghostty_terminals() -> Result<Vec<Term>, String> {
    // "tab" inside Ghostty's tell block means Ghostty's tab class, so define the separator first.
    let script = "set sep to character id 9\nset nl to character id 10\n\
        if application \"Ghostty\" is running then\ntell application \"Ghostty\"\nset out to \"\"\n\
        repeat with t in terminals\nset out to out & (id of t) & sep & (pid of t) & sep & (tty of t) & sep & (name of t) & nl\n\
        end repeat\nreturn out\nend tell\nend if\nreturn \"\"";
    let out = Command::new("/usr/bin/osascript").arg("-e").arg(script).output().map_err(|e| e.to_string())?;
    if !out.status.success() {
        return Err(String::from_utf8_lossy(&out.stderr).trim().to_string());
    }
    let mut v = Vec::new();
    for line in String::from_utf8_lossy(&out.stdout).lines() {
        let p: Vec<&str> = line.splitn(4, '\t').collect();
        if p.len() == 4 {
            v.push(Term { id: p[0].into(), pid: p[1].trim().parse().unwrap_or(-1), tty: p[2].trim().into(), name: p[3].into() });
        }
    }
    Ok(v)
}

/// The tab a Claude process runs in: by foreground pid, else by controlling tty.
fn match_terminal<'a>(terms: &'a [Term], pid: i64, tty: &str) -> Option<&'a Term> {
    terms.iter().find(|t| t.pid == pid).or_else(|| {
        if tty.is_empty() { return None; }
        let short = tty.trim_start_matches("/dev/");
        terms.iter().find(|t| t.tty.trim_start_matches("/dev/") == short)
    })
}

/// pid -> controlling tty name (e.g. "ttys003"), one `ps` call for all pids.
fn ttys_of(pids: &[i64]) -> HashMap<i64, String> {
    let mut m = HashMap::new();
    if pids.is_empty() { return m; }
    let list: Vec<String> = pids.iter().map(|p| p.to_string()).collect();
    if let Ok(out) = Command::new("/bin/ps").args(["-o", "pid=,tty=", "-p", &list.join(",")]).output() {
        for line in String::from_utf8_lossy(&out.stdout).lines() {
            let mut it = line.split_whitespace();
            if let (Some(p), Some(t)) = (it.next(), it.next()) {
                if let Ok(p) = p.parse::<i64>() {
                    if t != "??" { m.insert(p, t.to_string()); }
                }
            }
        }
    }
    m
}

fn focus(terminal: &str) {
    let Some(id) = terminal.strip_prefix("ghostty:") else { return };
    let id = id.replace('"', "");
    let s = format!("tell application \"Ghostty\"\nactivate\nfocus (first terminal whose id is \"{id}\")\nend tell");
    let _ = Command::new("/usr/bin/osascript").arg("-e").arg(s).status();
}

fn idle_secs() -> u64 {
    if let Ok(out) = Command::new("/usr/sbin/ioreg").args(["-c", "IOHIDSystem"]).output() {
        for line in String::from_utf8_lossy(&out.stdout).lines() {
            if let Some(i) = line.find("\"HIDIdleTime\" = ") {
                if let Ok(ns) = line[i + 16..].trim().parse::<u64>() {
                    return ns / 1_000_000_000;
                }
            }
        }
    }
    0
}

/// Short text for the robot's session screen, read from the tail of the session's transcript.
fn ask_summary(cwd: &str, sid: &str, status: &str) -> String {
    let enc: String = cwd.chars().map(|c| if c.is_ascii_alphanumeric() || c == '-' { c } else { '-' }).collect();
    let path = home().join(".claude/projects").join(enc).join(format!("{sid}.jsonl"));
    let Ok(mut f) = std::fs::File::open(&path) else { return String::new() };
    use std::io::{Read, Seek, SeekFrom};
    let len = f.metadata().map(|m| m.len()).unwrap_or(0);
    let _ = f.seek(SeekFrom::Start(len.saturating_sub(32 * 1024)));
    let mut buf = Vec::new();
    let _ = f.read_to_end(&mut buf);
    summarize_transcript(&String::from_utf8_lossy(&buf), status)
}

/// waiting: the pending request (tool call, else question). busy: the latest tool call or text.
/// idle / shell: the end of Claude's last reply (usually its conclusion or a question to you).
fn summarize_transcript(text: &str, status: &str) -> String {
    let squash = |s: &str| s.split_whitespace().collect::<Vec<_>>().join(" ");
    for line in text.lines().rev() {
        let Ok(j) = serde_json::from_str::<Value>(line) else { continue };
        if j["type"] != "assistant" { continue; }
        let Some(parts) = j["message"]["content"].as_array() else { continue };
        let mut tool = String::new();
        let mut said = String::new();
        for p in parts.iter().rev() {
            if p["type"] == "tool_use" && tool.is_empty() {
                let inp = &p["input"];
                let arg = ["command", "file_path", "url", "pattern", "description", "prompt"]
                    .iter().find_map(|k| inp[*k].as_str()).unwrap_or("");
                tool = format!("{}: {}", p["name"].as_str().unwrap_or("tool"), arg);
            }
            if p["type"] == "text" && said.is_empty() {
                said = p["text"].as_str().unwrap_or("").to_string();
            }
        }
        let chars = |s: String| -> Vec<char> { squash(&s).chars().collect() };
        if status == "idle" || status == "shell" {   // shell: idle with a background command still running
            if said.trim().is_empty() { continue; }        // idle: skip tool-only lines, find the reply
            let c = chars(said);
            return if c.len() > 220 { format!("…{}", c[c.len() - 220..].iter().collect::<String>()) }
                   else { c.iter().collect() };
        }
        let pick = if !tool.is_empty() { tool } else { said };
        if pick.trim().is_empty() { continue; }
        let c = chars(pick);
        return if c.len() > 140 { c[..140].iter().collect::<String>() + "…" } else { c.iter().collect() };
    }
    String::new()
}

fn applescript_str(s: &str) -> String {
    format!("\"{}\"", s.replace('\\', "\\\\").replace('"', "\\\""))
}

fn ghostty_do(terminal_id: &str, commands: &[String]) -> Result<(), String> {
    let t = format!("(first terminal whose id is {})", applescript_str(terminal_id));
    let body: Vec<String> = commands.iter().map(|c| c.replace("{T}", &t)).collect();
    let script = format!("tell application \"Ghostty\"\n{}\nend tell", body.join("\n"));
    let out = Command::new("/usr/bin/osascript").arg("-e").arg(script).output().map_err(|e| e.to_string())?;
    if out.status.success() { Ok(()) } else { Err(String::from_utf8_lossy(&out.stderr).trim().to_string()) }
}

/// Approve / reject / send for one session, after re-checking the live session file and terminal.
fn do_action(dir: &Path, m: &Value) -> Result<(), String> {
    let sid = m["session"].as_str().unwrap_or("");
    let action = m["action"].as_str().unwrap_or("");
    let terminal = m["terminal"].as_str().and_then(|t| t.strip_prefix("ghostty:")).ok_or("not in Ghostty")?;
    let mut live: Option<Value> = None;
    for e in std::fs::read_dir(dir).map_err(|e| e.to_string())?.flatten() {
        if let Ok(j) = std::fs::read_to_string(e.path()).ok().and_then(|t| serde_json::from_str::<Value>(&t).ok()).ok_or(()) {
            if j["sessionId"] == sid { live = Some(j); break; }
        }
    }
    let live = live.ok_or("session ended")?;
    let pid = live["pid"].as_i64().unwrap_or(-1);
    let terms = ghostty_terminals()?;
    let tty = ttys_of(&[pid]).remove(&pid).unwrap_or_default();
    if match_terminal(&terms, pid, &tty).map(|t| t.id != terminal).unwrap_or(true) {
        return Err("terminal changed".into());
    }
    match action {
        "approve" | "reject" => {
            if live["status"] != "waiting" { return Err("no longer waiting".into()); }
            if let (Some(a), Some(b)) = (m["since"].as_f64(), live["statusUpdatedAt"].as_f64()) {
                if (a - b).abs() > 0.5 { return Err("prompt changed".into()); }
            }
            let key = if action == "approve" { "enter" } else { "escape" };
            ghostty_do(terminal, &[format!("send key \"{key}\" to {{T}}")])
        }
        "send" => {
            let text = m["text"].as_str().unwrap_or("").trim();
            if text.is_empty() { return Err("empty prompt".into()); }
            ghostty_do(terminal, &[format!("input text {} to {{T}}", applescript_str(text)), "send key \"enter\" to {T}".into()])
        }
        _ => Err("unknown action".into()),
    }
}

// ---------------------------------------------------------------------- kqueue watching

struct Watch {
    dir: PathBuf,
    kq: i32,
    files: HashMap<PathBuf, i32>,
    ghostty: HashMap<i64, (String, String)>,   // claude pid -> (terminal id, tab title)
    use_ghostty: bool,
    known_pids: Vec<i64>,                      // pids seen at the last Ghostty lookup
    last_lookup: Option<Instant>,
    last_error: String,
}

impl Watch {
    fn new(dir: PathBuf, use_ghostty: bool) -> Self {
        let _ = std::fs::create_dir_all(&dir);
        let kq = unsafe { libc::kqueue() };
        let w = Watch { dir, kq, files: HashMap::new(), ghostty: HashMap::new(), use_ghostty,
                        known_pids: Vec::new(), last_lookup: None, last_error: String::new() };
        let fd = w.open(&w.dir.clone());
        if fd < 0 {
            log("cannot open ~/.claude/sessions");
            std::process::exit(1);
        }
        w.register(fd);
        w
    }

    fn open(&self, p: &Path) -> i32 {
        let c = CString::new(p.as_os_str().to_string_lossy().as_bytes()).unwrap();
        unsafe { libc::open(c.as_ptr(), libc::O_EVTONLY) }
    }

    fn register(&self, fd: i32) {
        let ev = libc::kevent {
            ident: fd as usize,
            filter: libc::EVFILT_VNODE,
            flags: libc::EV_ADD | libc::EV_CLEAR,
            fflags: libc::NOTE_WRITE | libc::NOTE_EXTEND | libc::NOTE_DELETE | libc::NOTE_RENAME,
            data: 0,
            udata: std::ptr::null_mut(),
        };
        unsafe { libc::kevent(self.kq, &ev, 1, std::ptr::null_mut(), 0, std::ptr::null()) };
    }

    /// Block until something in the folder changes or `timeout` passes. Returns true on a change.
    fn wait(&self, timeout: Duration) -> bool {
        let ts = libc::timespec { tv_sec: timeout.as_secs() as _, tv_nsec: timeout.subsec_nanos() as _ };
        let mut evs: [libc::kevent; 16] = unsafe { std::mem::zeroed() };
        let n = unsafe { libc::kevent(self.kq, std::ptr::null(), 0, evs.as_mut_ptr(), 16, &ts) };
        if n > 0 {
            std::thread::sleep(Duration::from_millis(150)); // coalesce a burst of writes
            let mut drain: [libc::kevent; 16] = unsafe { std::mem::zeroed() };
            let zero = libc::timespec { tv_sec: 0, tv_nsec: 0 };
            unsafe { libc::kevent(self.kq, std::ptr::null(), 0, drain.as_mut_ptr(), 16, &zero) };
        }
        n > 0
    }

    fn read(&mut self) -> HashMap<String, Value> {
        let mut parsed = Vec::new();
        let mut seen = Vec::new();
        let mut need_ghostty = false;
        if let Ok(rd) = std::fs::read_dir(&self.dir) {
            for e in rd.flatten() {
                let path = e.path();
                if path.extension().and_then(|x| x.to_str()) != Some("json") {
                    continue;
                }
                let Ok(text) = std::fs::read_to_string(&path) else { continue };
                let Ok(j) = serde_json::from_str::<Value>(&text) else { continue };
                let (Some(pid), Some(_)) = (j["pid"].as_i64(), j["sessionId"].as_str()) else { continue };
                if !alive(pid as i32) {
                    continue; // crashed session: file left behind
                }
                if !self.files.contains_key(&path) {
                    let fd = self.open(&path);
                    if fd >= 0 {
                        self.register(fd);
                        self.files.insert(path.clone(), fd);
                    }
                }
                seen.push(path);
                if self.use_ghostty && !self.known_pids.contains(&pid) {
                    need_ghostty = true;   // a session we haven't looked up yet
                }
                parsed.push(j);
            }
        }
        // Stop watching files that are gone.
        let gone: Vec<PathBuf> = self.files.keys().filter(|p| !seen.contains(p)).cloned().collect();
        for p in gone {
            if let Some(fd) = self.files.remove(&p) {
                unsafe { libc::close(fd) };
            }
        }
        // Look up tabs when a new session appears; otherwise refresh at most every 30 s (tab titles,
        // sessions that moved), so sessions outside Ghostty don't cost an osascript run per file change.
        let stale = self.last_lookup.map(|t| t.elapsed() >= Duration::from_secs(30)).unwrap_or(true);
        if self.use_ghostty && (need_ghostty || stale) {
            let pids: Vec<i64> = parsed.iter().filter_map(|j| j["pid"].as_i64()).collect();
            self.last_lookup = Some(Instant::now());
            self.known_pids = pids.clone();
            match ghostty_terminals() {
                Ok(terms) => {
                    let ttys = ttys_of(&pids);
                    self.ghostty = pids.iter().filter_map(|p| {
                        match_terminal(&terms, *p, ttys.get(p).map(|s| s.as_str()).unwrap_or(""))
                            .map(|t| (*p, (t.id.clone(), t.name.clone())))
                    }).collect();
                    if !self.last_error.is_empty() { log("ghostty lookup working again"); }
                    self.last_error.clear();
                }
                Err(e) => {
                    if e != self.last_error {
                        log(&format!("ghostty lookup failed: {e} (System Settings > Privacy & Security > Automation: allow cc-watch to control Ghostty)"));
                    }
                    self.last_error = e;
                }
            }
        }
        let mut out = HashMap::new();
        for j in parsed {
            let mut s = Map::new();
            let sid = j["sessionId"].as_str().unwrap_or_default().to_string();
            let cwd = j["cwd"].as_str().unwrap_or("?");
            s.insert("id".into(), json!(sid));
            s.insert("project".into(), json!(Path::new(cwd).file_name().and_then(|x| x.to_str()).unwrap_or("?")));
            s.insert("name".into(), json!(j["name"].as_str().unwrap_or("")));
            s.insert("kind".into(), json!(j["kind"].as_str().unwrap_or("")));
            s.insert("status".into(), json!(j["status"].as_str().unwrap_or("")));
            s.insert("since".into(), json!(j["statusUpdatedAt"].as_f64().unwrap_or(0.0)));
            if let Some(w) = j["waitingFor"].as_str() {
                s.insert("waitingFor".into(), json!(w));
            }
            let status = j["status"].as_str().unwrap_or("");
            let ask = ask_summary(cwd, &sid, status);
            if !ask.is_empty() {
                s.insert("ask".into(), json!(ask));
            }
            if let Some((tid, title)) = self.ghostty.get(&j["pid"].as_i64().unwrap_or(-1)) {
                s.insert("terminal".into(), json!(format!("ghostty:{tid}")));
                s.insert("tab".into(), json!(title));
            }
            out.insert(sid, Value::Object(s));
        }
        out
    }
}

// ---------------------------------------------------------------------- network thread

type Ws = tungstenite::WebSocket<MaybeTlsStream<TcpStream>>;

/// How long each step of connecting (TCP connect, TLS, WebSocket upgrade) may wait on the server.
const CONNECT_TIMEOUT: Duration = Duration::from_secs(10);

fn ws_request(server: &str, token: &str) -> Result<Request, String> {
    let mut req = server.into_client_request().map_err(|e| e.to_string())?;
    let auth = format!("Bearer {token}").parse().map_err(|_| "token has invalid characters".to_string())?;
    req.headers_mut().insert("Authorization", auth);
    Ok(req)
}

/// Open the WebSocket to the server. Every step gives up after `timeout`: a link that dies
/// mid-handshake (the Mac going back to sleep, a network change) must fail so the caller retries,
/// instead of blocking the network thread forever. `tungstenite::connect` has no such timeout.
fn connect_ws(req: Request, timeout: Duration) -> Result<Ws, String> {
    let mode = uri_mode(req.uri()).map_err(|e| e.to_string())?;
    let host = req.uri().host().ok_or("server URL has no host")?;
    let host = host.trim_start_matches('[').trim_end_matches(']').to_string(); // IPv6 literal
    let port = req.uri().port_u16().unwrap_or(match mode { Mode::Plain => 80, Mode::Tls => 443 });

    let mut tcp = Err(format!("no address for {host}"));
    for addr in (host.as_str(), port).to_socket_addrs().map_err(|e| e.to_string())? {
        tcp = TcpStream::connect_timeout(&addr, timeout).map_err(|e| e.to_string());
        if tcp.is_ok() { break; }
    }
    let tcp = tcp?;
    let _ = tcp.set_nodelay(true);
    tcp.set_read_timeout(Some(timeout)).map_err(|e| e.to_string())?;
    tcp.set_write_timeout(Some(timeout)).map_err(|e| e.to_string())?;

    // A read that hits the timeout surfaces as "would block" from both handshakes below.
    let stream = match mode {
        Mode::Plain => MaybeTlsStream::Plain(tcp),
        Mode::Tls => {
            let tls = native_tls::TlsConnector::new().map_err(|e| e.to_string())?;
            match tls.connect(&host, tcp) {
                Ok(s) => MaybeTlsStream::NativeTls(s),
                Err(native_tls::HandshakeError::WouldBlock(_)) => return Err("TLS handshake timed out".into()),
                Err(native_tls::HandshakeError::Failure(e)) => return Err(e.to_string()),
            }
        }
    };
    match tungstenite::client(req, stream) {
        Ok((ws, _)) => Ok(ws),
        Err(HandshakeError::Interrupted(_)) => Err("WebSocket handshake timed out".into()),
        Err(HandshakeError::Failure(e)) => Err(e.to_string()),
    }
}

fn net_loop(server: String, token: String, rx: Receiver<String>, resync: Sender<()>, inbox: Sender<Value>) {
    loop {
        let req = match ws_request(&server, &token) {
            Ok(r) => r,
            Err(e) => {
                log(&format!("bad server URL: {e}"));
                std::process::exit(1);
            }
        };
        match connect_ws(req, CONNECT_TIMEOUT) {
            Ok(mut ws) => {
                log("connected");
                match ws.get_mut() {
                    MaybeTlsStream::Plain(s) => { let _ = s.set_read_timeout(Some(Duration::from_millis(250))); }
                    MaybeTlsStream::NativeTls(s) => { let _ = s.get_ref().set_read_timeout(Some(Duration::from_millis(250))); }
                    _ => {}
                }
                while rx.try_recv().is_ok() {} // stale; a fresh snapshot follows
                let _ = resync.send(());
                'conn: loop {
                    while let Ok(m) = rx.try_recv() {
                        if ws.send(Message::text(m)).is_err() {
                            break 'conn;
                        }
                    }
                    match ws.read() {
                        Ok(Message::Text(t)) => {
                            if let Ok(v) = serde_json::from_str::<Value>(&t) {
                                if v["type"] == "focus" {
                                    if let Some(term) = v["terminal"].as_str() {
                                        focus(term);
                                    }
                                } else if v["type"] == "act" {
                                    let _ = inbox.send(v);
                                }
                            }
                        }
                        Ok(_) => {}
                        Err(tungstenite::Error::Io(e))
                            if e.kind() == ErrorKind::WouldBlock || e.kind() == ErrorKind::TimedOut => {}
                        Err(e) => {
                            log(&format!("disconnected: {e}; retrying in 5 s"));
                            break 'conn;
                        }
                    }
                }
            }
            Err(e) => log(&format!("connect failed: {e}; retrying in 5 s")),
        }
        std::thread::sleep(Duration::from_secs(5));
    }
}

// ---------------------------------------------------------------------- main

fn main() {
    let cfg_path = home().join(".config/cc-watch/config.json");
    let cfg: Value = match std::fs::read_to_string(&cfg_path).ok().and_then(|t| serde_json::from_str(&t).ok()) {
        Some(v) => v,
        None => {
            log(&format!("missing or invalid {}", cfg_path.display()));
            std::process::exit(1);
        }
    };
    let server = cfg["server"].as_str().unwrap_or_default().to_string();
    let token = cfg["token"].as_str().unwrap_or_default().to_string();
    let host = hostname();
    let (tx, rx) = channel::<String>();
    let (resync_tx, resync_rx) = channel::<()>();
    let (inbox_tx, inbox_rx) = channel::<Value>();
    std::thread::spawn({
        let server = server.clone();
        move || net_loop(server, token, rx, resync_tx, inbox_tx)
    });

    let mut w = Watch::new(home().join(".claude/sessions"), cfg["ghostty"].as_bool().unwrap_or(true));
    log(&format!("watching {} as {host} -> {server}", w.dir.display()));
    let mut sent: HashMap<String, String> = HashMap::new();
    let mut last_beat = Instant::now();
    let mut last_idle = Instant::now();
    let with_idle = |s: &Value| -> Value {
        let mut m = s.clone();
        if s["status"] == "waiting" {
            m["idleSec"] = json!(idle_secs());
        }
        m
    };

    loop {
        let changed = w.wait(Duration::from_millis(300));
        while let Ok(m) = inbox_rx.try_recv() {
            let res = do_action(&w.dir, &m);
            log(&format!("action {} {}: {:?}", m["action"], m["session"], res));
            let mut r = json!({"type": "result", "session": m["session"], "action": m["action"], "ok": res.is_ok()});
            if let Err(e) = res { r["error"] = json!(e); }
            let _ = tx.send(r.to_string());
        }
        if resync_rx.try_recv().is_ok() {
            let now = w.read();
            sent = now.iter().map(|(k, v)| (k.clone(), v.to_string())).collect();
            let sessions: Vec<Value> = now.values().map(&with_idle).collect();
            let _ = tx.send(json!({"type": "snapshot", "host": host, "sessions": sessions}).to_string());
        }
        // While a session waits, report keyboard/mouse idle time so the server knows if you walk away.
        if last_idle.elapsed() >= Duration::from_secs(30) {
            last_idle = Instant::now();
            if sent.values().any(|t| t.contains("\"status\":\"waiting\"")) {
                let _ = tx.send(json!({"type": "idle", "host": host, "idleSec": idle_secs()}).to_string());
            }
        }
        let beat = last_beat.elapsed() >= Duration::from_secs(60);
        if changed || beat {
            let now = w.read();
            let mut updates = Vec::new();
            for (sid, s) in &now {
                let text = s.to_string();
                if sent.get(sid) != Some(&text) {
                    updates.push(with_idle(s));
                    sent.insert(sid.clone(), text);
                }
            }
            let gone: Vec<String> = sent.keys().filter(|k| !now.contains_key(*k)).cloned().collect();
            for sid in gone {
                sent.remove(&sid);
                let _ = tx.send(json!({"type": "gone", "id": sid}).to_string());
            }
            if !updates.is_empty() {
                let _ = tx.send(json!({"type": "update", "host": host, "sessions": updates}).to_string());
            }
            if beat {
                last_beat = Instant::now();
                let ids: Vec<&String> = sent.keys().collect();
                let _ = tx.send(json!({"type": "heartbeat", "host": host, "ids": ids}).to_string());
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::summarize_transcript;

    const T: &str = concat!(
        r#"{"type":"user","message":{"content":"build it"}}"#, "\n",
        r#"{"type":"assistant","message":{"content":[{"type":"tool_use","name":"Bash","input":{"command":"cargo build --release"}}]}}"#, "\n",
        r#"{"type":"user","message":{"content":[{"type":"tool_result","content":"ok"}]}}"#, "\n",
        r#"{"type":"assistant","message":{"content":[{"type":"text","text":"Build finished.\n\nShould I also run the tests?"}]}}"#, "\n",
    );

    /// A server that accepts connections and then says nothing (like a link that died mid-handshake).
    fn silent_server() -> u16 {
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        std::thread::spawn(move || {
            let mut held = Vec::new();
            for conn in listener.incoming().flatten() {
                held.push(conn); // keep it open, never reply
            }
        });
        port
    }

    #[test]
    fn connect_gives_up_when_the_server_never_answers() {
        use super::{connect_ws, ws_request};
        use std::time::{Duration, Instant};
        // ws: stalls waiting for the upgrade reply; wss: stalls inside the TLS handshake.
        for scheme in ["ws", "wss"] {
            let url = format!("{scheme}://127.0.0.1:{}/cc/mac", silent_server());
            let (tx, rx) = std::sync::mpsc::channel();
            let started = Instant::now();
            std::thread::spawn(move || {
                let res = connect_ws(ws_request(&url, "t").unwrap(), Duration::from_millis(300));
                let _ = tx.send(res.map(|_| ()));
            });
            match rx.recv_timeout(Duration::from_secs(5)) {
                Ok(res) => {
                    let err = res.expect_err("a silent server is not a connection");
                    assert!(err.contains("timed out"), "{scheme}: {err}");
                    assert!(started.elapsed() < Duration::from_secs(3), "{scheme}: took {:?}", started.elapsed());
                }
                Err(_) => panic!("{scheme}: connect is still waiting after 5 s; it must give up"),
            }
        }
    }

    #[test]
    #[allow(clippy::result_large_err)] // the callback's error type is fixed by tungstenite
    fn connect_reaches_a_ws_server_and_sends_the_token() {
        use super::{connect_ws, ws_request};
        use std::time::Duration;
        use tungstenite::handshake::server::{Request, Response};
        let listener = std::net::TcpListener::bind("127.0.0.1:0").unwrap();
        let port = listener.local_addr().unwrap().port();
        let server = std::thread::spawn(move || {
            let (conn, _) = listener.accept().unwrap();
            let mut auth = String::new();
            let mut ws = tungstenite::accept_hdr(conn, |req: &Request, resp: Response| {
                auth = req.headers().get("Authorization").and_then(|v| v.to_str().ok()).unwrap_or("").to_string();
                Ok(resp)
            }).unwrap();
            let msg = ws.read().unwrap().into_text().unwrap().to_string();
            (auth, msg)
        });
        let url = format!("ws://127.0.0.1:{port}/cc/mac");
        let mut ws = connect_ws(ws_request(&url, "secret").unwrap(), Duration::from_secs(2)).expect("connects");
        ws.send(tungstenite::Message::text("hello")).unwrap();
        assert_eq!(server.join().unwrap(), ("Bearer secret".to_string(), "hello".to_string()));
    }

    #[test]
    fn terminal_matches_by_pid_then_by_tty() {
        use super::{match_terminal, Term};
        let terms = vec![
            Term { id: "A".into(), pid: 100, tty: "/dev/ttys001".into(), name: "tab a".into() },
            Term { id: "B".into(), pid: 555, tty: "/dev/ttys002".into(), name: "tab b".into() },
        ];
        assert_eq!(match_terminal(&terms, 100, "").map(|t| t.id.as_str()), Some("A"));
        // Claude isn't the tab's foreground process (wrapper, child process), but shares its tty.
        assert_eq!(match_terminal(&terms, 200, "ttys002").map(|t| t.id.as_str()), Some("B"));
        assert!(match_terminal(&terms, 300, "ttys009").is_none());
        assert!(match_terminal(&terms, 300, "").is_none(), "no tty (e.g. not a terminal): no match");
    }

    #[test]
    fn ready_session_shows_end_of_last_reply() {
        assert_eq!(summarize_transcript(T, "idle"), "Build finished. Should I also run the tests?");
    }

    #[test]
    fn shell_session_is_ready_and_shows_end_of_last_reply() {
        // "shell" = Claude is idle while a background command it started is still running.
        assert_eq!(summarize_transcript(T, "shell"), "Build finished. Should I also run the tests?");
        let long = format!(r#"{{"type":"assistant","message":{{"content":[{{"type":"text","text":"{}THE END"}}]}}}}"#, "x".repeat(500));
        assert!(summarize_transcript(&long, "shell").ends_with("THE END"));
    }

    #[test]
    fn long_reply_keeps_the_ending() {
        let long = format!(r#"{{"type":"assistant","message":{{"content":[{{"type":"text","text":"{}THE END"}}]}}}}"#, "x".repeat(500));
        let s = summarize_transcript(&long, "idle");
        assert!(s.starts_with('…') && s.ends_with("THE END") && s.chars().count() <= 221, "{s}");
    }

    #[test]
    fn working_session_shows_latest_tool_call() {
        let t = T.lines().take(2).collect::<Vec<_>>().join("\n");
        assert_eq!(summarize_transcript(&t, "busy"), "Bash: cargo build --release");
    }

    #[test]
    fn waiting_session_shows_pending_request() {
        let t = T.lines().take(2).collect::<Vec<_>>().join("\n");
        assert_eq!(summarize_transcript(&t, "waiting"), "Bash: cargo build --release");
    }
}
