/*
 * Colours and pixel drawing for the CC Monitor app: backgrounds, the face for each mood, the big number and
 * the small session faces, drawn into plain RGB565 buffers.
 *
 * No LVGL or ESP-IDF here, so the same code also builds on a Mac: firmware/render draws the faces from it
 * for the server's browser preview. Change a face here and both follow.
 */
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>

struct Look {
    uint32_t bg, ink;
    bool rainbow;        // drifting rainbow background; text gets a black outline
    bool grid = false;   // dark screen with a thin green grid and border; everything drawn in one green
};

// The working screen: true = green grid on a dark screen (trial), false = drifting rainbow.
inline constexpr bool WORKING_GRID = true;

// Mood ("needs_you", "still_waiting", ...) or session key ("needs", "waiting", "working", "ready") -> colours.
inline Look look_of(const std::string& k)
{
    if (k == "needs_you" || k == "needs") return {0xE8380D, 0x190400, false};
    if (k == "still_waiting" || k == "waiting") return {0xF28A0C, 0x1A0A00, false};
    if (k == "working") return WORKING_GRID ? Look{0x2F3B0E, 0x5CF53A, false, true} : Look{0x000000, 0xFFFFFF, true};
    if (k == "just_finished") return {0xF2D40A, 0x1A1500, false};
    if (k == "all_done" || k == "ready") return {0x2F9BFF, 0x031A33, false};   // blue
    if (k == "none") return {0x18244A, 0x8FA6E8, false};
    if (k == "dizzy") return {0xF28A0C, 0x1A0A00, false};   // orange; shown for a few seconds after a shake
    return {0x4E5154, 0xC9CCCF, false};   // offline
}

inline std::string key_of(const std::string& state, bool waiting_long)
{
    if (state == "needs_you") return waiting_long ? "waiting" : "needs";
    return state == "working" ? "working" : "ready";
}

inline uint32_t soft_of(const std::string& key)   // readable text colour for a session's state on the dark body
{
    if (key == "needs") return 0xFF9A82;
    if (key == "waiting") return 0xFFC27A;
    if (key == "working") return 0xC9B8FF;
    return 0x8CC8FF;   // ready: light blue
}

// ------------------------------------------------------------------ pixel drawing (into RGB565 buffers)

