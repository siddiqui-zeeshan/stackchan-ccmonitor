// Tests for Frames.swift and Relay.swift. Run with test.sh. The byte examples match firmware/tests/frames_test.cpp.
import Foundation

var failures = 0
func check(_ ok: Bool, _ what: String, line: Int = #line) {
    if !ok {
        FileHandle.standardError.write("Tests.swift:\(line): \(what)\n".data(using: .utf8)!)
        failures += 1
    }
}

func big(_ n: Int) -> Data { Data((0..<n).map { UInt8(ascii: "a") + UInt8($0 % 26) }) }

// One packet: the header then the payload.
do {
    let p = Frames.split(Data("{\"a\":1}".utf8), packetSize: 20)
    check(p.count == 1, "one packet")
    check(p.first == Data([0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 1, 0, 7]) + Data("{\"a\":1}".utf8), "one packet bytes")
}

// 7000 bytes at 244 per packet: 30 packets, length 1B 58 in every header, joined back.
do {
    let msg = big(7000)
    let p = Frames.split(msg, packetSize: 244)
    check(p.count == 30, "30 packets")
    for (i, pk) in p.enumerated() {
        let b = [UInt8](pk)
        check(Array(b[0..<4]) == [0xAA, 0x55, 0xC3, 0x01], "magic \(i)")
        check(b[4] == 0 && Int(b[5]) == i && b[6] == 0 && b[7] == 30, "index/count \(i)")
        check(b[8] == 0x1B && b[9] == 0x58, "length \(i)")
        check(pk.count == (i < 29 ? 244 : 224), "size \(i)")
    }
    let j = Joiner()
    for pk in p.dropLast() { if case .partial = j.push(pk) {} else { check(false, "partial") } }
    if case .message(let m) = j.push(p.last!) { check(m == msg, "joined") } else { check(false, "message") }
}

// A packet out of order is an error; a new index 0 starts over.
do {
    let msg = big(1000)
    let p = Frames.split(msg, packetSize: 244)
    let j = Joiner()
    _ = j.push(p[0])
    if case .error = j.push(p[2]) {} else { check(false, "out of order is an error") }
    for pk in p.dropLast() { _ = j.push(pk) }
    if case .message(let m) = j.push(p.last!) { check(m == msg, "fresh start joined") } else { check(false, "fresh start") }
}

// Bad packets.
do {
    let ok = Frames.split(Data("{\"a\":1}".utf8), packetSize: 20)[0]
    var badMagic = ok
    badMagic[2] = 0
    let cases: [(String, Data)] = [
        ("wrong magic", badMagic),
        ("shorter than the header", ok.prefix(9)),
        ("total over 16384", Data([0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 1, 0x40, 0x01]) + Data("x".utf8)),
        ("payload longer than stated", Data([0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 1, 0, 3]) + Data("abcd".utf8)),
        ("zero packets", Data([0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 0, 0, 1]) + Data("x".utf8)),
    ]
    for (name, pk) in cases {
        if case .error = Joiner().push(pk) {} else { check(false, "\(name): not an error") }
    }
}

// split refuses.
check(Frames.split(big(16385), packetSize: 244).isEmpty, "too long")
check(Frames.split(Data(), packetSize: 244).isEmpty, "empty")
check(Frames.split(Data("{}".utf8), packetSize: 10).isEmpty, "packet too small")

// Relay: only head-panel touches go from the robot to the server.
do {
    let r = Relay()
    let cases: [(String, Bool)] = [
        (#"{"type":"touch","gesture":"tap","row":0}"#, true),
        (#"{"type":"act","action":"approve","session":"s"}"#, false),
        (#"{"type":"send","session":"s","text":"rm -rf"}"#, false),
        (#"{"type":"focus","session":"s"}"#, false),
        (#"{"type":"ptt_start","session":"s","rate":16000}"#, false),
        ("not json", false),
        (#"["touch"]"#, false),
    ]
    for (msg, pass) in cases {
        check((r.fromRobot(msg) != nil) == pass, "fromRobot \(msg)")
    }
}

// Relay: a robot that (re)connects gets the latest display at once.
do {
    var r = Relay()
    check(r.robotConnected() == nil, "nothing before a display")
    let d1 = #"{"type":"display","mood":"working"}"#
    let d2 = #"{"type":"display","mood":"needs_you"}"#
    let res = #"{"type":"result","ok":true}"#
    check(r.fromServer(d1) == d1, "display forwarded")
    check(r.fromServer(res) == res, "result forwarded")
    check(r.fromServer(d2) == d2, "second display forwarded")
    check(r.robotConnected() == d2, "latest display on connect")
}

if failures > 0 {
    FileHandle.standardError.write("\(failures) failed\n".data(using: .utf8)!)
    exit(1)
}
print("ok")
