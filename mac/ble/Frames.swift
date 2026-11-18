// Messages between cc-ble and the robot are JSON split into packets that fit one Bluetooth write or
// notification. Every packet starts with the stock StackChan 10-byte header:
//   AA 55 C3 01, packet index, packet count, total message length   (the last three big-endian UInt16)
// Same format as firmware/app_cc_monitor/frames.h on the robot.
import Foundation

enum Frames {
    static let header = 10
    static let maxMessage = 16384

    /// Packets of at most `packetSize` bytes; empty when the message is empty, too long, or nothing fits.
    static func split(_ msg: Data, packetSize: Int) -> [Data] {
        guard !msg.isEmpty, msg.count <= maxMessage, packetSize > header else { return [] }
        let bytes = [UInt8](msg)
        let chunk = packetSize - header
        let count = (bytes.count + chunk - 1) / chunk
        return (0..<count).map { i in
            var p = Data([0xAA, 0x55, 0xC3, 0x01, UInt8(i >> 8), UInt8(i & 0xFF), UInt8(count >> 8),
                          UInt8(count & 0xFF), UInt8(bytes.count >> 8), UInt8(bytes.count & 0xFF)])
            p.append(contentsOf: bytes[(i * chunk)..<min(bytes.count, (i + 1) * chunk)])
            return p
        }
    }
}

/// Joins packets back into messages. A packet that doesn't continue the current message is an error and drops
/// it; a packet with index 0 always starts a new message.
final class Joiner {
    enum Got { case partial, message(Data), error }

    private var buf = [UInt8]()
    private var count = 0, total = 0, next = 0

    func push(_ packet: Data) -> Got {
        let p = [UInt8](packet)
        guard p.count >= Frames.header, p[0] == 0xAA, p[1] == 0x55, p[2] == 0xC3, p[3] == 0x01 else { return fail() }
        let index = Int(p[4]) << 8 | Int(p[5]), cnt = Int(p[6]) << 8 | Int(p[7]), tot = Int(p[8]) << 8 | Int(p[9])
        guard cnt > 0, tot > 0, tot <= Frames.maxMessage else { return fail() }
        if index == 0 {
            buf.removeAll()
            count = cnt; total = tot; next = 0
        } else if index != next || cnt != count || tot != total {
            return fail()
        }
        buf.append(contentsOf: p[Frames.header...])
        if buf.count > total { return fail() }
        next += 1
        if next < count { return .partial }
        guard buf.count == total else { return fail() }
        let done = Data(buf)
        reset()
        return .message(done)
    }

    private func fail() -> Got {
        reset()
        return .error
    }

    private func reset() {
        buf.removeAll()
        count = 0; total = 0; next = 0
    }
}
