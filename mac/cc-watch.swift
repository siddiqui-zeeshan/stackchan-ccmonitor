// cc-watch: reports live Claude Code session state to cc-monitor. Event-driven, no polling of Claude.
//
// Reads ~/.claude/sessions/<pid>.json (Claude Code's own per-session status files), checks the PID is
// alive, labels Ghostty sessions (tab title, terminal id) via Ghostty's AppleScript, attaches the Mac's
// idle time when a session starts waiting, and sends changes over one WebSocket.
// Config: ~/.config/cc-watch/config.json  {"server": "wss://host/cc/mac", "token": "...", "ghostty": true}

import Foundation
import IOKit

struct Config: Decodable {
    let server: String
    let token: String
    let ghostty: Bool?
}

func log(_ s: String) {
    FileHandle.standardError.write(("[cc-watch] " + s + "\n").data(using: .utf8)!)
}

final class Watcher {
    let cfg: Config
    let dir = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(".claude/sessions")
    let host: String = {
        var h = ProcessInfo.processInfo.hostName
        if h.hasSuffix(".local") { h = String(h.dropLast(6)) }
        return h
    }()
    var dirSource: DispatchSourceFileSystemObject?
    var fileSources: [String: DispatchSourceFileSystemObject] = [:]
    var sent: [String: String] = [:]                    // session id -> last sent payload (JSON text)
    var ghostty: [Int: (id: String, name: String)] = [:] // pid -> Ghostty terminal
    var scanPending = false
    var ws: URLSessionWebSocketTask?
    var connected = false

    init(cfg: Config) { self.cfg = cfg }

    // ------------------------------------------------------------------ watching