namespace pix {

inline constexpr int W = 320, H = 240;
inline int G = 4;   // size of one "big pixel" of the face (doubled for the dizzy face, which has the screen to itself)

struct Target {
    uint16_t* buf;
    int w, h;
};

inline uint32_t shade(uint32_t hex, float k)
{
    uint32_t r = (uint32_t)(((hex >> 16) & 0xFF) * k), g = (uint32_t)(((hex >> 8) & 0xFF) * k), b = (uint32_t)((hex & 0xFF) * k);
    return (r << 16) | (g << 8) | b;
}

inline uint16_t rgb(uint32_t hex)
{
    // RGB565, the same as lv_color_to_u16(lv_color_hex(hex))
    return (uint16_t)(((hex >> 8) & 0xF800) | ((hex >> 5) & 0x07E0) | ((hex >> 3) & 0x001F));
}

inline void fill(Target& t, int x, int y, int w, int h, uint16_t c)
{
    int x0 = std::max(0, x), y0 = std::max(0, y), x1 = std::min(t.w, x + w), y1 = std::min(t.h, y + h);
    for (int yy = y0; yy < y1; yy++) {
        uint16_t* p = t.buf + yy * t.w;
        for (int xx = x0; xx < x1; xx++) p[xx] = c;
    }
}

inline void cell(Target& t, float gx, float gy, int w, int h, uint16_t c)
{
    fill(t, (int)lroundf(gx) * G, (int)lroundf(gy) * G, w * G, h * G, c);
}

inline void ell(Target& t, float cx, float cy, float rx, float ry, uint16_t c)
{
    for (int y = (int)floorf(cy - ry); y <= (int)ceilf(cy + ry); y++)
        for (int x = (int)floorf(cx - rx); x <= (int)ceilf(cx + rx); x++) {
            float dx = (x + 0.5f - cx) / rx, dy = (y + 0.5f - cy) / ry;
            if (dx * dx + dy * dy <= 1.0f) fill(t, x * G, y * G, G, G, c);
        }
}

// Solid colour with fine vertical screen lines (every other pair of columns slightly darker).
inline void bg_solid(Target& t, uint32_t hex)
{
    uint16_t a = rgb(hex), b = rgb(shade(hex, 0.90f));
    for (int x = 0; x < t.w; x++) t.buf[x] = (x & 3) >= 2 ? b : a;
    for (int y = 1; y < t.h; y++) memcpy(t.buf + y * t.w, t.buf, t.w * 2);
}

// Diagonal rainbow bands, with the same fine screen lines as bg_solid. `step` drifts the bands.
inline uint16_t RB[2][12];
inline void bg_rainbow(Target& t, int step)
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
inline void bg_grid(Target& t, uint32_t hex, int spacing, int border)
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

inline void band(Target& t, const std::string& key)   // header band of the list / session screen
{
    Look l = look_of(key);
    if (l.rainbow) bg_rainbow(t, 0);
    else if (l.grid) bg_grid(t, l.bg, 16, 2);
    else bg_solid(t, l.bg);
}

inline const char* HEART[8] = {".XX...XX.", "XXXX.XXXX", "XXXXXXXXX", "XXXXXXXXX",
                               ".XXXXXXX.", "..XXXXX..", "...XXX...", "....X...."};

// The face for one mood at time `s` seconds, centred near cell (fx, fy).
inline void face(Target& t, const std::string& mood, float s, int fx, int fy, uint32_t ink_hex)
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
    } else if (mood == "dizzy") {   // spinning spiral eyes, a travelling wobble for a mouth, stars circling
        static const uint8_t SPIRAL[7] = {0b1111111, 0b0000001, 0b1111101, 0b1000101, 0b1011101, 0b1000001, 0b1111111};
        int turn = ((int)(s / 0.12f)) % 4;
        for (int e = -1; e <= 1; e += 2)
            for (int y = 0; y < 7; y++)
                for (int x = 0; x < 7; x++) {
                    int sx = x, sy = y;
                    for (int r = 0; r < (e < 0 ? turn : 3 - turn); r++) {   // quarter turns; the eyes spin opposite ways
                        int old = sx;
                        sx      = sy;
                        sy      = 6 - old;
                    }
                    if (SPIRAL[sy] & (0b1000000 >> sx)) cell(t, fx + e * 11 - 3 + x, fy - 6 + y, 1, 1, K);
                }
        static const int wave[4] = {0, -1, 0, 1};
        int phase = (int)(s / 0.15f);
        for (int x = -8; x <= 8; x++) cell(t, fx + x, fy + 9 + wave[((x + phase) % 4 + 4) % 4], 1, 1, K);
        const uint16_t STAR = rgb(0xFFFFFF);   // white: yellow would be lost on the orange
        for (int i = 0; i < 3; i++) {
            float a = s * 4.0f + i * 2.094f;
            int x = fx + (int)lroundf(cosf(a) * 17), y = fy - 12 + (int)lroundf(sinf(a) * 3);
            cell(t, x, y - 1, 1, 3, STAR);
            cell(t, x - 1, y, 3, 1, STAR);
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
inline constexpr uint8_t DIGITS[10][7] = {
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

inline void number(Target& t, int n, int cx, int y, uint16_t c, bool outline)
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
inline void frame(Target& t, const std::string& mood, bool solo, int big, uint32_t ms, uint32_t tick)
{
    Look l  = look_of(mood);
    float s = (ms % 600000) / 1000.0f;
    if (l.rainbow) bg_rainbow(t, (int)tick);
    else if (l.grid) bg_grid(t, l.bg, 32, 3);
    else bg_solid(t, l.bg);
    if (mood == "dizzy") {   // nothing else on screen: draw it twice the size, centred
        G = 8;
        face(t, mood, s, 20, 16, l.ink);
        G = 4;
    } else {
        face(t, mood, s, solo ? 40 : 26, 20, l.ink);
    }
    if (!solo) {
        if (big > 0) number(t, big, 258, 34, rgb(l.ink), l.rainbow);
        // Line above the sentence. On the rainbow: white with a thin black edge, like the text.
        if (l.rainbow) fill(t, 11, 176, 298, 4, rgb(0x000000));
        fill(t, 12, 177, 296, 2, rgb(l.rainbow ? 0xFFFFFF : l.grid ? l.ink : shade(l.bg, 0.62f)));
    }
}

// Small face for a session's state, on a 12x10 grid of g-sized cells.
inline void mini(Target& t, const std::string& key, int x, int y, int g)
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
inline void block(Target& t, const std::string& key)
{
    Look l = look_of(key);
    if (l.rainbow) bg_rainbow(t, 0);
    else if (l.grid) bg_grid(t, l.bg, 0, 2);
    else bg_solid(t, l.bg);
    mini(t, key, 2, 4, 2);
}

}   // namespace pix
