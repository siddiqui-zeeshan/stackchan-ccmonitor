// cc-ble: carries messages between the local CC Monitor server and the paired StackChan over Bluetooth.
//   cc-ble          run (as the dev.stackchan.cc-ble LaunchAgent): connect to the server as the robot, and to the
//                   paired robot over Bluetooth; reconnect to either whenever it drops
//   cc-ble --pair   pair with a robot that is ready to pair (plugged in by USB, CC Monitor started in the last
//                   2 minutes): macOS asks for the code the robot shows; then save it as the paired robot
// Config: ~/.config/cc-monitor/ble.json {"server": "ws://127.0.0.1:8810/<ROBOT_SECRET>/cc/robot",
//                                        "token": "<ROBOT_TOKEN>", "robot": "<paired robot's id on this Mac>"}
// Built by build.sh with Info.plist embedded (macOS requires it for Bluetooth).
import CoreBluetooth
import Foundation

let serviceUUID = CBUUID(string: "e2e5e5e0-1234-5678-1234-56789abcdef0")
let charUUID = CBUUID(string: "e2e5e5e5-1234-5678-1234-56789abcdef0")
let pairingFlag = Data([0xFF, 0xFF, 0x01])   // manufacturer data while the robot accepts a new pairing
let home = ProcessInfo.processInfo.environment["HOME"].map { URL(fileURLWithPath: $0) }
    ?? FileManager.default.homeDirectoryForCurrentUser
let configURL = home.appendingPathComponent(".config/cc-monitor/ble.json")

func log(_ s: String) {
    let t = ISO8601DateFormatter().string(from: Date())
    FileHandle.standardError.write("\(t) [cc-ble] \(s)\n".data(using: .utf8)!)
}

struct Config: Codable {
    var server: String
    var token: String
    var robot: String
}

func loadConfig() -> Config {
    guard let d = try? Data(contentsOf: configURL), let c = try? JSONDecoder().decode(Config.self, from: d) else {
        log("no config at \(configURL.path): run ./setup.sh")
        exit(1)
    }
    return c
}

func saveConfig(_ c: Config) throws {
    let enc = JSONEncoder()
    enc.outputFormatting = [.prettyPrinted, .sortedKeys, .withoutEscapingSlashes]
    try enc.encode(c).write(to: configURL, options: .atomic)
    try FileManager.default.setAttributes([.posixPermissions: 0o600], ofItemAtPath: configURL.path)
}

// MARK: - the local server, as the robot sees it

final class Server {
    var onText: (String) -> Void = { _ in }
    private let request: URLRequest
    private var task: URLSessionWebSocketTask?

    init(url: String, token: String) {
        guard let u = URL(string: url) else {
            log("bad server URL in config: \(url)")
            exit(1)
        }
        var r = URLRequest(url: u)
        r.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request = r
    }

    func connect() {
        let t = URLSession.shared.webSocketTask(with: request)
        task = t
        t.resume()
        receive(t)
    }

    func send(_ text: String) {
        task?.send(.string(text)) { e in if let e = e { log("server send failed: \(e.localizedDescription)") } }
    }

    private func receive(_ t: URLSessionWebSocketTask) {
        t.receive { [weak self] result in
            DispatchQueue.main.async {
                guard let self = self, t === self.task else { return }
                switch result {
                case .success(.string(let s)):
                    self.onText(s)
                    self.receive(t)
                case .success:
                    self.receive(t)
                case .failure(let e):
                    log("server connection lost (\(e.localizedDescription)); retrying in 5 s")
                    self.task = nil
                    DispatchQueue.main.asyncAfter(deadline: .now() + 5) { self.connect() }
                }
            }
        }
    }
}

// MARK: - the robot over Bluetooth

final class Robot: NSObject, CBCentralManagerDelegate, CBPeripheralDelegate {
    enum Mode { case run(UUID), pair }

    var onMessage: (String) -> Void = { _ in }
    var onReady: () -> Void = {}
    private(set) var ready = false

    private let mode: Mode
    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var characteristic: CBCharacteristic?
    private let joiner = Joiner()
    private var packets: [Data] = []      // the message being written now
    private var waiting: [String] = []    // messages after it; only the newest "display" is kept

