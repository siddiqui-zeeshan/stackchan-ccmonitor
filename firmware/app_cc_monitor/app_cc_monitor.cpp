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
 * just_finished (yellow smiley + heart), all_done (blue smiley), none (asleep), offline (grey).
 * Head: tilted up in every mood except none/offline (down); a light nod when the mood changes; small
 * random "thinking" moves while working. Head panel: tap = acknowledge, long press = quiet mode.
 * Shake the robot: a dizzy face for 5 seconds (orange screen, spiral eyes), then back to the mood.
 *
 * The face and backgrounds are drawn as pixels into an LVGL canvas (no stock avatar is attached); the
 * drawing code is in pix.h.
 */
#include "app_cc_monitor.h"
#include "pix.h"          // colours and pixel drawing, shared with firmware/render
#include "cc_monitor_config.h"   // CC_MONITOR_URL, CC_MONITOR_TOKEN, or CC_MONITOR_BLE (written by build.sh)
#ifdef CC_MONITOR_BLE
#include "cc_ble.h"   // Bluetooth build: talks to cc-ble on the paired Mac instead of the server
static constexpr bool DISPLAY_ONLY = true;   // no Approve / Reject / Talk / focus, whatever the Mac sends
#else
static constexpr bool DISPLAY_ONLY = false;
#endif
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

static std::string fmt_age(int s)
{
    if (s < 60) return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m";
    return std::to_string(s / 3600) + "h";
}

// ------------------------------------------------------------------ main canvas buffers (drawing is in pix.h)

