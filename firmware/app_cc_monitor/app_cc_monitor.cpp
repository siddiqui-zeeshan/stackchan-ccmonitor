/*
 * CC Monitor: shows live Claude Code session state from cc-monitor.
 *
 * The robot only draws what the server sends: {"type":"display","mood","leds","chime","quiet","hush",
 * "big":[n,label],"rest":[...],"spot":{bold,text,sub,session}|null,
 * "rows":[{session,name,project,host,detail,state,ask,say,cut,what?,waitingLong,ageSec,ghostty}],"more","count"}.
 *
 * Main screen: the whole screen takes the mood's colour, with a pixel face on the left, one big number
 * beside it, and one sentence about the session that matters most (cycling when there are several).
 * Tap the sentence to open that session, tap anywhere else for the session list.
 * Moods: working (green grid on a dark screen, or a drifting rainbow: WORKING_GRID), needs_you (red, sweating), still_waiting (orange, gritted teeth),
 * just_finished (yellow smiley + heart), all_done (green smiley), none (asleep), offline (grey).
 * Head: tilted up in every mood except none/offline (down); a light nod when the mood changes; small
 * random "thinking" moves while working. Head panel: tap = acknowledge, long press = quiet mode.
 *
 * The face and backgrounds are drawn as pixels into an LVGL canvas (no stock avatar is attached).
 */
#include "app_cc_monitor.h"
#include "cc_monitor_config.h"   // CC_MONITOR_URL, CC_MONITOR_TOKEN (git-ignored, see the .example)
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <assets/assets.h>
#include <stackchan/stackchan.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <board.h>
#include <web_socket.h>
#include <audio_codec.h>
#include <esp_heap_caps.h>
#include <esp_random.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <mutex>
#include <deque>
#include <vector>

using namespace mooncake;
using namespace stackchan;
using namespace ArduinoJson;

static const char* _tag = "CC-MONITOR";

// ------------------------------------------------------------------ palette

static const uint32_t C_DARK  = 0x131417;   // list / session screen body
static const uint32_t C_PANEL = 0x1D1F24;
static const uint32_t C_BTN   = 0x2B2E34;
static const uint32_t C_TEXT  = 0xF2F3F5;
static const uint32_t C_MUTED = 0x8B8F97;

struct Look {
    uint32_t bg, ink;
    bool rainbow;        // drifting rainbow background; text gets a black outline
    bool grid = false;   // dark screen with a thin green grid and border; everything drawn in one green
};

// The working screen: true = green grid on a dark screen (trial), false = drifting rainbow.
static const bool WORKING_GRID = true;

// Mood ("needs_you", "still_waiting", ...) or session key ("needs", "waiting", "working", "ready") -> colours.
static Look look_of(const std::string& k)
{
    if (k == "needs_you" || k == "needs") return {0xE8380D, 0x190400, false};
    if (k == "still_waiting" || k == "waiting") return {0xF28A0C, 0x1A0A00, false};
    if (k == "working") return WORKING_GRID ? Look{0x2F3B0E, 0x5CF53A, false, true} : Look{0x000000, 0xFFFFFF, true};
    if (k == "just_finished") return {0xF2D40A, 0x1A1500, false};
    if (k == "all_done" || k == "ready") return {0x3CC95A, 0x06200D, false};
    if (k == "none") return {0x18244A, 0x8FA6E8, false};
    return {0x4E5154, 0xC9CCCF, false};   // offline
}

static std::string key_of(const std::string& state, bool waiting_long)
{
    if (state == "needs_you") return waiting_long ? "waiting" : "needs";
    return state == "working" ? "working" : "ready";
}

static uint32_t soft_of(const std::string& key)   // readable text colour for a session's state on the dark body
{
    if (key == "needs") return 0xFF9A82;
    if (key == "waiting") return 0xFFC27A;
    if (key == "working") return 0xC9B8FF;
    return 0x86E79D;
}

static std::string fmt_age(int s)
{
    if (s < 60) return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m";
    return std::to_string(s / 3600) + "h";
}

// ------------------------------------------------------------------ pixel drawing (into RGB565 buffers)