    init(mode: Mode) {
        self.mode = mode
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    /// Queue a message for the robot. A newer screen state replaces an older one that hasn't gone out yet.
    func send(_ text: String) {
        guard ready else { return }
        if text.contains("\"type\": \"display\"") || text.contains("\"type\":\"display\"") {
            waiting.removeAll { $0.contains("\"type\": \"display\"") || $0.contains("\"type\":\"display\"") }
        }
        waiting.append(text)
        pump()
    }

    private func pump() {
        guard ready, let p = peripheral, let c = characteristic else { return }
        while true {
            if packets.isEmpty {
                guard !waiting.isEmpty else { return }
                let msg = waiting.removeFirst()
                packets = Frames.split(Data(msg.utf8), packetSize: p.maximumWriteValueLength(for: .withoutResponse))
                if packets.isEmpty { log("message too long for the robot (\(msg.utf8.count) bytes), dropped") }
                continue
            }
            guard p.canSendWriteWithoutResponse else { return }   // peripheralIsReady(...) calls pump again
            p.writeValue(packets.removeFirst(), for: c, type: .withoutResponse)
        }
    }

    private func reconnect(after seconds: Double) {
        guard case .run = mode, let p = peripheral else { return }
        DispatchQueue.main.asyncAfter(deadline: .now() + seconds) { [weak self] in self?.central.connect(p) }
    }

    func centralManagerDidUpdateState(_ c: CBCentralManager) {
        switch c.state {
        case .poweredOn:
            switch mode {
            case .pair:
                log("looking for a robot that is ready to pair...")
                // Duplicates on: a robot first seen before its pairing window opened is reported again.
                c.scanForPeripherals(withServices: [serviceUUID],
                                     options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
            case .run(let id):
                if let p = c.retrievePeripherals(withIdentifiers: [id]).first {
                    peripheral = p
                    p.delegate = self
                    log("waiting for the robot")
                    c.connect(p)   // completes whenever the robot is in range; no timeout
                } else {
                    log("this Mac doesn't know the paired robot any more: run ./setup.sh --pair")
                    exit(1)
                }
            }
        case .unauthorized:
            log("Bluetooth not allowed: System Settings > Privacy & Security > Bluetooth: allow cc-ble")
            exit(1)
        case .poweredOff:
            log("Bluetooth is off")
        default:
            break
        }
    }

    func centralManager(_ c: CBCentralManager, didDiscover p: CBPeripheral, advertisementData a: [String: Any],
                        rssi: NSNumber) {
        guard case .pair = mode, peripheral == nil,
              (a[CBAdvertisementDataManufacturerDataKey] as? Data) == pairingFlag else { return }
        log("found a robot ready to pair; connecting (macOS will ask for the code on the robot's screen)")
        c.stopScan()
        peripheral = p
        p.delegate = self
        c.connect(p)
    }

    func centralManager(_ c: CBCentralManager, didConnect p: CBPeripheral) {
        log("connected")
        p.discoverServices([serviceUUID])
    }

    func centralManager(_ c: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        log("could not connect: \(error?.localizedDescription ?? "unknown")")
        if case .pair = mode { exit(1) }
        reconnect(after: 5)
    }

    func centralManager(_ c: CBCentralManager, didDisconnectPeripheral p: CBPeripheral, error: Error?) {
        log("disconnected\(error.map { ": \($0.localizedDescription)" } ?? "")")
        ready = false
        characteristic = nil
        packets.removeAll()
        waiting.removeAll()
        if case .pair = mode {
            log("pairing failed: the robot closed the connection (pairing closed, or the code was wrong)")
            exit(1)
        }
        reconnect(after: 5)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        guard let s = p.services?.first(where: { $0.uuid == serviceUUID }) else {
            log("the robot has no CC Monitor service: is it running the Bluetooth build?")
            return
        }
        p.discoverCharacteristics([charUUID], for: s)
    }

    func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        guard let c = s.characteristics?.first(where: { $0.uuid == charUUID }) else { return }
        characteristic = c
        p.setNotifyValue(true, for: c)
    }

    func peripheral(_ p: CBPeripheral, didUpdateNotificationStateFor c: CBCharacteristic, error: Error?) {
        if let e = error {
            log("could not subscribe: \(e.localizedDescription)")
            return
        }
        switch mode {
        case .pair:
            // Writing needs a paired, authenticated link: success means the pairing is in place.
            p.writeValue(Data(#"{"type":"hello"}"#.utf8).withFrames(p), for: c, type: .withResponse)
        case .run:
            log("robot ready")
            ready = true
            onReady()
        }
    }

    func peripheral(_ p: CBPeripheral, didWriteValueFor c: CBCharacteristic, error: Error?) {
        guard case .pair = mode else { return }
        if let e = error {
            log("pairing failed: \(e.localizedDescription)")
            exit(1)
        }
        do {
            var cfg = loadConfig()
            cfg.robot = p.identifier.uuidString
            try saveConfig(cfg)
        } catch {
            log("paired, but could not save \(configURL.path): \(error.localizedDescription)")
            exit(1)
        }
        print("Paired with StackChan CC")
        exit(0)
    }

    func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) { pump() }

    func peripheral(_ p: CBPeripheral, didUpdateValueFor c: CBCharacteristic, error: Error?) {
        guard let v = c.value else { return }
        switch joiner.push(v) {
        case .message(let d):
            if let s = String(data: d, encoding: .utf8) { onMessage(s) }
        case .error:
            log("bad packet from the robot, message dropped")
        case .partial:
            break
        }
    }
}

extension Data {
    /// This message as a single framed packet (for the short pairing check).
    func withFrames(_ p: CBPeripheral) -> Data {
        Frames.split(self, packetSize: p.maximumWriteValueLength(for: .withResponse)).first ?? Data()
    }
}

// MARK: - main

let cfg = loadConfig()
if CommandLine.arguments.contains("--pair") {
    let robot = Robot(mode: .pair)
    DispatchQueue.main.asyncAfter(deadline: .now() + 120) {
        log("no robot ready to pair found in 2 minutes. Plug the robot into this Mac by USB, restart it, "
            + "and run ./setup.sh --pair again.")
        exit(1)
    }
    RunLoop.main.run()
    _ = robot
} else {
    guard let id = UUID(uuidString: cfg.robot) else {
        log("not paired: run ./setup.sh --pair")
        exit(1)
    }
    var relay = Relay()
    let server = Server(url: cfg.server, token: cfg.token)
    let robot = Robot(mode: .run(id))
    server.onText = { text in
        if let out = relay.fromServer(text) { robot.send(out) }
    }
    robot.onReady = {
        if let d = relay.robotConnected() { robot.send(d) }
    }
    robot.onMessage = { text in
        if let out = relay.fromRobot(text) {
            log("robot: \(out)")
            server.send(out)
        } else {
            log("robot sent something other than a head-panel touch; dropped")
        }
    }
    log("starting: server \(cfg.server.replacingOccurrences(of: #"/[^/]+/cc/robot"#, with: "/…/cc/robot", options: .regularExpression))")
    server.connect()
    RunLoop.main.run()
}
