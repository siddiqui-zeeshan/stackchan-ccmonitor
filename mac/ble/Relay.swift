// What cc-ble passes between the local server and the robot.
import Foundation

struct Relay {
    private var lastDisplay: String?

    /// Server -> robot: everything is passed on; the latest "display" is kept for a robot that (re)connects,
    /// because the server only sends the whole state when its socket opens, and ours stays open.
    mutating func fromServer(_ text: String) -> String? {
        if Relay.type(of: text) == "display" { lastDisplay = text }
        return text
    }

    /// Robot -> server: only head-panel touches. The robot is display only, so nothing that acts on a session
    /// (approve, reject, typing, focusing a tab, Talk) gets through, whatever the robot sends.
    func fromRobot(_ text: String) -> String? {
        Relay.type(of: text) == "touch" ? text : nil
    }

    /// What to send a robot that just connected: the latest screen state, if any.
    func robotConnected() -> String? { lastDisplay }

    private static func type(of text: String) -> String? {
        guard let obj = try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any] else { return nil }
        return obj["type"] as? String
    }
}