    func start() {
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        let fd = open(dir.path, O_EVTONLY)
        guard fd >= 0 else { log("cannot open \(dir.path)"); exit(1) }
        let src = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .rename, .delete], queue: .main)
        src.setEventHandler { [weak self] in self?.scheduleScan() }
        src.setCancelHandler { close(fd) }
        src.resume()
        dirSource = src
        connect()
        scheduleScan()
        Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { [weak self] _ in self?.heartbeat() }
    }

    /// Coalesce bursts of file events into one scan.
    func scheduleScan() {
        if scanPending { return }
        scanPending = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.15) { [weak self] in
            self?.scanPending = false
            self?.scan()
        }
    }

    /// Watch each session file too: in-place rewrites don't always touch the directory.
    func watchFile(_ path: String) {
        if fileSources[path] != nil { return }
        let fd = open(path, O_EVTONLY)
        guard fd >= 0 else { return }
        let src = DispatchSource.makeFileSystemObjectSource(fileDescriptor: fd, eventMask: [.write, .extend, .delete, .rename], queue: .main)
        src.setEventHandler { [weak self, weak src] in
            if let ev = src?.data, ev.contains(.delete) || ev.contains(.rename) {
                src?.cancel()
                self?.fileSources[path] = nil
            }
            self?.scheduleScan()
        }
        src.setCancelHandler { close(fd) }
        src.resume()
        fileSources[path] = src
    }

    func readSessions() -> [String: [String: Any]] {
        var out: [String: [String: Any]] = [:]
        let names = (try? FileManager.default.contentsOfDirectory(atPath: dir.path)) ?? []
        var needGhostty = false
        var parsed: [(String, [String: Any])] = []
        for name in names where name.hasSuffix(".json") {
            let path = dir.appendingPathComponent(name).path
            guard let data = FileManager.default.contents(atPath: path),
                  let j = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
                  let pid = j["pid"] as? Int, let sid = j["sessionId"] as? String else { continue }
            if kill(pid_t(pid), 0) != 0 && errno == ESRCH { continue }   // crashed: file left behind
            watchFile(path)
            if ghostty[pid] == nil { needGhostty = true }
            parsed.append((sid, j))
        }
        if needGhostty && cfg.ghostty != false { refreshGhostty() }
        for (sid, j) in parsed {
            let pid = j["pid"] as! Int
            var s: [String: Any] = [
                "id": sid,
                "project": URL(fileURLWithPath: j["cwd"] as? String ?? "?").lastPathComponent,
                "name": j["name"] as? String ?? "",
                "kind": j["kind"] as? String ?? "",
                "status": j["status"] as? String ?? "",
                "since": j["statusUpdatedAt"] as? Double ?? 0,
            ]
            if let w = j["waitingFor"] as? String { s["waitingFor"] = w }
            if let t = ghostty[pid] { s["terminal"] = "ghostty:" + t.id; s["tab"] = t.name }
            out[sid] = s
        }
        return out
    }

    func scan() {
        let now = readSessions()
        var changed: [[String: Any]] = []
        for (sid, s) in now {
            let text = json(s)
            if sent[sid] != text {
                var msg = s
                if (s["status"] as? String) == "waiting" { msg["idleSec"] = idleSeconds() }
                changed.append(msg)
                sent[sid] = text
            }
        }
        for sid in sent.keys where now[sid] == nil {
            sent[sid] = nil
            send(["type": "gone", "id": sid])
        }
        if !changed.isEmpty { send(["type": "update", "host": host, "sessions": changed]) }
    }

    func heartbeat() {
        scan()   // also the liveness sweep for crashed sessions
        send(["type": "heartbeat", "host": host, "ids": Array(sent.keys)])
    }

    // ------------------------------------------------------------------ Ghostty + idle

    func refreshGhostty() {
        let src = """
        -- "tab" inside Ghostty's tell block means Ghostty's tab class, so define the separator here.
        set sep to character id 9
        set nl to character id 10
        if application "Ghostty" is running then
          tell application "Ghostty"
            set out to ""
            repeat with t in terminals
              set out to out & (id of t) & sep & (pid of t) & sep & (name of t) & nl
            end repeat
            return out
          end tell
        end if
        return ""
        """
        var err: NSDictionary?
        guard let result = NSAppleScript(source: src)?.executeAndReturnError(&err).stringValue else {
            if let err { log("ghostty lookup failed: \(err[NSAppleScript.errorMessage] ?? err)") }
            return
        }
        var map: [Int: (id: String, name: String)] = [:]
        for line in result.split(separator: "\n") {
            let parts = line.split(separator: "\t", maxSplits: 2, omittingEmptySubsequences: false)
            if parts.count == 3, let pid = Int(parts[1]) { map[pid] = (String(parts[0]), String(parts[2])) }
        }
        ghostty = map
    }

    func focus(_ terminal: String) {
        guard terminal.hasPrefix("ghostty:") else { return }
        let id = terminal.dropFirst(8).replacingOccurrences(of: "\"", with: "")
        let src = "tell application \"Ghostty\"\nactivate\nfocus (first terminal whose id is \"\(id)\")\nend tell"
        var err: NSDictionary?
        NSAppleScript(source: src)?.executeAndReturnError(&err)
        if let err { log("focus failed: \(err[NSAppleScript.errorMessage] ?? err)") }
    }

    func idleSeconds() -> Int {
        let svc = IOServiceGetMatchingService(kIOMainPortDefault, IOServiceMatching("IOHIDSystem"))
        defer { IOObjectRelease(svc) }
        guard let v = IORegistryEntryCreateCFProperty(svc, "HIDIdleTime" as CFString, kCFAllocatorDefault, 0)?
            .takeRetainedValue() as? NSNumber else { return 0 }
        return Int(v.uint64Value / 1_000_000_000)
    }

    // ------------------------------------------------------------------ WebSocket

    func connect() {
        guard let url = URL(string: cfg.server) else { log("bad server URL"); exit(1) }
        var req = URLRequest(url: url)
        req.setValue("Bearer \(cfg.token)", forHTTPHeaderField: "Authorization")
        let task = URLSession.shared.webSocketTask(with: req)
        ws = task
        task.resume()
        connected = true
        sent = [:]   // resend everything as a fresh snapshot
        let snap = readSessions()
        for (sid, s) in snap { sent[sid] = json(s) }
        let sessions = snap.values.map { s -> [String: Any] in
            var m = s
            if (s["status"] as? String) == "waiting" { m["idleSec"] = idleSeconds() }
            return m
        }
        send(["type": "snapshot", "host": host, "sessions": sessions])
        receive()
    }

    func receive() {
        ws?.receive { [weak self] result in
            DispatchQueue.main.async {
                guard let self else { return }
                switch result {
                case .success(.string(let text)):
                    if let d = text.data(using: .utf8),
                       let m = (try? JSONSerialization.jsonObject(with: d)) as? [String: Any],
                       m["type"] as? String == "focus", let t = m["terminal"] as? String {
                        self.focus(t)
                    }
                    self.receive()
                case .success:
                    self.receive()
                case .failure(let e):
                    log("disconnected: \(e.localizedDescription); retrying in 5 s")
                    self.connected = false
                    DispatchQueue.main.asyncAfter(deadline: .now() + 5) { self.connect() }
                }
            }
        }
    }

    func send(_ obj: [String: Any]) {
        guard connected, let ws else { return }
        ws.send(.string(json(obj))) { err in
            if let err { log("send failed: \(err.localizedDescription)") }
        }
    }

    func json(_ obj: Any) -> String {
        let d = (try? JSONSerialization.data(withJSONObject: obj, options: [.sortedKeys])) ?? Data()
        return String(data: d, encoding: .utf8) ?? "{}"
    }
}

let cfgPath = URL(fileURLWithPath: NSHomeDirectory()).appendingPathComponent(".config/cc-watch/config.json")
guard let data = FileManager.default.contents(atPath: cfgPath.path),
      let cfg = try? JSONDecoder().decode(Config.self, from: data) else {
    log("missing or invalid \(cfgPath.path)")
    exit(1)
}
let watcher = Watcher(cfg: cfg)
watcher.start()
log("watching \(watcher.dir.path) as \(watcher.host) -> \(cfg.server)")
RunLoop.main.run()