namespace pix {

static const int W = 320, H = 240, G = 4;   // G: size of one "big pixel" of the face

struct Target {
    uint16_t* buf;
    int w, h;
};

static uint16_t* fb = nullptr;   // what the main canvas shows
static uint16_t* nb = nullptr;   // next frame, compared against fb so only changed parts are sent to the screen

static uint16_t* alloc(int w, int h)
{
    return (uint16_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

static uint32_t shade(uint32_t hex, float k)
{
    uint32_t r = (uint32_t)(((hex >> 16) & 0xFF) * k), g = (uint32_t)(((hex >> 8) & 0xFF) * k), b = (uint32_t)((hex & 0xFF) * k);
    return (r << 16) | (g << 8) | b;
}

static uint16_t rgb(uint32_t hex)
{
    return lv_color_to_u16(lv_color_hex(hex));
}

static void fill(Target& t, int x, int y, int w, int h, uint16_t c)
{
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(t.w, x + w), y1 = std::min(t.h, y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t* p = t.buf + yy * t.w;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

static void cell(Target& t, float gx, float gy, int w, int h, uint16_t c)
{
    fill(t, (int)lroundf(gx) * G, (int)lroundf(gy) * G, w * G, h * G, c);
}

static void ell(Target& t, float cx, float cy, float rx, float ry, uint16_t c)
{
    for (int y = (int)floorf(cy - ry); y <= (int)ceilf(cy + ry); y++)
        for (int x = (int)floorf(cx - rx); x <= (int)ceilf(cx + rx); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) fill(t, x * G, y * G, G, G, c);
        }
}

// Solid colour with fine vertical screen lines (every other pair of columns slightly darker).
static void bg_solid(Target& t, uint32_t hex)
{
    uint16_t a = rgb(hex), b = rgb(shade(hex, 0.90f));
    for (int x = 0; x < t.w; x++) t.buf[x] = (x & 3) >= 2 ? b : a;
    for (int y = 1; y < t.h; y++) memcpy(t.buf + y * t.w, t.buf, t.w * 2);
}

// Diagonal rainbow bands, with the same fine screen lines as bg_solid. `step` drifts the bands.
static uint16_t RB[2][12];
static void bg_rainbow(Target& t, int step)
{
    static bool ready = false;
    if (!ready) {
        static const uint32_t hues[12] = {0xFA0505, 0xFA8005, 0xFAFA05, 0x80FA05, 0x05FA05, 0x05FA80,
                                          0x05FAFA, 0x0580FA, 0x0505FA, 0x8005FA, 0xFA05FA, 0xFA0580};
        for (int i = 0; i < 12; i++) {
            RB[0][i] = rgb(hues[i]);
            RB[1][i] = rgb(shade(hues[i], 0.90f));
        }
        ready = true;
    }
    for (int gy = 0; gy * G < t.h; gy++)
        for (int gx = 0; gx * G < t.w; gx++) {
            int band   = ((gx + gy + step) / 6) % 12;
            uint16_t a = RB[0][band], b = RB[1][band];
            for (int yy = gy * G; yy < std::min(t.h, gy * G + G); yy++) {
                uint16_t* p = t.buf + yy * t.w + gx * G;
                int n       = std::min(G, t.w - gx * G);
                for (int i = 0; i < n; i++) p[i] = i >= 2 ? b : a;
            }
        }
}

// Dark screen with thin green lines every `spacing` px (0 = none) and a brighter green border.
static void bg_grid(Target& t, uint32_t hex, int spacing, int border)
{
    bg_solid(t, hex);
    const uint16_t LINE = rgb(0x1E8F35), EDGE = rgb(0x22C52E);
    if (spacing > 0) {
        for (int x = (t.w % spacing) / 2; x < t.w; x += spacing) fill(t, x, 0, 1, t.h, LINE);
        for (int y = (t.h % spacing) / 2; y < t.h; y += spacing) fill(t, 0, y, t.w, 1, LINE);
    }
    fill(t, 0, 0, t.w, border, EDGE);
    fill(t, 0, t.h - border, t.w, border, EDGE);
    fill(t, 0, 0, border, t.h, EDGE);
    fill(t, t.w - border, 0, border, t.h, EDGE);
}

static void band(Target& t, const std::string& key)   // header band of the list / session screen
{
    Look l = look_of(key);
    if (l.rainbow) bg_rainbow(t, 0);
    else if (l.grid) bg_grid(t, l.bg, 16, 2);
    else bg_solid(t, l.bg);
}

static const char* HEART[8] = {".XX...XX.", "XXXX.XXXX", "XXXXXXXXX", "XXXXXXXXX",
                               ".XXXXXXX.", "..XXXXX..", "...XXX...", "....X...."};

// The face for one mood at time `s` seconds, centred near cell (fx, fy).
static void face(Target& t, const std::string& mood, float s, int fx, int fy, uint32_t ink_hex)
{
    const uint16_t K = rgb(0x000000), INK = rgb(ink_hex);
    const uint16_t WH = look_of("working").grid ? INK : rgb(0xFFFFFF);   // working face: one colour
    if (mood == "working") {
        int look = (int)lroundf(sinf(s * 1.4f) * 1.2f);
        bool bl  = fmodf(s, 3.4f) > 3.25f;
        for (int e = -1; e <= 1; e += 2) {
            if (bl) cell(t, fx + e * 10 - 3 + look, fy - 3, 8, 1, WH);
            else ell(t, fx + e * 10 + look + 0.5f, fy - 2.5f, 4.2f, 4.2f, WH);
        }
        bool wide = ((int)(s / 0.45f)) % 5 == 0;
        cell(t, fx - (wide ? 3 : 2), fy + 9, wide ? 8 : 6, 2, WH);
    } else if (mood == "needs_you") {   // sweating: only the drops move
        for (int e = -1; e <= 1; e += 2) ell(t, fx + e * 9 + 0.5f, fy - 0.5f, 4.2f, 4.2f, K);
        for (int ox : {fx - 21, fx + 11})
            for (int k = 0; k < 3; k++)
                for (int j = 0; j < 4; j++) cell(t, ox + k * 3 + j, fy + 12 - j, 2, 1, K);
        static const float drops[5][4] = {{-17, -9, -1, 0}, {-10, -14, -0.4f, 0.35f}, {-1, -10, 0, 0.6f},
                                          {6, -16, 0.3f, 0.15f}, {15, -11, 1, 0.75f}};
        const uint16_t DA = rgb(0xEAFBFF), DB = rgb(0xBFE9F5);
        for (auto& d : drops) {
            float p = fmodf(s / 1.05f + d[3], 1.0f);
            if (p > 0.8f) continue;
            float x = fx + d[0] + d[2] * p * 3, y = fy + d[1] + p * 4;
            cell(t, x, y, 1, 2, DA);
            cell(t, x - 1, y + 2, 3, 2, DA);
            cell(t, x, y + 4, 1, 1, DB);
        }
    } else if (mood == "still_waiting") {   // gritted teeth, chattering
        bool twitch = fmodf(s, 1.8f) > 1.68f;
        for (int e = -1; e <= 1; e += 2) ell(t, fx + e * 10 + 0.5f, fy - 2.5f, 2.6f, (twitch && e > 0) ? 2.2f : 3.6f, K);
        bool nub = ((int)(s / 0.11f)) % 4 == 0;
        int y0   = fy + 6;
        cell(t, fx - 8, y0, 17, 1, K);
        cell(t, fx - 9, y0 + 1, 19, 1, K);
        cell(t, fx - 10, y0 + 2, 21, 5, K);
        cell(t, fx - 9, y0 + 7, 19, 1, K);
        cell(t, fx - 8, y0 + 8, 17, 1, K);
        if (nub) {
            cell(t, fx - 11, y0 + 3, 1, 3, K);
            cell(t, fx + 11, y0 + 3, 1, 3, K);
        }
        const uint16_t TEETH = rgb(0xC5F3E6);
        for (int i = 0; i < 6; i++) {
            cell(t, fx - 8 + i * 3, y0 + 2, 2, 2, TEETH);
            cell(t, fx - 8 + i * 3, y0 + 5, 2, 2, TEETH);
        }
    } else if (mood == "just_finished" || mood == "all_done") {   // smiley; blink = stretch, squash, back
        float p   = fmodf(s, 3.2f) / 3.2f;
        bool tall = p > 0.86f && p <= 0.90f, flat = p > 0.90f && p <= 0.95f;
        for (int e = -1; e <= 1; e += 2)
            ell(t, fx + e * 10 + 0.5f, fy - 2.5f, flat ? 3.6f : tall ? 2.2f : 2.6f, flat ? 1.0f : tall ? 4.6f : 3.6f, K);
        int my = fy + 8 + (flat ? 1 : 0);
        cell(t, fx - 6, my, 13, 1, K);
        cell(t, fx - 5, my + 1, 11, 1, K);
        cell(t, fx - 4, my + 2, 9, 1, K);
        cell(t, fx - 2, my + 3, 5, 1, K);
        if (mood == "just_finished") {
            int hy = fy - 19 + (int)lroundf(sinf(s * 2.6f) * 1.5f), hx = fx + 14;
            const uint16_t RED = rgb(0xFF2638);
            for (int y = 0; y < 8; y++)
                for (int x = 0; x < 9; x++)
                    if (HEART[y][x] == 'X') cell(t, hx + x, hy + y, 1, 1, RED);
            cell(t, hx + 2, hy + 1, 1, 1, rgb(0xFFC2C8));
        }
    } else if (mood == "none") {   // asleep
        for (int e = -1; e <= 1; e += 2) {
            int ex = fx + e * 10;
            cell(t, ex - 3, fy - 1, 7, 1, INK);
            cell(t, ex - 4, fy - 2, 1, 1, INK);
            cell(t, ex + 4, fy - 2, 1, 1, INK);
        }
        cell(t, fx - 1, fy + 9, 3, 1, INK);
        float p = fmodf(s, 3.6f) / 3.6f;
        if (p > 0.1f && p < 0.8f) {   // a small z drifting up
            int zx = fx + 15 + (int)lroundf(p * 4), zy = fy - 8 - (int)lroundf(p * 8);
            cell(t, zx, zy, 5, 1, INK);
            cell(t, zx + 3, zy + 1, 1, 1, INK);
            cell(t, zx + 2, zy + 2, 1, 1, INK);
            cell(t, zx + 1, zy + 3, 1, 1, INK);
            cell(t, zx, zy + 4, 5, 1, INK);
        }
    } else {   // offline: crossed eyes; the picture jumps sideways now and then
        int jump = fmodf(s, 4.2f) > 4.05f ? 2 : 0;
        for (int e = -1; e <= 1; e += 2)
            for (int j = -2; j <= 2; j++) {
                cell(t, fx + e * 10 + j + jump, fy - 2 + j, 1, 1, INK);
                cell(t, fx + e * 10 + j + jump, fy - 2 - j, 1, 1, INK);
            }
        static const int zig[4] = {0, -1, -2, -1};
        for (int x = -7; x <= 7; x++) cell(t, fx + x + jump, fy + 9 + zig[((x % 4) + 4) % 4], 1, 1, INK);
    }
}

// The big number: chunky 6x7 digits, 8 px per cell.
static const uint8_t DIGITS[10][7] = {
    {0b011110, 0b110011, 0b110011, 0b110011, 0b110011, 0b110011, 0b011110},
    {0b001100, 0b011100, 0b001100, 0b001100, 0b001100, 0b001100, 0b011110},
    {0b011110, 0b110011, 0b000011, 0b000110, 0b001100, 0b011000, 0b111111},
    {0b111110, 0b000011, 0b000011, 0b011110, 0b000011, 0b000011, 0b111110},
    {0b000110, 0b001110, 0b010110, 0b100110, 0b111111, 0b000110, 0b000110},
    {0b111111, 0b110000, 0b111110, 0b000011, 0b000011, 0b110011, 0b011110},
    {0b011110, 0b110000, 0b110000, 0b111110, 0b110011, 0b110011, 0b011110},
    {0b111111, 0b000011, 0b000110, 0b001100, 0b001100, 0b001100, 0b001100},
    {0b011110, 0b110011, 0b110011, 0b011110, 0b110011, 0b110011, 0b011110},
    {0b011110, 0b110011, 0b110011, 0b011111, 0b000011, 0b000011, 0b011110},
};

static void number(Target& t, int n, int cx, int y, uint16_t c, bool outline)
{
    const int D = 8, O = 3;
    n           = std::max(0, std::min(99, n));
    int digits[2], count = 0;
    if (n >= 10) digits[count++] = n / 10;
    digits[count++] = n % 10;
    int x0 = cx - (count * 6 * D + (count - 1) * D) / 2;
    for (int pass = outline ? 0 : 1; pass < 2; pass++) {   // pass 0: every cell grown by O px in black
        int x = x0, o = pass ? 0 : O;
        for (int i = 0; i < count; i++, x += 7 * D)
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 6; b++)
                    if (DIGITS[digits[i]][r] & (0b100000 >> b))
                        fill(t, x + b * D - o, y + r * D - o, D + 2 * o, D + 2 * o, pass ? c : rgb(0x000000));
    }
}

// One frame of the main screen. `solo` = no number or sentence columns (nothing running / offline).
static void frame(Target& t, const std::string& mood, bool solo, int big, uint32_t ms, uint32_t tick)
{
    Look l  = look_of(mood);
    float s = (ms % 600000) / 1000.0f;
    if (l.rainbow) bg_rainbow(t, (int)tick);
    else if (l.grid) bg_grid(t, l.bg, 32, 3);
    else bg_solid(t, l.bg);
    face(t, mood, s, solo ? 40 : 26, 20, l.ink);
    if (!solo) {
        if (big > 0) number(t, big, 258, 34, rgb(l.ink), l.rainbow);
        // Line above the sentence. On the rainbow: white with a thin black edge, like the text.
        if (l.rainbow) fill(t, 11, 176, 298, 4, rgb(0x000000));
        fill(t, 12, 177, 296, 2, rgb(l.rainbow ? 0xFFFFFF : l.grid ? l.ink : shade(l.bg, 0.62f)));
    }
}

// Copy the rows of `nb` that differ from `fb` and report their bounding box (false = nothing changed).
static bool diff(lv_area_t& out)
{
    int x0 = W, x1 = -1, y0 = -1, y1 = -1;
    for (int y = 0; y < H; y++) {
        uint16_t *a = fb + y * W, *b = nb + y * W;
        if (memcmp(a, b, W * 2) == 0) continue;
        int l = 0, r = W - 1;
        while (a[l] == b[l]) l++;
        while (a[r] == b[r]) r--;
        x0 = std::min(x0, l);
        x1 = std::max(x1, r);
        if (y0 < 0) y0 = y;
        y1 = y;
        memcpy(a, b, W * 2);
    }
    if (y0 < 0) return false;
    out.x1 = x0;
    out.y1 = y0;
    out.x2 = x1;
    out.y2 = y1;
    return true;
}

// Small face for a session's state, on a 12x10 grid of g-sized cells.
static void mini(Target& t, const std::string& key, int x, int y, int g)
{
    const Look wl    = look_of("working");
    const uint16_t K = rgb(key != "working" ? 0x000000 : wl.grid ? wl.ink : 0xFFFFFF);
    auto p           = [&](int gx, int gy, int w, int h, uint16_t c) { fill(t, x + gx * g, y + gy * g, w * g, h * g, c); };
    if (key == "needs") {
        p(2, 3, 3, 3, K);
        p(7, 3, 3, 3, K);
        p(1, 0, 1, 2, rgb(0xEAFBFF));
        p(10, 1, 1, 2, rgb(0xEAFBFF));
        p(1, 8, 2, 1, K);
        p(9, 8, 2, 1, K);
    } else if (key == "waiting") {
        p(2, 1, 2, 3, K);
        p(8, 1, 2, 3, K);
        p(1, 6, 10, 4, K);
        for (int i = 0; i < 4; i++) p(2 + i * 2, 7, 1, 2, rgb(0xC5F3E6));
    } else if (key == "working") {   // white on the rainbow: black edge first so it stands out
        static const int parts[3][4] = {{2, 2, 3, 3}, {7, 2, 3, 3}, {4, 8, 4, 1}};
        const int o = std::max(1, g / 2);
        if (wl.rainbow)
            for (auto& q : parts) fill(t, x + q[0] * g - o, y + q[1] * g - o, q[2] * g + 2 * o, q[3] * g + 2 * o, rgb(0x000000));
        for (auto& q : parts) p(q[0], q[1], q[2], q[3], K);
    } else {
        p(2, 2, 2, 3, K);
        p(8, 2, 2, 3, K);
        p(2, 7, 8, 1, K);
        p(3, 8, 6, 1, K);
        p(4, 9, 4, 1, K);
    }
}

// 28x28 face block for a list row.
static void block(Target& t, const std::string& key)
{
    Look l = look_of(key);
    if (l.rainbow) bg_rainbow(t, 0);
    else if (l.grid) bg_grid(t, l.bg, 0, 2);
    else bg_solid(t, l.bg);
    mini(t, key, 2, 4, 2);
}

}   // namespace pix

// ------------------------------------------------------------------ connection (own task)

static std::mutex _rx_mutex;
static std::deque<std::string> _rx_queue;   // server messages, applied on the UI loop
static std::string _net_msg;             // Wi-Fi / connection progress text
static std::atomic<bool> _running{false};
static std::atomic<bool> _connected{false};
static std::atomic<bool> _task_alive{false};
static std::mutex _tx_mutex;
static std::unique_ptr<WebSocket> _ws;

static void ws_task(void*)
{
    _task_alive = true;
    // Wi-Fi is not started until an app asks for it; connecting before that asserts in lwip.
    GetHAL().startNetwork([](std::string_view msg) {
        std::lock_guard<std::mutex> lock(_rx_mutex);
        _net_msg = std::string(msg);
    });
    while (_running) {
        {
            std::lock_guard<std::mutex> lock(_rx_mutex);
            _net_msg = "Connecting to server...";
        }
        auto network = Board::GetInstance().GetNetwork();
        auto ws      = network ? network->CreateWebSocket(1) : nullptr;
        if (ws) {
            std::string auth = std::string("Bearer ") + CC_MONITOR_TOKEN;
            ws->SetHeader("Authorization", auth.c_str());
            ws->SetReceiveBufferSize(16384);
            ws->OnData([](const char* data, size_t len, bool binary) {
                if (binary) return;
                std::lock_guard<std::mutex> lock(_rx_mutex);
                if (_rx_queue.size() >= 8) _rx_queue.pop_front();
                _rx_queue.emplace_back(data, len);
            });
            if (ws->Connect(CC_MONITOR_URL)) {
                {
                    std::lock_guard<std::mutex> lock(_tx_mutex);
                    _ws = std::move(ws);
                }
                _connected = true;
                mclog::tagInfo(_tag, "connected");
                while (_running) {
                    {
                        std::lock_guard<std::mutex> lock(_tx_mutex);
                        if (!_ws || !_ws->IsConnected()) break;
                    }
                    vTaskDelay(pdMS_TO_TICKS(500));
                }
                _connected = false;
                std::lock_guard<std::mutex> lock(_tx_mutex);
                _ws.reset();
                mclog::tagInfo(_tag, "disconnected");
            } else {
                mclog::tagWarn(_tag, "connect failed");
                std::lock_guard<std::mutex> lock(_rx_mutex);
                _net_msg = "Server unreachable, retrying...";
            }
        }
        for (int i = 0; i < 10 && _running; i++) vTaskDelay(pdMS_TO_TICKS(500));   // retry in 5 s
    }
    _task_alive = false;
    vTaskDelete(nullptr);
}

static void send_json(const std::string& s)
{
    std::lock_guard<std::mutex> lock(_tx_mutex);
    if (_ws && _ws->IsConnected()) _ws->Send(s);
}

static std::string jstr(const std::string& s)
{
    JsonDocument d;
    d.set(s);
    std::string out;
    serializeJson(d, out);
    return out;
}

static void send_touch(const char* gesture)
{
    std::lock_guard<std::mutex> lock(_tx_mutex);
    if (_ws && _ws->IsConnected()) {
        _ws->Send(std::string(R"({"type":"touch","gesture":")") + gesture + R"(","row":0})");
    }
}

// ------------------------------------------------------------------ small LVGL helpers

static lv_obj_t* box(lv_obj_t* parent, uint32_t color, lv_opa_t opa)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

// Stepped "pixel" corners: paint a small square of the colour behind the box over each corner.
static void notch_cb(lv_event_t* e)
{
    lv_obj_t* o       = (lv_obj_t*)lv_event_get_target(e);
    lv_layer_t* layer = lv_event_get_layer(e);
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    lv_draw_rect_dsc_t d;
    lv_draw_rect_dsc_init(&d);
    d.bg_color = lv_color_hex((uint32_t)(uintptr_t)lv_event_get_user_data(e));
    d.bg_opa   = LV_OPA_COVER;
    const int s = 4;
    for (int i = 0; i < 4; i++) {
        lv_area_t c;
        c.x1 = (i & 1) ? a.x2 - s + 1 : a.x1;
        c.y1 = (i & 2) ? a.y2 - s + 1 : a.y1;
        c.x2 = c.x1 + s - 1;
        c.y2 = c.y1 + s - 1;
        lv_draw_rect(layer, &d, &c);
    }
}

static void notch(lv_obj_t* o, uint32_t behind)
{
    lv_obj_add_event_cb(o, notch_cb, LV_EVENT_DRAW_MAIN_END, (void*)(uintptr_t)behind);
}

static void show(lv_obj_t* o, bool on);

static lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
}

static void set_text(lv_obj_t* label, const std::string& s)   // only touch the label when the text changed
{
    if (s != lv_label_get_text(label)) lv_label_set_text(label, s.c_str());
}

// The built-in fonts can't draw an outline, so a label that must read on the rainbow gets eight black
// copies of itself, one pixel around it. halo() keeps the copies in step with the label.
struct Halo {
    lv_obj_t* label;
    lv_obj_t* copy[8];
};
static std::vector<Halo> _halos;

static lv_obj_t* outlined_text(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    Halo h;
    for (auto& c : h.copy) {
        c = text(parent, font, 0x000000);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
    }
    h.label = text(parent, font, color);   // created last, so it is drawn on top of its copies
    _halos.push_back(h);
    return h.label;
}

static void halo(lv_obj_t* label, bool on)
{
    static const int dx[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, dy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
    for (auto& h : _halos) {
        if (h.label != label) continue;
        bool visible = on && !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN);
        for (int i = 0; i < 8; i++) {
            lv_obj_t* c = h.copy[i];
            show(c, visible);
            if (!visible) continue;
            set_text(c, lv_label_get_text(label));
            lv_label_set_long_mode(c, lv_label_get_long_mode(label));
            lv_obj_set_width(c, lv_obj_get_style_width(label, LV_PART_MAIN));
            lv_obj_set_style_text_align(c, lv_obj_get_style_text_align(label, LV_PART_MAIN), 0);
            lv_obj_set_pos(c, lv_obj_get_style_x(label, LV_PART_MAIN) + dx[i], lv_obj_get_style_y(label, LV_PART_MAIN) + dy[i]);
        }
    }
}

static lv_obj_t* canvas(lv_obj_t* parent, uint16_t* buf, int w, int h)
{
    lv_obj_t* c = lv_canvas_create(parent);
    lv_canvas_set_buffer(c, buf, w, h, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    return c;
}

static void anim_opa_cb(void* o, int32_t v)
{
    lv_obj_set_style_opa((lv_obj_t*)o, (lv_opa_t)v, 0);
}

static void anim_y_cb(void* o, int32_t v)
{
    lv_obj_set_y((lv_obj_t*)o, v);
}

static void fade_in(lv_obj_t* o, uint32_t ms)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 0, 255);
    lv_anim_set_duration(&a, ms);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void slide_x(lv_obj_t* o, int to)
{
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, [](void* v, int32_t x) { lv_obj_set_x((lv_obj_t*)v, x); });
    lv_anim_set_values(&a, lv_obj_get_x(o), to);
    lv_anim_set_duration(&a, 260);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static void show(lv_obj_t* o, bool on)
{
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

static float frand(float lo, float hi)
{
    return lo + (hi - lo) * (float)(esp_random() % 10001) / 10000.0f;
}

// ------------------------------------------------------------------ session screen + push-to-talk

static lv_obj_t* _ss = nullptr;            // session screen (slides in from the right)
static lv_obj_t *_ss_band, *_ss_title, *_ss_sub, *_ss_status, *_ss_ask_box, *_ss_ask, *_ss_note;
static lv_obj_t *_ss_reject, *_ss_approve, *_ss_approve_fill, *_ss_approve_label, *_ss_talk, *_ss_talk_label, *_ss_focus;
static lv_obj_t *_ss_confirm, *_ss_confirm_text, *_ss_toast, *_ss_toast_label;
static uint16_t* _ss_band_px    = nullptr;     // 320x54
static uint16_t* _sheet_band_px = nullptr;     // 320x44
static uint16_t* _mini_px[12]   = {};          // 28x28 each
static std::string _ss_band_key;
static std::atomic<bool> _back_flag{false}, _reject_flag{false}, _focus_flag{false}, _send_flag{false}, _cancel_flag{false};
static std::atomic<uint32_t> _approve_down{0};   // ms when the Approve press started, 0 = not pressed
static std::atomic<bool> _talk_down{false};
static std::atomic<bool> _rec{false};
static std::atomic<bool> _mic_alive{false};
static uint32_t _toast_until = 0;
static bool _transcribing = false;
static uint32_t _transcribe_ms = 0;   // when we started waiting for the transcript
static std::string _pending_text;

static std::atomic<int> _mic_rate{16000};

static void mic_task(void*)
{
    // Direct codec read. (The xiaozhi AudioService is not initialised in this mode, so it can't be used.)
    _mic_alive = true;
    auto codec = Board::GetInstance().GetAudioCodec();
    int rate   = codec->input_sample_rate();
    int ch     = std::max(1, codec->input_channels());
    codec->EnableInput(true);
    std::vector<int16_t> buf(rate / 50 * ch), mono(rate / 50);   // 20 ms frames
    uint32_t start = GetHAL().millis();
    int frames     = 0;
    const char* why = "released";
    while (true) {
        if (!_rec) break;
        if (GetHAL().millis() - start >= 60000) { why = "60 s limit"; break; }
        if (!codec->InputData(buf)) { why = "mic read failed"; break; }
        for (size_t i = 0; i < mono.size(); i++) mono[i] = buf[i * ch];   // first channel = microphone
        std::lock_guard<std::mutex> lock(_tx_mutex);
        if (!_ws || !_ws->IsConnected()) { why = "disconnected"; break; }
        if (!_ws->Send(mono.data(), mono.size() * 2, true)) { why = "send failed"; break; }
        frames++;
    }
    codec->EnableInput(false);
    mclog::tagInfo(_tag, "talk: recorded {} ms, {} frames at {} Hz x{} ch, stopped: {}", GetHAL().millis() - start,
                   frames, rate, ch, why);
    send_json(R"({"type":"ptt_end"})");
    _rec       = false;
    _mic_alive = false;
    vTaskDelete(nullptr);
}

static lv_obj_t* button(lv_obj_t* parent, const char* label, uint32_t bg, uint32_t fg, int x, int y, int w, int h,
                        lv_obj_t** label_out = nullptr, uint32_t behind = C_DARK)
{
    lv_obj_t* b = box(parent, bg, LV_OPA_COVER);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_PRESS_LOCK);           // stay pressed if the finger drifts
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_CHAIN);      // a drift must not scroll the screen (= press lost)
    lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    notch(b, behind);
    lv_obj_t* l = text(b, &lv_font_montserrat_16, fg);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

static void build_session_screen(lv_obj_t* scr)
{
    _ss = box(scr, C_DARK, LV_OPA_COVER);
    lv_obj_set_size(_ss, 320, 240);
    lv_obj_set_pos(_ss, 320, 0);
    lv_obj_add_flag(_ss, LV_OBJ_FLAG_CLICKABLE);   // swallow taps so they don't reach the main screen

    // Header band in the session's colour, with its small face on the right (drawn into the band).
    _ss_band = canvas(_ss, _ss_band_px, 320, 54);
    lv_obj_set_pos(_ss_band, 0, 0);
    _ss_band_key.clear();

    lv_obj_t* back = box(_ss, 0x000000, LV_OPA_COVER);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 8, 10);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_t* bl = text(back, &lv_font_montserrat_16, 0xFFFFFF);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { _back_flag = true; }, LV_EVENT_CLICKED, nullptr);

