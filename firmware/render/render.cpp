/*
 * Draws the robot's faces on a Mac, from the same code the robot uses (../app_cc_monitor/pix.h), and saves
 * them as images for the server's browser preview:
 *   <out>/<mood>.png   the main screen for each mood, animated (APNG, 10 frames a second, looping)
 *   <out>/row-<state>.png   the small face block of a session list row
 * Build and run with render.sh.
 */
#include "../app_cc_monitor/pix.h"
#include <zlib.h>
#include <cstdio>
#include <string>
#include <vector>

namespace {

struct Png {
    std::vector<uint8_t> out;
    uint32_t seq = 0;   // APNG sequence number, shared by fcTL and fdAT chunks

    void u32(std::vector<uint8_t>& v, uint32_t x)
    {
        for (int s = 24; s >= 0; s -= 8) v.push_back((uint8_t)(x >> s));
    }
    void chunk(const char* type, const std::vector<uint8_t>& data)
    {
        u32(out, (uint32_t)data.size());
        size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        uint32_t crc = crc32(0, out.data() + start, (uInt)(out.size() - start));
        u32(out, crc);
    }
};

// RGB565 pixels of the rectangle (x, y, w, h) as zlib-compressed PNG rows, each with the "Up" filter.
std::vector<uint8_t> compress(const uint16_t* px, int stride, int x, int y, int w, int h)
{
    std::vector<uint8_t> raw, prev(w * 3, 0), row(w * 3);
    raw.reserve((size_t)h * (w * 3 + 1));
    for (int r = 0; r < h; r++) {
        const uint16_t* p = px + (y + r) * stride + x;
        for (int i = 0; i < w; i++) {
            uint16_t c     = p[i];
            row[i * 3]     = (uint8_t)(((c >> 11) & 31) * 255 / 31);
            row[i * 3 + 1] = (uint8_t)(((c >> 5) & 63) * 255 / 63);
            row[i * 3 + 2] = (uint8_t)((c & 31) * 255 / 31);
        }
        raw.push_back(2);   // filter: Up
        for (int i = 0; i < w * 3; i++) raw.push_back((uint8_t)(row[i] - prev[i]));
        prev = row;
    }
    uLongf n = compressBound((uLong)raw.size());
    std::vector<uint8_t> z(n);
    compress2(z.data(), &n, raw.data(), (uLong)raw.size(), 9);
    z.resize(n);
    return z;
}

std::vector<uint8_t> header(Png& png, int w, int h)
{
    static const uint8_t SIG[8] = {137, 'P', 'N', 'G', '\r', '\n', 26, '\n'};
    png.out.assign(SIG, SIG + 8);
    std::vector<uint8_t> ihdr;
    png.u32(ihdr, (uint32_t)w);
    png.u32(ihdr, (uint32_t)h);
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});   // 8-bit RGB
    return ihdr;
}

bool save(const std::string& path, const std::vector<uint8_t>& bytes)
{
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    bool ok = fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    return fclose(f) == 0 && ok;
}

// The main screen for `mood`, `seconds` long at 10 frames a second. After the first frame only the part
// that changed is stored, like the robot only sends changed parts to its display.
bool animate(const std::string& path, const std::string& mood, float seconds)
{
    const int W = pix::W, H = pix::H, frames = (int)(seconds * 10 + 0.5f);
    const bool solo = mood == "none" || mood == "offline" || mood == "dizzy";
    std::vector<uint16_t> cur(W * H), last(W * H);
    Png png;
    std::vector<uint8_t> ihdr = header(png, W, H);
    png.chunk("IHDR", ihdr);
    std::vector<uint8_t> actl;
    png.u32(actl, (uint32_t)frames);
    png.u32(actl, 0);   // loop forever
    png.chunk("acTL", actl);
    for (int f = 0; f < frames; f++) {
        pix::Target t{cur.data(), W, H};
        pix::frame(t, mood, solo, 0, (uint32_t)f * 100, (uint32_t)f);
        int x0 = 0, y0 = 0, x1 = W - 1, y1 = H - 1;
        if (f > 0) {   // bounding box of what changed (a 1x1 corner when nothing did)
            x0 = W, y0 = H, x1 = -1, y1 = -1;
            for (int y = 0; y < H; y++)
                for (int x = 0; x < W; x++)
                    if (cur[y * W + x] != last[y * W + x]) {
                        x0 = std::min(x0, x), x1 = std::max(x1, x), y0 = std::min(y0, y), y1 = std::max(y1, y);
                    }
            if (x1 < 0) x0 = y0 = x1 = y1 = 0;
        }
        int w = x1 - x0 + 1, h = y1 - y0 + 1;
        std::vector<uint8_t> fctl;
        png.u32(fctl, png.seq++);
        png.u32(fctl, (uint32_t)w);
        png.u32(fctl, (uint32_t)h);
        png.u32(fctl, (uint32_t)x0);
        png.u32(fctl, (uint32_t)y0);
        fctl.insert(fctl.end(), {0, 1, 0, 10, 0, 0});   // 1/10 s; keep the previous frame; replace the area
        png.chunk("fcTL", fctl);
        std::vector<uint8_t> z = compress(cur.data(), W, x0, y0, w, h);
        if (f == 0) {
            png.chunk("IDAT", z);
        } else {
            std::vector<uint8_t> fdat;
            png.u32(fdat, png.seq++);
            fdat.insert(fdat.end(), z.begin(), z.end());
            png.chunk("fdAT", fdat);
        }
        last = cur;
    }
    png.chunk("IEND", {});
    return save(path, png.out);
}

bool still(const std::string& path, const std::string& key)   // 28x28 list-row face block
{
    std::vector<uint16_t> px(28 * 28);
    pix::Target t{px.data(), 28, 28};
    pix::block(t, key);
    Png png;
    png.chunk("IHDR", header(png, 28, 28));
    png.chunk("IDAT", compress(px.data(), 28, 0, 0, 28, 28));
    png.chunk("IEND", {});
    return save(path, png.out);
}

}   // namespace

int main(int argc, char** argv)
{
    if (argc != 2) {
        fprintf(stderr, "usage: render <output dir>\n");
        return 2;
    }
    const std::string dir = argv[1];
    // Each mood loops after a length that suits its animation (a whole number of blinks, drips, ...).
    static const struct { const char* mood; float seconds; } MOODS[] = {
        {"working", 6.8f}, {"needs_you", 4.2f}, {"still_waiting", 3.6f}, {"just_finished", 3.2f},
        {"all_done", 3.2f}, {"none", 3.6f}, {"offline", 4.2f}, {"dizzy", 2.4f},
    };
    bool ok = true;
    for (auto& m : MOODS) ok &= animate(dir + "/" + m.mood + ".png", m.mood, m.seconds);
    for (const char* key : {"needs", "waiting", "working", "ready"}) ok &= still(dir + "/row-" + key + ".png", key);
    if (!ok) fprintf(stderr, "render: could not write to %s\n", dir.c_str());
    return ok ? 0 : 1;
}
