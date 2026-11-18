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

    static let gestures: Set<String> = ["tap", "double_tap", "long_press", "stroke"]

    /// Robot -> server: only head-panel touches. The robot is display only, so nothing that acts on a session
    /// (approve, reject, typing, focusing a tab, Talk) gets through, whatever the robot sends. The message is
    /// rebuilt from the known fields rather than passed on, so extra or duplicate keys can't ride along.
    func fromRobot(_ text: String) -> String? {
        guard let obj = Relay.object(text), obj["type"] as? String == "touch",
              let gesture = obj["gesture"] as? String, Relay.gestures.contains(gesture),
              let row = obj["row"] as? NSNumber, CFGetTypeID(row) == CFNumberGetTypeID(),
              let out = try? JSONSerialization.data(withJSONObject: ["type": "touch", "gesture": gesture,
                                                                     "row": row.intValue], options: .sortedKeys)
        else { return nil }
        return String(data: out, encoding: .utf8)
    }

    /// What to send a robot that just connected: the latest screen state, if any.
    func robotConnected() -> String? { lastDisplay }

    private static func object(_ text: String) -> [String: Any]? {
        try? JSONSerialization.jsonObject(with: Data(text.utf8)) as? [String: Any]
    }

    private static func type(of text: String) -> String? { object(text)?["type"] as? String }
}
