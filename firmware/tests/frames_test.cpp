// Tests for ../app_cc_monitor/frames.h (Bluetooth packet splitting and the pairing rule). Run with run.sh.
#include "../app_cc_monitor/frames.h"
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

static int failures = 0;
#define CHECK(cond)                                                      \
    do {                                                                 \
        if (!(cond)) {                                                   \
            fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                  \
        }                                                                \
    } while (0)

using Got = frames::Joiner::Got;

static std::string bytes(std::initializer_list<int> b)
{
    std::string s;
    for (int x : b) s.push_back((char)x);
    return s;
}

static Got push(frames::Joiner& j, const std::string& p)
{
    return j.push(reinterpret_cast<const uint8_t*>(p.data()), p.size());
}

static std::string big(size_t n)
{
    std::string s(n, ' ');
    for (size_t i = 0; i < n; i++) s[i] = (char)('a' + i % 26);
    return s;
}

static void one_packet_message()
{
    auto p = frames::split("{\"a\":1}", 20);
    CHECK(p.size() == 1);
    CHECK(p[0] == bytes({0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 1, 0, 7}) + "{\"a\":1}");
}

static void seven_kilobytes_in_thirty_packets()
{
    std::string msg = big(7000);
    auto p          = frames::split(msg, 244);
    CHECK(p.size() == 30);
    for (size_t i = 0; i < p.size(); i++) {
        CHECK(p[i].substr(0, 4) == bytes({0xAA, 0x55, 0xC3, 0x01}));
        CHECK((uint8_t)p[i][4] == 0 && (uint8_t)p[i][5] == i);
        CHECK((uint8_t)p[i][6] == 0 && (uint8_t)p[i][7] == 30);
        CHECK((uint8_t)p[i][8] == 0x1B && (uint8_t)p[i][9] == 0x58);
        CHECK(p[i].size() == (i < 29 ? 244u : 10u + 214u));
    }

    frames::Joiner j;
    for (size_t i = 0; i < 29; i++) CHECK(push(j, p[i]) == Got::Partial);
    CHECK(push(j, p[29]) == Got::Message);
    CHECK(j.take() == msg);
    CHECK(j.take().empty());   // once only
}

static void out_of_order_packet_then_a_fresh_start()
{
    std::string msg = big(1000);
    auto p          = frames::split(msg, 244);   // 5 packets
    frames::Joiner j;
    CHECK(push(j, p[0]) == Got::Partial);
    CHECK(push(j, p[2]) == Got::Error);
    for (size_t i = 0; i + 1 < p.size(); i++) CHECK(push(j, p[i]) == Got::Partial);
    CHECK(push(j, p.back()) == Got::Message);
    CHECK(j.take() == msg);
}

static void bad_packets_are_errors()
{
    struct Case {
        const char* name;
        std::string packet;
    };
    const std::string ok = frames::split("{\"a\":1}", 20)[0];
    std::string bad_magic = ok;
    bad_magic[2]          = 0x00;
    std::string too_long = bytes({0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 1, 0x40, 0x01}) + "x";   // 16385
    std::string more_than_stated = bytes({0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 1, 0, 3}) + "abcd";
    const Case cases[] = {
        {"wrong magic", bad_magic},
        {"shorter than the header", ok.substr(0, 9)},
        {"total over 16384", too_long},
        {"payload longer than the stated length", more_than_stated},
        {"zero packets", bytes({0xAA, 0x55, 0xC3, 0x01, 0, 0, 0, 0, 0, 1}) + "x"},
    };
    for (const auto& c : cases) {
        frames::Joiner j;
        if (push(j, c.packet) != Got::Error) {
            fprintf(stderr, "case \"%s\": not an error\n", c.name);
            failures++;
        }
    }
}

static void total_length_changing_mid_message()
{
    auto p = frames::split(big(1000), 244);
    frames::Joiner j;
    CHECK(push(j, p[0]) == Got::Partial);
    std::string second = p[1];
    second[9]          = (char)((uint8_t)second[9] + 1);
    CHECK(push(j, second) == Got::Error);
}

static void split_refuses()
{
    CHECK(frames::split(big(16385), 244).empty());
    CHECK(frames::split("", 244).empty());
    CHECK(frames::split("{}", 10).empty());
    CHECK(frames::split(big(16384), 244).size() == 71);
}

static void pairing_window()
{
    struct Case {
        bool usb;
        uint32_t ms;
        bool want;
    };
    const Case cases[] = {{true, 0, true}, {true, 119999, true}, {true, 120000, false}, {false, 10, false}};
    for (const auto& c : cases) {
        if (frames::may_pair(c.usb, c.ms) != c.want) {
            fprintf(stderr, "may_pair(%d, %u) != %d\n", c.usb, c.ms, c.want);
            failures++;
        }
    }
}

int main()
{
    one_packet_message();
    seven_kilobytes_in_thirty_packets();
    out_of_order_packet_then_a_fresh_start();
    bad_packets_are_errors();
    total_length_changing_mid_message();
    split_refuses();
    pairing_window();
    if (failures) {
        fprintf(stderr, "%d failed\n", failures);
        return 1;
    }
    puts("ok");
    return 0;
}