namespace pix {

static uint16_t* fb = nullptr;   // what the main canvas shows
static uint16_t* nb = nullptr;   // next frame, compared against fb so only changed parts are sent to the screen

static uint16_t* alloc(int w, int h)
{
    return (uint16_t*)heap_caps_malloc(w * h * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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

}   // namespace pix

// ------------------------------------------------------------------ connection (own task)

static std::mutex _rx_mutex;
static std::deque<std::string> _rx_queue;   // server messages, applied on the UI loop
static std::string _net_title = "Not connected";   // offline screen, first line
static std::string _net_msg;             // Wi-Fi / connection progress text
static std::atomic<bool> _running{false};
static std::atomic<bool> _connected{false};
static std::atomic<bool> _task_alive{false};
static std::mutex _tx_mutex;
static std::unique_ptr<WebSocket> _ws;

#ifndef CC_MONITOR_BLE
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
#endif

static void send_json(const std::string& s)
{
    std::lock_guard<std::mutex> lock(_tx_mutex);
#ifdef CC_MONITOR_BLE
    if (s.rfind(R"({"type":"touch")", 0) != 0) return;   // display only: head-panel touches are all it sends
    bool ok = cc_ble::send(s);
    mclog::tagInfo(_tag, "sent {}: {}", s, ok ? "ok" : "not sent (Mac not connected)");
#else
    if (_ws && _ws->IsConnected()) _ws->Send(s);
#endif
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
    send_json(std::string(R"({"type":"touch","gesture":")") + gesture + R"(","row":0})");
}

#ifdef CC_MONITOR_BLE
// Bluetooth build: what the offline screen says while the paired Mac isn't connected.
static std::atomic<int> _ble_state{(int)cc_ble::State::NotPaired};
static std::atomic<uint32_t> _passkey{0};

static void ble_texts()
{
    auto st = (cc_ble::State)_ble_state.load();
    uint32_t code = _passkey;
    std::lock_guard<std::mutex> lock(_rx_mutex);
    if (code) {
        char buf[24];
        snprintf(buf, sizeof(buf), "Code: %03u %03u", (unsigned)(code / 1000), (unsigned)(code % 1000));
        _net_title = buf, _net_msg = "Type it on your Mac";
    } else if (st == cc_ble::State::NotPaired) {
        _net_title = "Not paired", _net_msg = "Plug into your Mac by USB, then restart me";
    } else if (st == cc_ble::State::Pairing) {
        _net_title = "Ready to pair", _net_msg = "Run ./setup.sh --pair on your Mac";
    } else {
        _net_title = "Waiting for your Mac", _net_msg = "Is it on and nearby?";
    }
    _connected = st == cc_ble::State::Connected;
}

static void ble_start()
{
    cc_ble::Events ev;
    ev.on_message = [](std::string m) {
        std::lock_guard<std::mutex> lock(_rx_mutex);
        if (_rx_queue.size() >= 8) _rx_queue.pop_front();
        _rx_queue.push_back(std::move(m));
    };
    ev.on_passkey = [](uint32_t code) {
        _passkey = code;
        ble_texts();
    };
    ev.on_state = [](cc_ble::State s) {
        _ble_state = (int)s;
        ble_texts();
    };
    ble_texts();
    cc_ble::start(std::move(ev));
}
#endif

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

// One line of text that ends in "..." when it is too long. The height has to be fixed as well as the width:
// a label with a free height wraps onto more lines instead (and then sits on top of whatever is below it).
static void one_line(lv_obj_t* label, const lv_font_t* font)
{
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_obj_set_height(label, lv_font_get_line_height(font));
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
            lv_obj_set_height(c, lv_obj_get_style_height(label, LV_PART_MAIN));
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
    one_line(_ss_title, &lv_font_montserrat_20);
    lv_obj_set_pos(_ss_title, 52, 7);
    _ss_sub = outlined_text(_ss, &lv_font_montserrat_14, 0x000000);
    lv_obj_set_width(_ss_sub, 206);
    one_line(_ss_sub, &lv_font_montserrat_14);
    lv_obj_set_pos(_ss_sub, 52, 32);

    _ss_status = text(_ss, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_width(_ss_status, 292);
    one_line(_ss_status, &lv_font_montserrat_16);
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
static std::atomic<bool> _stroked{false};   // the finger moved along the panel since it went down
static size_t _shake_conn = 0;
static std::atomic<bool> _shake_flag{false};      // the motion sensor saw a shake
static const uint32_t DIZZY_MS = 5000;
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
    _dizzy_until = 0;
    _was_dizzy   = false;
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
            _stroked  = false;
        } else if (g == HeadPetGesture::Release) {
            // The firmware reports Release after a stroke too. Only a finger that stayed still is a tap or a
            // long press; otherwise petting the head for 1.5 s would toggle quiet mode.
            if (_stroked.exchange(false)) return;
            uint32_t held = now - _press_ms.load();
            if (held >= 1500) {
                send_touch("long_press");
            } else if (held < 600) {
                send_touch("tap");
            }
        } else if (g == HeadPetGesture::SwipeForward || g == HeadPetGesture::SwipeBackward) {
            _stroked = true;
            send_touch("stroke");
        }
    });

    _shake_flag = false;
    _shake_conn = GetHAL().onImuMotionEvent.connect([](ImuMotionEvent e) {
        if (e == ImuMotionEvent::Shake) _shake_flag = true;
    });

    _running = true;
#ifdef CC_MONITOR_BLE
    ble_start();   // once; Bluetooth stays up while the app is closed
#else
    if (!_task_alive) {
        xTaskCreate(ws_task, "cc_ws", 8192, nullptr, 4, nullptr);
    }
#endif
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
    one_line(_line1, &lv_font_montserrat_16);
    _line2 = outlined_text(scr, &lv_font_montserrat_14, 0x000000);       // "npm run deploy:staging"
    one_line(_line2, &lv_font_montserrat_14);

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
        one_line(_sheet_names[i], &lv_font_montserrat_16);
        lv_obj_align(_sheet_names[i], LV_ALIGN_LEFT_MID, 40, 0);
        _sheet_tags[i] = text(c, &lv_font_montserrat_14, C_MUTED);   // "view only": not in Ghostty
        lv_label_set_text(_sheet_tags[i], "view only");
        _sheet_details[i] = text(c, &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_width(_sheet_details[i], 96);
        one_line(_sheet_details[i], &lv_font_montserrat_14);
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
        row.ghostty      = !DISPLAY_ONLY && (r["ghostty"] | false);
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

bool AppCcMonitor::isDizzy()
{
    return _dizzy_until && GetHAL().millis() < _dizzy_until;
}

// Labels of the main screen: number caption, the other counts, and the sentence (cycling if several).
void AppCcMonitor::renderMain(bool advance)
{
    if (isDizzy()) {   // just the face; restyle for the real mood when it ends
        _styled = "dizzy";
        _shown_session.clear();
        set_text(_line1, "");
        set_text(_line2, "");
        for (auto l : {_big_label, _rest_label, _mute}) show(l, false);
        for (auto l : {_big_label, _rest_label, _line1, _line2}) halo(l, false);
        return;
    }
    Look look = look_of(_mood);
    bool solo = _mood == "none" || _mood == "offline";
    std::string l1, l2;
    _shown_session.clear();
    if (_mood == "offline") {
        std::lock_guard<std::mutex> lock(_rx_mutex);
        l1 = _net_title;
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
        const int limit = r.ghostty || DISPLAY_ONLY ? 150 : 86;
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
        show(_sheet_tags[i], !r.ghostty && !DISPLAY_ONLY);
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
    bool dizzy = isDizzy();
    bool solo  = dizzy || _mood == "none" || _mood == "offline";
    pix::Target t{pix::nb, pix::W, pix::H};
    pix::frame(t, dizzy ? "dizzy" : _mood, solo, _big, now, _face_tick);
    lv_area_t area;
    if (pix::diff(area)) lv_obj_invalidate_area(_canvas, &area);
}

void AppCcMonitor::updateLeds()
{
    uint32_t now = GetHAL().millis();
    if (now - _led_tick < 40) return;   // ~25 fps
    _led_tick = now;
    if (isDizzy()) {   // orange, swapping sides
        bool left = (now / 160) % 2 == 0;
        GetStackChan().leftNeonLight().setColor(left ? 200 : 0, left ? 80 : 0, 0);
        GetStackChan().rightNeonLight().setColor(left ? 0 : 200, left ? 0 : 80, 0);
        return;
    }
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
    bool busy    = _rec || _mic_alive || isDizzy();   // recording (servo noise), or just shaken (in someone's hand)
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
    if (_shake_flag.exchange(false)) {
        _dizzy_until = now + DIZZY_MS;
        mclog::tagInfo(_tag, "shaken: dizzy for {} ms", DIZZY_MS);
    }
    if (isDizzy() != _was_dizzy) {   // dizzy started or wore off: hide or bring back the number and sentence
        _was_dizzy = !_was_dizzy;
        renderMain(false);
    }
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
    GetHAL().onImuMotionEvent.disconnect(_shake_conn);
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
    lv_label_set_text(_ss_note, r->ghostty || DISPLAY_ONLY ? "" : "Not in Ghostty: view only");
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
