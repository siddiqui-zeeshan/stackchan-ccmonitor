#pragma once
/*
 * Bluetooth build: messages between the robot and cc-ble on the Mac are JSON, split into packets that fit one
 * Bluetooth write or notification. Every packet starts with the stock StackChan header (10 bytes):
 *   AA 55 C3 01, packet index, packet count, total message length   (the last three big-endian uint16)
 * Also the rule for when a new Mac may pair. No LVGL or ESP-IDF here, so firmware/tests builds it on a Mac.
 * mac/ble/Frames.swift is the Mac side of the same format.
 */
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace frames {

inline constexpr size_t HEADER      = 10;
inline constexpr size_t MAX_MESSAGE = 16384;

// Packets of at most `packet_size` bytes; empty when the message is empty, too long, or nothing fits.
inline std::vector<std::string> split(std::string_view msg, size_t packet_size)
{
    std::vector<std::string> out;
    if (msg.empty() || msg.size() > MAX_MESSAGE || packet_size <= HEADER) return out;
    const size_t chunk = packet_size - HEADER;
    const size_t count = (msg.size() + chunk - 1) / chunk;
    for (size_t i = 0; i < count; i++) {
        std::string p = {(char)0xAA, (char)0x55, (char)0xC3, (char)0x01,
                         (char)(i >> 8),          (char)(i & 0xFF),
                         (char)(count >> 8),      (char)(count & 0xFF),
                         (char)(msg.size() >> 8), (char)(msg.size() & 0xFF)};
        p.append(msg.substr(i * chunk, chunk));
        out.push_back(std::move(p));
    }
    return out;
}

// Joins packets back into a message. Any packet that doesn't continue the current message is an error and
// drops it; a packet with index 0 always starts a new message.
class Joiner {
public:
    enum class Got { Partial, Message, Error };

    Got push(const uint8_t* p, size_t n)
    {
        if (n < HEADER || p[0] != 0xAA || p[1] != 0x55 || p[2] != 0xC3 || p[3] != 0x01) return fail();
        const size_t index = (p[4] << 8) | p[5], count = (p[6] << 8) | p[7], total = (p[8] << 8) | p[9];
        if (count == 0 || total == 0 || total > MAX_MESSAGE) return fail();
        if (index == 0) {
            _buf.clear();
            _count = count, _total = total, _next = 0;
        } else if (index != _next || count != _count || total != _total) {
            return fail();
        }
        _buf.append(reinterpret_cast<const char*>(p + HEADER), n - HEADER);
        if (_buf.size() > _total) return fail();
        if (++_next < _count) return Got::Partial;
        if (_buf.size() != _total) return fail();
        _done = std::move(_buf);
        reset();
        return Got::Message;
    }

    std::string take()   // the finished message, once
    {
        std::string t;
        t.swap(_done);
        return t;
    }

private:
    Got fail()
    {
        reset();
        return Got::Error;
    }
    void reset()
    {
        _buf.clear();
        _count = _total = _next = 0;
    }

    std::string _buf, _done;
    size_t _count = 0, _total = 0, _next = 0;
};

// A new Mac may pair only while the robot is plugged into a computer by USB, and only in the first 2 minutes
// after the CC Monitor app started.
inline bool may_pair(bool usb_host, uint32_t ms_since_start)
{
    return usb_host && ms_since_start < 120000;
}

}   // namespace frames