    _ss_title = outlined_text(_ss, &lv_font_montserrat_20, 0x000000);
    lv_obj_set_width(_ss_title, 206);
    lv_label_set_long_mode(_ss_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_title, 52, 7);
    _ss_sub = outlined_text(_ss, &lv_font_montserrat_14, 0x000000);
    lv_obj_set_width(_ss_sub, 206);
    lv_label_set_long_mode(_ss_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_sub, 52, 32);

    _ss_status = text(_ss, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_width(_ss_status, 292);
    lv_label_set_long_mode(_ss_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_status, 14, 60);

    _ss_ask_box = box(_ss, C_PANEL, LV_OPA_COVER);
    lv_obj_set_size(_ss_ask_box, 300, 64);
    lv_obj_set_pos(_ss_ask_box, 10, 86);
    notch(_ss_ask_box, C_DARK);
    _ss_ask = text(_ss_ask_box, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(_ss_ask, 280);
    lv_label_set_long_mode(_ss_ask, LV_LABEL_LONG_DOT);
    lv_obj_set_height(_ss_ask, 50);
    lv_obj_set_pos(_ss_ask, 10, 7);

    _ss_note = text(_ss, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(_ss_note, 300);
    lv_obj_set_style_text_align(_ss_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(_ss_note, 10, 208);

    _ss_reject = button(_ss, LV_SYMBOL_CLOSE "  Reject", C_BTN, 0xFF9A82, 10, 156, 146, 38);
    lv_obj_add_event_cb(_ss_reject, [](lv_event_t*) { _reject_flag = true; }, LV_EVENT_CLICKED, nullptr);
    _ss_approve      = button(_ss, "", 0x17592A, 0xFFFFFF, 164, 156, 146, 38);
    _ss_approve_fill = box(_ss_approve, 0x3CC95A, LV_OPA_COVER);
    lv_obj_set_size(_ss_approve_fill, 0, 38);
    _ss_approve_label = text(_ss_approve, &lv_font_montserrat_16, 0xFFFFFF);
    lv_label_set_text(_ss_approve_label, "Hold to approve");
    lv_obj_center(_ss_approve_label);
    lv_obj_add_event_cb(_ss_approve, [](lv_event_t*) { _approve_down = GetHAL().millis() | 1; }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(_ss_approve, [](lv_event_t*) { _approve_down = 0; }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(_ss_approve, [](lv_event_t*) { _approve_down = 0; }, LV_EVENT_PRESS_LOST, nullptr);

    _ss_talk = button(_ss, "", 0x163764, 0xA9CEFF, 10, 200, 196, 34, &_ss_talk_label);
    lv_label_set_text(_ss_talk_label, LV_SYMBOL_AUDIO "  Hold to talk");
    lv_obj_add_event_cb(_ss_talk, [](lv_event_t*) { _talk_down = true; }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(_ss_talk, [](lv_event_t*) { _talk_down = false; }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(_ss_talk, [](lv_event_t*) { mclog::tagInfo(_tag, "talk: press lost"); }, LV_EVENT_PRESS_LOST, nullptr);
    _ss_focus = button(_ss, LV_SYMBOL_EYE_OPEN "  Mac", C_BTN, C_TEXT, 214, 200, 96, 34);
    lv_obj_add_event_cb(_ss_focus, [](lv_event_t*) { _focus_flag = true; }, LV_EVENT_CLICKED, nullptr);

    // Transcript confirmation (over the session screen).
    const uint32_t C_CONFIRM = 0x0F1012;
    _ss_confirm              = box(_ss, C_CONFIRM, LV_OPA_COVER);
    lv_obj_set_size(_ss_confirm, 300, 156);
    lv_obj_set_pos(_ss_confirm, 10, 76);
    lv_obj_set_style_border_color(_ss_confirm, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(_ss_confirm, 2, 0);
    lv_obj_add_flag(_ss_confirm, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t* ch = text(_ss_confirm, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(ch, "Send this prompt?");
    lv_obj_set_pos(ch, 14, 10);
    _ss_confirm_text = text(_ss_confirm, &lv_font_montserrat_16, 0xFFFFFF);
    lv_obj_set_width(_ss_confirm_text, 270);
    lv_obj_set_height(_ss_confirm_text, 62);
    lv_label_set_long_mode(_ss_confirm_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_confirm_text, 14, 32);
    lv_obj_t* cancel = button(_ss_confirm, "Cancel", C_BTN, C_TEXT, 12, 106, 132, 36, nullptr, C_CONFIRM);
    lv_obj_add_event_cb(cancel, [](lv_event_t*) { _cancel_flag = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* send = button(_ss_confirm, LV_SYMBOL_UPLOAD "  Send", 0x1F6FE0, 0xFFFFFF, 152, 106, 132, 36, nullptr, C_CONFIRM);
    lv_obj_add_event_cb(send, [](lv_event_t*) { _send_flag = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(_ss_confirm, LV_OBJ_FLAG_HIDDEN);

    // Toast for results ("Approved", "Sent", errors).
    _ss_toast = box(scr, 0x000000, LV_OPA_COVER);
    lv_obj_set_size(_ss_toast, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_hor(_ss_toast, 16, 0);
    lv_obj_set_style_border_color(_ss_toast, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_border_width(_ss_toast, 2, 0);
    lv_obj_align(_ss_toast, LV_ALIGN_TOP_MID, 0, 8);
    _ss_toast_label = text(_ss_toast, &lv_font_montserrat_16, C_TEXT);
    lv_obj_center(_ss_toast_label);
    lv_obj_add_flag(_ss_toast, LV_OBJ_FLAG_HIDDEN);
}

static void toast(const std::string& msg, uint32_t color)
{
    lv_label_set_text(_ss_toast_label, msg.c_str());
    lv_obj_set_style_text_color(_ss_toast_label, lv_color_hex(color), 0);
    lv_obj_remove_flag(_ss_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(_ss_toast);
    fade_in(_ss_toast, 200);
    _toast_until = GetHAL().millis() + 2600;
}

// ------------------------------------------------------------------ chimes (synthesised)
// The firmware's Ogg player lives in the voice assistant's AudioService, which isn't started in this
// mode (calling it crashes), so chimes are generated here and written straight to the speaker.

static std::atomic<bool> _chime_busy{false};

static void chime_task(void* arg)
{
    const bool longer = arg != nullptr;
    auto codec        = Board::GetInstance().GetAudioCodec();
    const int rate    = codec->output_sample_rate();
    const int ch      = std::max(1, codec->output_channels());
    codec->EnableOutput(true);
    // (frequency Hz, length ms): brief = two-note "ding-dong"; longer = rising four-note phrase, twice.
    static const float brief[][2]  = {{1318.5f, 140}, {1760.0f, 420}};
    static const float phrase[][2] = {{1046.5f, 170}, {1318.5f, 170}, {1568.0f, 170}, {2093.0f, 520},
                                      {0, 220},
                                      {1046.5f, 170}, {1318.5f, 170}, {1568.0f, 170}, {2093.0f, 700}};
    const float (*notes)[2] = longer ? phrase : brief;
    const int count         = longer ? 9 : 2;
    std::vector<int16_t> buf(rate / 50 * ch);   // 20 ms chunks
    for (int n = 0; n < count; n++) {
        const float f = notes[n][0];
        const int total = (int)(notes[n][1] * rate / 1000);
        for (int done = 0; done < total;) {
            int k = std::min<int>(rate / 50, total - done);
            for (int i = 0; i < k; i++) {
                float t   = (float)(done + i) / rate;
                float env = std::min(1.0f, t * 200.0f) * expf(-t * (longer ? 5.0f : 6.0f));   // 5 ms attack, bell decay
                float s   = f > 0 ? env * (0.75f * sinf(2 * 3.14159f * f * t) + 0.25f * sinf(4 * 3.14159f * f * t)) : 0;
                for (int c2 = 0; c2 < ch; c2++) buf[i * ch + c2] = (int16_t)(s * 9000);
            }
            std::vector<int16_t> out(buf.begin(), buf.begin() + k * ch);
            codec->OutputData(out);
            done += k;
        }
    }
    _chime_busy = false;
    vTaskDelete(nullptr);
}

static void play_chime(bool longer)
{
    if (_chime_busy.exchange(true)) return;
    xTaskCreate(chime_task, "cc_chime", 4096, longer ? (void*)1 : nullptr, 4, nullptr);
}

// ------------------------------------------------------------------ app lifecycle

// Head pitch, -1 .. 1, maps to about 3 .. 87 degrees up. -0.357 is 30 degrees: tilted up towards someone
// at a desk. (64, 50 and 35 degrees were all too steep on the robot.)
static const float HEAD_UP   = -0.357f;  // every mood except nothing running / offline
static const float HEAD_DOWN = -0.7f;

static std::atomic<bool> _tap_flag{false};
static std::atomic<bool> _quit_flag{false};
static std::atomic<int> _open_flag{0};            // 1..12 = list row, 100 = the session in the main-screen sentence
static std::atomic<bool> _scrolled_flag{false};   // list scrolled: keep the sheet open
static size_t _touch_conn = 0;
static std::atomic<uint32_t> _press_ms{0};
static std::string _styled;   // mood the main-screen label colours / layout were last set for

AppCcMonitor::AppCcMonitor()
{
    setAppInfo().name = "CC MONITOR";
    static auto icon  = assets::get_image("icon_ai_agent.bin");
    setAppInfo().icon = (void*)&icon;
    static uint32_t theme_color = 0xFF9F0A;
    setAppInfo().userData       = (void*)&theme_color;
}

void AppCcMonitor::onCreate()
{
    mclog::tagInfo(_tag, "on create");
}

void AppCcMonitor::onOpen()
{
    mclog::tagInfo(_tag, "on open");
    if (!pix::fb) {   // pixel buffers live in PSRAM for the life of the firmware
        pix::fb        = pix::alloc(pix::W, pix::H);
        pix::nb        = pix::alloc(pix::W, pix::H);
        _ss_band_px    = pix::alloc(320, 54);
        _sheet_band_px = pix::alloc(320, 44);
        for (auto& p : _mini_px) p = pix::alloc(28, 28);
    }
    memset(pix::fb, 0, pix::W * pix::H * 2);
    _mood = "";
    _rows.clear();
    _count = _more = _big = 0;
    _big_text.clear();
    _rest_text.clear();
    _has_spot      = false;
    _quiet = _hush = false;
    _sheet_open    = false;
    _was_connected = false;
    _spot_index    = 0;
    _shown_session.clear();
    _sheet_band_key.clear();
    for (auto& k : _sheet_keys) k.clear();
    _nod_at = _nod_until = 0;
    _pose_pending = _was_hold = false;
    _tap_flag  = false;
    _quit_flag = false;
    {
        LvglLockGuard lock;
        GetStackChan().clearModifiers();   // this app draws its own face: no stock avatar or its effects
        GetStackChan().resetAvatar();
        _styled = "?";
        buildUi();
        setMood("offline");
        renderMain(false);
        renderSheet();
    }

    _touch_conn = GetHAL().onHeadPetGesture.connect([](HeadPetGesture g) {
        uint32_t now = GetHAL().millis();
        if (g == HeadPetGesture::Press) {
            _press_ms = now;
        } else if (g == HeadPetGesture::Release) {
            uint32_t held = now - _press_ms.load();
            if (held >= 1500) {
                send_touch("long_press");
            } else if (held < 600) {
                send_touch("tap");
            }
        } else if (g == HeadPetGesture::SwipeForward || g == HeadPetGesture::SwipeBackward) {
            send_touch("stroke");
        }
    });

    _running = true;
    if (!_task_alive) {
        xTaskCreate(ws_task, "cc_ws", 8192, nullptr, 4, nullptr);
    }
}

void AppCcMonitor::buildUi()
{
    lv_obj_t* scr = lv_screen_active();
    lv_obj_clean(scr);
    _halos.clear();
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Main screen: background colour, face and big number are pixels in this canvas. Tap = session list.
    _canvas = canvas(scr, pix::fb, pix::W, pix::H);
    lv_obj_set_pos(_canvas, 0, 0);
    lv_obj_add_flag(_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_canvas, [](lv_event_t*) { _tap_flag = true; }, LV_EVENT_CLICKED, nullptr);

    _big_label = outlined_text(scr, &lv_font_montserrat_16, 0x000000);   // "need you" under the big number
    lv_obj_set_width(_big_label, 116);
    lv_obj_set_style_text_align(_big_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(_big_label, 200, 96);
    _rest_label = outlined_text(scr, &lv_font_montserrat_14, 0x000000);  // "3 working\n1 ready"
    lv_obj_set_width(_rest_label, 116);
    lv_obj_set_style_text_align(_rest_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(_rest_label, 200, 120);

    _line1 = outlined_text(scr, &lv_font_montserrat_16, 0x000000);       // "<session> wants to run"
    lv_label_set_long_mode(_line1, LV_LABEL_LONG_DOT);
    _line2 = outlined_text(scr, &lv_font_montserrat_14, 0x000000);       // "npm run deploy:staging"
    lv_label_set_long_mode(_line2, LV_LABEL_LONG_DOT);

    _mute = text(scr, &lv_font_montserrat_14, 0x000000);
    lv_label_set_text(_mute, LV_SYMBOL_MUTE);
    lv_obj_set_pos(_mute, 8, 6);
    lv_obj_add_flag(_mute, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* hit = box(scr, 0x000000, LV_OPA_TRANSP);          // tap the sentence = open that session
    lv_obj_set_size(hit, 320, 62);
    lv_obj_set_pos(hit, 0, 178);
    lv_obj_add_flag(hit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(hit, [](lv_event_t*) { _open_flag = 100; }, LV_EVENT_CLICKED, nullptr);

    // Session list: starts below the screen, slides up over it.
    _sheet = box(scr, C_DARK, LV_OPA_COVER);
    lv_obj_set_size(_sheet, 320, 240);
    lv_obj_set_pos(_sheet, 0, 240);
    lv_obj_add_flag(_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_sheet, [](lv_event_t*) { _tap_flag = true; }, LV_EVENT_CLICKED, nullptr);

    _sheet_band = canvas(_sheet, _sheet_band_px, 320, 44);      // takes the mood's colour
    lv_obj_set_pos(_sheet_band, 0, 0);
    _sheet_title = outlined_text(_sheet, &lv_font_montserrat_20, 0x000000);
    lv_label_set_text(_sheet_title, "Sessions");
    lv_obj_set_pos(_sheet_title, 14, 10);
    lv_obj_t* badge = box(_sheet, 0x000000, LV_OPA_COVER);
    lv_obj_set_size(badge, 30, 22);
    lv_obj_set_pos(badge, 116, 11);
    _sheet_count = text(badge, &lv_font_montserrat_14, 0xFFFFFF);
    lv_obj_center(_sheet_count);

    lv_obj_t* quit = box(_sheet, 0x000000, LV_OPA_COVER);       // leaves the app
    lv_obj_set_size(quit, 32, 28);
    lv_obj_set_pos(quit, 278, 8);
    lv_obj_add_flag(quit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(quit, 8);
    lv_obj_t* x = text(quit, &lv_font_montserrat_14, 0xFFFFFF);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);
    lv_obj_add_event_cb(quit, [](lv_event_t*) { _quit_flag = true; }, LV_EVENT_CLICKED, nullptr);

    // Scrollable session list (swipe up/down).
    _sheet_list = box(_sheet, C_DARK, LV_OPA_TRANSP);
    lv_obj_set_pos(_sheet_list, 0, 50);
    lv_obj_set_size(_sheet_list, 320, 190);
    lv_obj_add_flag(_sheet_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(_sheet_list, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(_sheet_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(_sheet_list, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(_sheet_list, lv_color_hex(0x5A5E66), LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(_sheet_list, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(_sheet_list, 3, LV_PART_SCROLLBAR);
    lv_obj_set_flex_flow(_sheet_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(_sheet_list, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(_sheet_list, 4, 0);
    lv_obj_set_style_pad_bottom(_sheet_list, 8, 0);
    lv_obj_add_event_cb(_sheet_list, [](lv_event_t*) { _scrolled_flag = true; }, LV_EVENT_SCROLL, nullptr);

    for (int i = 0; i < 12; i++) {
        lv_obj_t* c = box(_sheet_list, C_PANEL, LV_OPA_COVER);
        lv_obj_set_size(c, 304, 34);
        notch(c, C_DARK);
        _sheet_cards[i] = c;
        _sheet_faces[i] = canvas(c, _mini_px[i], 28, 28);       // small face in the session's colour
        lv_obj_set_pos(_sheet_faces[i], 4, 3);
        _sheet_names[i] = text(c, &lv_font_montserrat_16, 0xFFFFFF);
        lv_label_set_long_mode(_sheet_names[i], LV_LABEL_LONG_DOT);
        lv_obj_align(_sheet_names[i], LV_ALIGN_LEFT_MID, 40, 0);
        _sheet_tags[i] = text(c, &lv_font_montserrat_14, C_MUTED);   // "view only": not in Ghostty
        lv_label_set_text(_sheet_tags[i], "view only");
        _sheet_details[i] = text(c, &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_width(_sheet_details[i], 96);
        lv_label_set_long_mode(_sheet_details[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(_sheet_details[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(_sheet_details[i], LV_ALIGN_RIGHT_MID, -8, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, [](lv_event_t* e) { _open_flag = 1 + (int)(intptr_t)lv_event_get_user_data(e); },
                            LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    _sheet_more = text(_sheet_list, &lv_font_montserrat_14, C_MUTED);
    build_session_screen(scr);
}

// ------------------------------------------------------------------ state -> screen

void AppCcMonitor::applyDisplay(const std::string& body)
{
    JsonDocument doc;
    if (deserializeJson(doc, body) || std::string(doc["type"] | "") != "display") {
        return;
    }
    _leds  = doc["leds"] | "off";
    _count = doc["count"] | 0;
    _more  = doc["more"] | 0;
    _quiet = doc["quiet"] | false;
    if ((doc["hush"] | false) != _hush) mclog::tagInfo(_tag, "hush (no thinking moves): {}", !_hush);
    _hush  = doc["hush"] | false;
    _rx_ms = GetHAL().millis();
    _big   = 0;
    _big_text.clear();
    if (doc["big"].is<JsonArray>()) {
        _big      = doc["big"][0] | 0;
        _big_text = doc["big"][1] | "";
    }
    _rest_text.clear();
    for (JsonVariant r : doc["rest"].as<JsonArray>()) {
        if (!_rest_text.empty()) _rest_text += "\n";
        _rest_text += r | "";
    }
    _has_spot = doc["spot"].is<JsonObject>();
    if (_has_spot) {
        _spot_bold    = doc["spot"]["bold"] | "";
        _spot_text    = doc["spot"]["text"] | "";
        _spot_sub     = doc["spot"]["sub"] | "";
        _spot_session = doc["spot"]["session"] | "";
    }
    _rows.clear();
    for (JsonObject r : doc["rows"].as<JsonArray>()) {
        Row row;
        std::string name = r["name"] | "";
        row.name         = name.empty() ? std::string(r["project"] | "session") : name;
        row.detail       = r["detail"] | "";
        row.session      = r["session"] | "";
        row.project      = r["project"] | "";
        row.host         = r["host"] | "";
        row.ask          = r["ask"] | "";
        row.say          = r["say"] | "";
        row.what         = r["what"] | "";
        row.cut          = r["cut"] | 0;
        row.state        = r["state"] | "";
        row.age_sec      = r["ageSec"] | 0;
        row.ghostty      = r["ghostty"] | false;
        row.waiting_long = r["waitingLong"] | false;
        _rows.push_back(row);
    }
    setMood(doc["mood"] | "none");
    renderMain(false);
    renderSheet();
    std::string chime = doc["chime"].is<bool>() ? (doc["chime"].as<bool>() ? "long" : "") : (doc["chime"] | "");
    if (!chime.empty()) {
        play_chime(chime == "long");
    }
}

// Mood change: set the head's resting height and queue a light nod.
void AppCcMonitor::setMood(const std::string& mood)
{
    if (mood == _mood) return;
    auto& motion  = GetStackChan().motion();
    auto is_down  = [](const std::string& m) { return m.empty() || m == "none" || m == "offline"; };
    uint32_t now  = GetHAL().millis();
    bool hold     = _sheet_open || !_open_session.empty();
    if (hold) {
        _pose_pending = true;   // someone is using the touch screen: take the new pose once they are done
        _nod_at       = 0;
    } else if (is_down(mood)) {
        motion.lookAtNormalized(0.0f, HEAD_DOWN, 200);   // nothing to show: look at the desk
        _nod_at = _nod_until = 0;
    } else if (is_down(_mood)) {
        motion.lookAtNormalized(0.0f, HEAD_UP, 300);     // lift first, then nod
        _nod_at = now + 900;
    } else {
        _nod_at = now | 1;
    }
    auto at = motion.getCurrentAngles();
    mclog::tagInfo(_tag, "mood {} -> {}: head {}, now at yaw {} pitch {}", _mood.empty() ? "-" : _mood, mood,
                   hold ? "held still (list or session screen open)" : is_down(mood) ? "down" : is_down(_mood) ? "up, then nod" : "nod",
                   at.x, at.y);
    _think_at   = now + 4000;
    _mood       = mood;
    _spot_index = 0;
}

// Labels of the main screen: number caption, the other counts, and the sentence (cycling if several).
void AppCcMonitor::renderMain(bool advance)
{
    Look look = look_of(_mood);
    bool solo = _mood == "none" || _mood == "offline";
    std::string l1, l2;
    _shown_session.clear();
    if (_mood == "offline") {
        std::lock_guard<std::mutex> lock(_rx_mutex);
        l1 = "Not connected";
        l2 = _net_msg.empty() ? "Starting Wi-Fi..." : _net_msg;
    } else {
        const char* want = (_mood == "needs_you" || _mood == "still_waiting") ? "needs_you" : _mood == "working" ? "working" : "";
        std::vector<int> pool;
        for (int i = 0; i < (int)_rows.size(); i++)
            if (_rows[i].state == want) pool.push_back(i);
        if (!pool.empty()) {
            if (advance) _spot_index++;
            _spot_index    %= (int)pool.size();
            const Row& r   = _rows[pool[_spot_index]];
            l1             = r.name + r.say;
            l2             = !r.what.empty() ? r.what : (r.cut < (int)r.ask.size() ? r.ask.substr(r.cut) : "");
            _shown_session = r.session;
        } else if (_has_spot) {
            l1             = _spot_bold + _spot_text;
            l2             = _spot_sub;
            _shown_session = _spot_session;
        }
    }
    if (_styled != _mood) {
        _styled = _mood;
        for (auto l : {_big_label, _rest_label, _line1, _line2, _mute}) lv_obj_set_style_text_color(l, lv_color_hex(look.ink), 0);
        for (auto l : {_line1, _line2}) {
            lv_obj_set_width(l, solo ? 300 : 288);
            lv_obj_set_x(l, solo ? 10 : 16);
            lv_obj_set_style_text_align(l, solo ? LV_TEXT_ALIGN_CENTER : LV_TEXT_ALIGN_LEFT, 0);
        }
        lv_obj_set_y(_line1, solo ? 186 : 184);
        lv_obj_set_y(_line2, solo ? 210 : 209);
    }
    set_text(_line1, l1);
    set_text(_line2, l2);
    set_text(_big_label, _big_text);
    set_text(_rest_label, _rest_text);
    show(_big_label, !solo && _big > 0);
    show(_rest_label, !solo && _big > 0);
    show(_mute, _quiet);
    for (auto l : {_big_label, _rest_label, _line1, _line2}) halo(l, look.rainbow);
}

void AppCcMonitor::renderSheet()
{
    if (_sheet_band_key != _mood) {
        _sheet_band_key = _mood;
        pix::Target t{_sheet_band_px, 320, 44};
        pix::band(t, _mood);
        lv_obj_invalidate(_sheet_band);
        lv_obj_set_style_text_color(_sheet_title, lv_color_hex(look_of(_mood).ink), 0);
        halo(_sheet_title, look_of(_mood).rainbow);
    }
    lv_label_set_text_fmt(_sheet_count, "%d", _count);
    for (int i = 0; i < 12; i++) {
        if (i >= (int)_rows.size()) {
            lv_obj_add_flag(_sheet_cards[i], LV_OBJ_FLAG_HIDDEN);
            continue;
        }
        const Row& r    = _rows[i];
        std::string key = key_of(r.state, r.waiting_long);
        lv_obj_remove_flag(_sheet_cards[i], LV_OBJ_FLAG_HIDDEN);
        if (_sheet_keys[i] != key) {
            _sheet_keys[i] = key;
            pix::Target t{_mini_px[i], 28, 28};
            pix::block(t, key);
            lv_obj_invalidate(_sheet_faces[i]);
        }
        // Name takes its natural width up to a limit, so "view only" can sit right after it.
        const int limit = r.ghostty ? 150 : 86;
        if (r.name != lv_label_get_text(_sheet_names[i])) {
            lv_obj_set_width(_sheet_names[i], LV_SIZE_CONTENT);
            lv_label_set_text(_sheet_names[i], r.name.c_str());
        }
        lv_obj_update_layout(_sheet_names[i]);
        int w = lv_obj_get_width(_sheet_names[i]);
        if (w > limit) {
            lv_obj_set_width(_sheet_names[i], limit);
            w = limit;
        }
        show(_sheet_tags[i], !r.ghostty);
        lv_obj_align(_sheet_tags[i], LV_ALIGN_LEFT_MID, 40 + w + 8, 0);
        const char* word = key == "needs" ? "needs you" : key == "waiting" ? "waiting" : key == "working" ? "working" : "ready";
        std::string d    = std::string(word) + "  " + fmt_age(r.age_sec + (GetHAL().millis() - _rx_ms) / 1000);
        set_text(_sheet_details[i], d);
        lv_obj_set_style_text_color(_sheet_details[i], lv_color_hex(soft_of(key)), 0);
    }
    if (_more > 0) {
        lv_label_set_text_fmt(_sheet_more, "+ %d more", _more);
    } else if (_rows.empty()) {
        lv_label_set_text(_sheet_more, _mood == "offline" ? "Not connected" : "No Claude sessions running");
    } else {
        lv_label_set_text(_sheet_more, "");
    }
}

void AppCcMonitor::showSheet(bool show)
{
    _sheet_open      = show;
    _sheet_opened_ms = GetHAL().millis();
    if (show) {
        lv_obj_scroll_to_y(_sheet_list, 0, LV_ANIM_OFF);
        lv_obj_move_foreground(_sheet);
        if (_ss) lv_obj_move_foreground(_ss);   // the session screen slides in over the list
    }
    lv_anim_delete(_sheet, anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _sheet);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(_sheet), show ? 0 : 240);
    lv_anim_set_duration(&a, show ? 300 : 240);
    lv_anim_set_path_cb(&a, show ? lv_anim_path_ease_out : lv_anim_path_ease_in);
    lv_anim_start(&a);
}

// Draw the next frame of the main screen (about 11 a second) and send only what changed to the display.
void AppCcMonitor::updateFace()
{
    uint32_t now = GetHAL().millis();
    if (now - _face_ms < 90) return;
    if (_sheet_open || !_open_session.empty()) return;   // main screen is covered
    _face_ms = now;
    _face_tick++;
    bool solo = _mood == "none" || _mood == "offline";
    pix::Target t{pix::nb, pix::W, pix::H};
    pix::frame(t, _mood, solo, _big, now, _face_tick);
    lv_area_t area;
    if (pix::diff(area)) lv_obj_invalidate_area(_canvas, &area);
}

void AppCcMonitor::updateLeds()
{
    uint32_t now = GetHAL().millis();
    if (now - _led_tick < 40) return;   // ~25 fps
    _led_tick = now;
    uint8_t r = 0, g = 0, b = 0;
    if (_leds == "flash") {
        if ((now / 250) % 2 == 0) { r = 200; }                               // red, twice a second
    } else if (_leds == "orange_flash") {
        if ((now / 250) % 2 == 0) { r = 200; g = 80; }
    } else if (_leds == "rainbow") {                                         // slow colour cycle while working
        float h = (now % 9000) / 9000.0f * 6.0f;
        float x = 1.0f - fabsf(fmodf(h, 2.0f) - 1.0f);
        float rr = 0, gg = 0, bb = 0;
        switch ((int)h) {
            case 0: rr = 1, gg = x; break;
            case 1: rr = x, gg = 1; break;
            case 2: gg = 1, bb = x; break;
            case 3: gg = x, bb = 1; break;
            case 4: rr = x, bb = 1; break;
            default: rr = 1, bb = x; break;
        }
        r = (uint8_t)(rr * 130); g = (uint8_t)(gg * 130); b = (uint8_t)(bb * 130);
    } else if (_leds == "green") {
        g = 120;
    } else if (_leds == "dim_white") {
        r = g = b = 20;
    }
    GetStackChan().leftNeonLight().setColor(r, g, b);
    GetStackChan().rightNeonLight().setColor(r, g, b);
}

// Nod on a mood change, and small "thinking" moves while working (not while recording or hushed).
void AppCcMonitor::updateHead()
{
    uint32_t now = GetHAL().millis();
    auto& motion = GetStackChan().motion();
    bool busy    = _rec || _mic_alive;   // servo noise would end up in the recording
    // The head holds still while the session list or a session screen is open: you are touching the screen.
    bool hold = _sheet_open || !_open_session.empty();
    if (hold != _was_hold) {
        _was_hold = hold;
        if (hold) {
            _nod_at = 0;
            if (!_nod_until) motion.stop();   // halt a "thinking" glide where it is (a nod finishes its return)
            mclog::tagInfo(_tag, "head: holding still");
        } else {
            if (_pose_pending) {
                bool down = _mood == "none" || _mood == "offline";
                motion.lookAtNormalized(0.0f, down ? HEAD_DOWN : HEAD_UP, 250);
                _pose_pending = false;
            }
            _think_at = now + 2000;
            mclog::tagInfo(_tag, "head: free to move again");
        }
    }
    if (hold) {
        if (_nod_until && now >= _nod_until) {
            _nod_until = 0;
            motion.lookAtNormalized(0.0f, HEAD_UP, 350);
        }
        return;
    }
    if (_nod_at && now >= _nod_at) {
        _nod_at = 0;
        if (!busy) {
            auto at = motion.getCurrentAngles();
            mclog::tagInfo(_tag, "nod: from yaw {} pitch {}", at.x, at.y);
            motion.lookAtNormalized(0.0f, HEAD_UP - 0.3f, 500);
            _nod_until = now + 320;
        }
    }
    if (_nod_until && now >= _nod_until) {
        _nod_until = 0;
        motion.lookAtNormalized(0.0f, HEAD_UP, 350);
        _think_at = now + 3000;
    }
    if (_mood == "working" && !_hush && !busy && !_nod_at && !_nod_until && now >= _think_at) {
        float x = frand(-0.2f, 0.2f), y = HEAD_UP + frand(-0.15f, 0.1f);
        auto at = motion.getCurrentAngles();
        mclog::tagInfo(_tag, "think: look at {:.2f}, {:.2f}; now at yaw {} pitch {}", x, y, at.x, at.y);
        motion.lookAtNormalized(x, y, 100 + (int)(esp_random() % 80));
        _think_at = now + 3000 + esp_random() % 4000;
    }
}

void AppCcMonitor::onRunning()
{
    std::deque<std::string> msgs;
    {
        std::lock_guard<std::mutex> lock(_rx_mutex);
        msgs.swap(_rx_queue);
    }
    LvglLockGuard lock;
    bool connected = _connected;
    uint32_t now   = GetHAL().millis();
    for (auto& m : msgs) handleMessage(m);
    if (!msgs.empty()) {
    } else if (connected != _was_connected || (!connected && now - _spot_ms > 500)) {
        if (!connected) {
            _leds = "dim_white";
            _rows.clear();
            _count = _more = _big = 0;
            setMood("offline");
        }
        renderMain(false);
        renderSheet();
        _spot_ms = now;
    }
    _was_connected = connected;
    if (connected && now - _spot_ms > 4000) {   // next session in the sentence
        renderMain(true);
        _spot_ms = now;
    }
    if (_tap_flag.exchange(false)) {
        showSheet(!_sheet_open);
    }
    if (_scrolled_flag.exchange(false)) _sheet_opened_ms = now;
    if (_sheet_open && _open_session.empty() && now - _sheet_opened_ms > 12000) {
        showSheet(false);
    }
    sessionTick();
    updateFace();
    updateLeds();
    updateHead();
    GetStackChan().update();   // pushes LED colours and servo targets to the hardware
    if (_quit_flag.exchange(false)) {
        close();
    }
}

void AppCcMonitor::onClose()
{
    mclog::tagInfo(_tag, "on close");
    _running = false;
    _rec     = false;
    GetHAL().onHeadPetGesture.disconnect(_touch_conn);
    {
        std::lock_guard<std::mutex> lock(_tx_mutex);
        if (_ws) _ws->Close();
    }
    LvglLockGuard lock;
    GetStackChan().leftNeonLight().setColor(0, 0, 0);
    GetStackChan().rightNeonLight().setColor(0, 0, 0);
    GetStackChan().motion().goHome(300);
    lv_obj_clean(lv_screen_active());
    _canvas = _sheet = nullptr;
    _ss = nullptr;
    _open_session.clear();
}

// ------------------------------------------------------------------ session screen logic

void AppCcMonitor::handleMessage(const std::string& body)
{
    if (body.find("\"type\": \"display\"") != std::string::npos || body.find("\"type\":\"display\"") != std::string::npos) {
        applyDisplay(body);
        if (!_open_session.empty()) renderSession();
        return;
    }
    JsonDocument doc;
    if (deserializeJson(doc, body)) return;
    std::string type = doc["type"] | "";
    if (type == "transcript") {
        _transcribing = false;
        std::string t = doc["text"] | "";
        std::string err = doc["error"] | "";
        if (_open_session.empty()) return;
        if (!err.empty() || t.empty()) {
            toast(err.empty() ? "Didn't catch that" : "Transcription failed", 0xFF9A82);
        } else {
            _pending_text = t;
            lv_label_set_text(_ss_confirm_text, t.c_str());
            show(_ss_confirm, true);
            fade_in(_ss_confirm, 200);
        }
        renderSession();
    } else if (type == "result") {
        bool ok            = doc["ok"] | false;
        std::string action = doc["action"] | "";
        std::string err    = doc["error"] | "failed";
        if (ok) {
            toast(action == "approve" ? LV_SYMBOL_OK "  Approved" : action == "reject" ? "Rejected" : LV_SYMBOL_OK "  Sent", 0x86E79D);
        } else {
            toast(err, 0xFF9A82);
        }
    }
}

void AppCcMonitor::openSession(int row)
{
    if (row < 0 || row >= (int)_rows.size()) return;
    _open_session = _rows[row].session;
    show(_ss_confirm, false);
    renderSession();
    lv_obj_move_foreground(_ss);
    slide_x(_ss, 0);
    if (_sheet_open) showSheet(false);
}

void AppCcMonitor::closeSession()
{
    _open_session.clear();
    _rec       = false;
    _talk_down = false;
    show(_ss_confirm, false);
    slide_x(_ss, 320);
}

void AppCcMonitor::renderSession()
{
    const Row* r = nullptr;
    for (auto& x : _rows) if (x.session == _open_session) r = &x;
    std::string key = r ? key_of(r->state, r->waiting_long) : "offline";
    if (key != _ss_band_key) {
        _ss_band_key = key;
        pix::Target t{_ss_band_px, 320, 54};
        pix::band(t, key);
        if (r) pix::mini(t, key, 270, 12, 3);
        lv_obj_invalidate(_ss_band);
        uint32_t ink = look_of(key).ink;
        lv_obj_set_style_text_color(_ss_title, lv_color_hex(ink), 0);
        lv_obj_set_style_text_color(_ss_sub, lv_color_hex(ink), 0);
    }
    if (!r) {
        lv_label_set_text(_ss_status, "Session ended");
        lv_obj_set_style_text_color(_ss_status, lv_color_hex(C_MUTED), 0);
        for (auto o : {_ss_reject, _ss_approve, _ss_talk, _ss_focus, _ss_ask_box}) show(o, false);
        lv_label_set_text(_ss_note, "");
        halo(_ss_title, false);
        halo(_ss_sub, false);
        return;
    }
    lv_label_set_text(_ss_title, r->name.c_str());
    std::string sub = r->project + "  on  " + r->host;
    lv_label_set_text(_ss_sub, sub.c_str());
    halo(_ss_title, look_of(key).rainbow);
    halo(_ss_sub, look_of(key).rainbow);
    std::string st = (r->waiting_long ? "still waiting" : r->detail) + ",  " + fmt_age(r->age_sec + (GetHAL().millis() - _rx_ms) / 1000);
    lv_label_set_text(_ss_status, st.c_str());
    lv_obj_set_style_text_color(_ss_status, lv_color_hex(soft_of(key)), 0);

    bool waiting = r->state == "needs_you";
    std::string ask = !r->ask.empty() ? r->ask
                      : waiting           ? "Waiting for you in the terminal."
                      : r->state == "working" ? "Claude is working..."
                                              : "Ready for your next prompt.";
    lv_label_set_text(_ss_ask, ask.c_str());
    show(_ss_ask_box, true);
    bool actions = waiting && r->ghostty;
    lv_obj_set_height(_ss_ask_box, actions ? 64 : 108);
    lv_obj_set_height(_ss_ask, actions ? 50 : 94);
    show(_ss_reject, actions);
    show(_ss_approve, actions);
    show(_ss_talk, r->ghostty);
    show(_ss_focus, r->ghostty);
    lv_label_set_text(_ss_note, r->ghostty ? "" : "Not in Ghostty: view only");
    if (_rec) {
        lv_label_set_text(_ss_talk_label, LV_SYMBOL_AUDIO "  Listening... release");
    } else if (_transcribing) {
        lv_label_set_text(_ss_talk_label, "Transcribing...");
    } else {
        lv_label_set_text(_ss_talk_label, LV_SYMBOL_AUDIO "  Hold to talk");
    }
}

void AppCcMonitor::sessionTick()
{
    uint32_t now = GetHAL().millis();
    if (_toast_until && now > _toast_until) {
        _toast_until = 0;
        show(_ss_toast, false);
    }
    int open = _open_flag.exchange(0);
    if (open == 100) {   // tapped the main-screen sentence: open that session, or the list if it isn't about one
        int idx = -1;
        for (int i = 0; i < (int)_rows.size(); i++)
            if (!_shown_session.empty() && _rows[i].session == _shown_session) idx = i;
        if (idx >= 0) openSession(idx);
        else _tap_flag = true;
    } else if (open > 0) {
        openSession(open - 1);
    }
    // Never wait forever for a transcript (server restarted, connection dropped, STT down).
    if (_transcribing && (!_connected || now - _transcribe_ms > 25000)) {
        _transcribing = false;
        mclog::tagWarn(_tag, "talk: no transcript ({})", _connected ? "timeout" : "disconnected");
        if (!_open_session.empty()) {
            toast(_connected ? "No reply, try again" : "Connection lost, try again", 0xFF9A82);
            renderSession();
        }
    }
    if (_open_session.empty()) return;
    if (_back_flag.exchange(false)) {
        closeSession();
        return;
    }
    if (_reject_flag.exchange(false)) {
        send_json(R"({"type":"act","action":"reject","session":)" + jstr(_open_session) + "}");
    }
    if (_focus_flag.exchange(false)) {
        send_json(R"({"type":"focus","session":)" + jstr(_open_session) + "}");
        toast("Opened on Mac", C_TEXT);
    }
    if (_cancel_flag.exchange(false)) show(_ss_confirm, false);
    if (_send_flag.exchange(false)) {
        show(_ss_confirm, false);
        send_json(R"({"type":"send","session":)" + jstr(_open_session) + R"(,"text":)" + jstr(_pending_text) + "}");
    }
    // Hold-to-approve: the fill grows over 1 s; letting go early cancels.
    uint32_t down = _approve_down;
    if (down) {
        uint32_t held = now - down;
        lv_obj_set_width(_ss_approve_fill, std::min<uint32_t>(146, held * 146 / 1000));
        if (held >= 1000) {
            _approve_down = 0;
            lv_obj_set_width(_ss_approve_fill, 0);
            send_json(R"({"type":"act","action":"approve","session":)" + jstr(_open_session) + "}");
        }
    } else {
        lv_obj_set_width(_ss_approve_fill, 0);
    }
    // Push-to-talk: record while held.
    if (_talk_down && !_connected && !_rec) {
        _talk_down = false;
        toast("Not connected", 0xFF9A82);
    } else if (_talk_down && !_rec && !_mic_alive && !_transcribing) {
        send_json(R"({"type":"ptt_start","session":)" + jstr(_open_session) + R"(,"rate":)" +
                  std::to_string(Board::GetInstance().GetAudioCodec()->input_sample_rate()) + "}");
        _rec = true;
        xTaskCreate(mic_task, "cc_mic", 6144, nullptr, 5, nullptr);
        renderSession();
    } else if (!_talk_down && _rec) {
        mclog::tagInfo(_tag, "talk: button released");
        _rec           = false;
        _transcribing  = true;
        _transcribe_ms = now;
        renderSession();
    }
}
