/*
 * CC Monitor: shows live Claude Code session state from cc-monitor.
 *
 * The robot only draws what the server sends: {"type":"display","mood","leds","chime","quiet",
 * "rows":[{name,detail,state,ageSec,ghostty}],"more","count","counts":{needs_you,working,ready}}.
 *
 * Screen: the animated StackChan face (emotion follows the mood), a status chip top-left, a card at the
 * bottom that cycles through sessions (pinned to the ones that need you), and a session list that slides
 * up when the screen is tapped. Head panel: tap = acknowledge, long press = quiet mode, stroke = page.
 */
#include "app_cc_monitor.h"
#include "cc_monitor_config.h"   // CC_MONITOR_URL, CC_MONITOR_TOKEN (git-ignored, see the .example)
#include <hal/hal.h>
#include <hal/board/hal_bridge.h>
#include <assets/assets.h>
#include <stackchan/stackchan.h>
#include <stackchan/avatar/skins/default/default.h>
#include <stackchan/avatar/decorators/decorators.h>
#include <mooncake_log.h>
#include <ArduinoJson.hpp>
#include <board.h>
#include <web_socket.h>
#include <audio_codec.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <deque>

using namespace mooncake;
using namespace stackchan;
using namespace ArduinoJson;

static const char* _tag = "CC-MONITOR";

// ------------------------------------------------------------------ palette

static const uint32_t C_BG      = 0x000000;
static const uint32_t C_SURFACE = 0x1C1C1E;
static const uint32_t C_SHEET   = 0x111113;
static const uint32_t C_TEXT    = 0xF5F5F7;
static const uint32_t C_MUTED   = 0x8E8E93;
static const uint32_t C_NEEDS   = 0xFF9F0A;
static const uint32_t C_WORKING = 0xFFD60A;
static const uint32_t C_READY   = 0x30D158;
static const uint32_t C_OFFLINE = 0x636366;

static uint32_t state_color(const std::string& s)
{
    if (s == "needs_you") return C_NEEDS;
    if (s == "working") return C_WORKING;
    if (s == "ready") return C_READY;
    return C_OFFLINE;
}

static std::string fmt_age(int s)
{
    if (s < 60) return std::to_string(s) + "s";
    if (s < 3600) return std::to_string(s / 60) + "m";
    return std::to_string(s / 3600) + "h";
}

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
            ws->SetReceiveBufferSize(4096);
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

static lv_obj_t* box(lv_obj_t* parent, uint32_t color, lv_opa_t opa, int radius)
{
    lv_obj_t* o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_set_style_bg_color(o, lv_color_hex(color), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    return o;
}

static lv_obj_t* text(lv_obj_t* parent, const lv_font_t* font, uint32_t color)
{
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, "");
    return l;
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

static void pulse(lv_obj_t* o, uint32_t period_ms)
{
    lv_anim_delete(o, anim_opa_cb);
    lv_obj_set_style_opa(o, 255, 0);
    if (!period_ms) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, anim_opa_cb);
    lv_anim_set_values(&a, 255, 70);
    lv_anim_set_duration(&a, period_ms / 2);
    lv_anim_set_playback_duration(&a, period_ms / 2);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

// ------------------------------------------------------------------ session screen + push-to-talk

static lv_obj_t* _ss = nullptr;            // session screen (slides in from the right)
static lv_obj_t *_ss_title, *_ss_sub, *_ss_dot, *_ss_status, *_ss_ask_box, *_ss_ask, *_ss_note;
static lv_obj_t *_ss_reject, *_ss_approve, *_ss_approve_fill, *_ss_approve_label, *_ss_talk, *_ss_talk_label, *_ss_focus;
static lv_obj_t *_ss_confirm, *_ss_confirm_text, *_ss_toast, *_ss_toast_label;
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
                        lv_obj_t** label_out = nullptr)
{
    lv_obj_t* b = box(parent, bg, LV_OPA_COVER, 12);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_add_flag(b, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(b, LV_OBJ_FLAG_PRESS_LOCK);           // stay pressed if the finger drifts
    lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLL_CHAIN);      // a drift must not scroll the screen (= press lost)
    lv_obj_remove_flag(b, LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_opa(b, LV_OPA_70, LV_STATE_PRESSED);
    lv_obj_t* l = text(b, &lv_font_montserrat_16, fg);
    lv_label_set_text(l, label);
    lv_obj_center(l);
    if (label_out) *label_out = l;
    return b;
}

static void build_session_screen(lv_obj_t* scr)
{
    _ss = box(scr, C_BG, LV_OPA_COVER, 0);
    lv_obj_set_size(_ss, 320, 240);
    lv_obj_set_pos(_ss, 320, 0);
    lv_obj_add_flag(_ss, LV_OBJ_FLAG_CLICKABLE);   // swallow taps so they don't reach the face

    lv_obj_t* back = box(_ss, C_SURFACE, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_size(back, 34, 34);
    lv_obj_set_pos(back, 10, 10);
    lv_obj_add_flag(back, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(back, 10);
    lv_obj_t* bl = text(back, &lv_font_montserrat_16, C_TEXT);
    lv_label_set_text(bl, LV_SYMBOL_LEFT);
    lv_obj_center(bl);
    lv_obj_add_event_cb(back, [](lv_event_t*) { _back_flag = true; }, LV_EVENT_CLICKED, nullptr);

    _ss_title = text(_ss, &lv_font_montserrat_20, C_TEXT);
    lv_obj_set_width(_ss_title, 256);
    lv_label_set_long_mode(_ss_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_title, 54, 8);
    _ss_sub = text(_ss, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(_ss_sub, 256);
    lv_label_set_long_mode(_ss_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_sub, 54, 31);

    _ss_dot = box(_ss, C_OFFLINE, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_size(_ss_dot, 10, 10);
    lv_obj_set_pos(_ss_dot, 16, 62);
    _ss_status = text(_ss, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_width(_ss_status, 280);
    lv_label_set_long_mode(_ss_status, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_status, 34, 57);

    _ss_ask_box = box(_ss, C_SURFACE, LV_OPA_COVER, 12);
    lv_obj_set_size(_ss_ask_box, 300, 68);
    lv_obj_set_pos(_ss_ask_box, 10, 82);
    _ss_ask = text(_ss_ask_box, &lv_font_montserrat_14, C_TEXT);
    lv_obj_set_width(_ss_ask, 280);
    lv_label_set_long_mode(_ss_ask, LV_LABEL_LONG_DOT);
    lv_obj_set_height(_ss_ask, 54);
    lv_obj_set_pos(_ss_ask, 10, 7);

    _ss_note = text(_ss, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(_ss_note, 300);
    lv_obj_set_style_text_align(_ss_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(_ss_note, 10, 206);

    _ss_reject = button(_ss, LV_SYMBOL_CLOSE "  Reject", C_SURFACE, 0xFF6961, 10, 158, 146, 36);
    lv_obj_add_event_cb(_ss_reject, [](lv_event_t*) { _reject_flag = true; }, LV_EVENT_CLICKED, nullptr);
    _ss_approve = button(_ss, "", 0x0E3B1C, C_READY, 164, 158, 146, 36);
    _ss_approve_fill = box(_ss_approve, C_READY, LV_OPA_40, 12);
    lv_obj_set_size(_ss_approve_fill, 0, 36);
    _ss_approve_label = text(_ss_approve, &lv_font_montserrat_16, C_READY);
    lv_label_set_text(_ss_approve_label, LV_SYMBOL_OK "  Hold to approve");
    lv_obj_center(_ss_approve_label);
    lv_obj_add_event_cb(_ss_approve, [](lv_event_t*) { _approve_down = GetHAL().millis() | 1; }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(_ss_approve, [](lv_event_t*) { _approve_down = 0; }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(_ss_approve, [](lv_event_t*) { _approve_down = 0; }, LV_EVENT_PRESS_LOST, nullptr);

    _ss_talk = button(_ss, "", 0x10243F, 0x64A8FF, 10, 200, 196, 34, &_ss_talk_label);
    lv_label_set_text(_ss_talk_label, LV_SYMBOL_AUDIO "  Hold to talk");
    lv_obj_add_event_cb(_ss_talk, [](lv_event_t*) { _talk_down = true; }, LV_EVENT_PRESSED, nullptr);
    lv_obj_add_event_cb(_ss_talk, [](lv_event_t*) { _talk_down = false; }, LV_EVENT_RELEASED, nullptr);
    lv_obj_add_event_cb(_ss_talk, [](lv_event_t*) { mclog::tagInfo(_tag, "talk: press lost"); }, LV_EVENT_PRESS_LOST, nullptr);
    _ss_focus = button(_ss, LV_SYMBOL_EYE_OPEN "  Mac", C_SURFACE, C_TEXT, 214, 200, 96, 34);
    lv_obj_add_event_cb(_ss_focus, [](lv_event_t*) { _focus_flag = true; }, LV_EVENT_CLICKED, nullptr);

    // Transcript confirmation (over the session screen).
    _ss_confirm = box(_ss, C_SHEET, LV_OPA_COVER, 16);
    lv_obj_set_size(_ss_confirm, 300, 150);
    lv_obj_set_pos(_ss_confirm, 10, 80);
    lv_obj_set_style_border_color(_ss_confirm, lv_color_hex(0x3A3A3C), 0);
    lv_obj_set_style_border_width(_ss_confirm, 1, 0);
    lv_obj_t* ch = text(_ss_confirm, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(ch, "Send this prompt?");
    lv_obj_set_pos(ch, 14, 10);
    _ss_confirm_text = text(_ss_confirm, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_width(_ss_confirm_text, 272);
    lv_obj_set_height(_ss_confirm_text, 62);
    lv_label_set_long_mode(_ss_confirm_text, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_ss_confirm_text, 14, 32);
    lv_obj_t* cancel = button(_ss_confirm, "Cancel", C_SURFACE, C_TEXT, 12, 102, 132, 36);
    lv_obj_add_event_cb(cancel, [](lv_event_t*) { _cancel_flag = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_t* send = button(_ss_confirm, LV_SYMBOL_UPLOAD "  Send", 0x0A84FF, 0xFFFFFF, 154, 102, 132, 36);
    lv_obj_add_event_cb(send, [](lv_event_t*) { _send_flag = true; }, LV_EVENT_CLICKED, nullptr);
    lv_obj_add_flag(_ss_confirm, LV_OBJ_FLAG_HIDDEN);

    // Toast for results ("Approved", "Sent", errors).
    _ss_toast = box(scr, 0x2C2C2E, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_size(_ss_toast, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_hor(_ss_toast, 16, 0);
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

// ------------------------------------------------------------------ app lifecycle

static std::atomic<bool> _tap_flag{false};
static std::atomic<bool> _quit_flag{false};
static std::atomic<int> _open_flag{0};     // 1..4 = sheet row, 100 = row shown on the bottom card
static size_t _touch_conn = 0;
static std::atomic<uint32_t> _press_ms{0};

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
    _mood = "";
    _rows.clear();
    _count = _more = _n_needs = _n_working = _n_ready = 0;
    _sheet_open    = false;
    _sweat_id      = -1;
    _was_connected = false;
    _ticker_key.clear();
    _dot_anim.clear();
    _tap_flag  = false;
    _quit_flag = false;
    {
        LvglLockGuard lock;
        buildUi();
        GetStackChan().addModifier(std::make_unique<BlinkModifier>());
        GetStackChan().addModifier(std::make_unique<BreathModifier>());
        setMood("offline");
        renderChip();
        renderTicker(false);
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
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    // Face fills the screen; tapping it opens the session list.
    auto face = std::make_unique<avatar::DefaultAvatar>();
    face->init(scr);
    face->getPanel()->onClick().connect([]() { _tap_flag = true; });
    GetStackChan().attachAvatar(std::move(face));

    // Status chip, top-left: pulsing dot + summary (+ mute icon in quiet mode).
    _chip = box(scr, C_SURFACE, LV_OPA_80, LV_RADIUS_CIRCLE);
    lv_obj_set_size(_chip, LV_SIZE_CONTENT, 28);
    lv_obj_set_pos(_chip, 10, 10);
    lv_obj_set_style_pad_hor(_chip, 12, 0);
    lv_obj_set_flex_flow(_chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(_chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(_chip, 8, 0);
    _chip_dot = box(_chip, C_OFFLINE, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_size(_chip_dot, 10, 10);
    _chip_label = text(_chip, &lv_font_montserrat_14, C_TEXT);
    _chip_mute  = text(_chip, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(_chip_mute, LV_SYMBOL_MUTE);
    lv_obj_add_flag(_chip_mute, LV_OBJ_FLAG_HIDDEN);

    // Session card, bottom: accent bar, title, subtitle, age.
    _card = box(scr, C_SURFACE, LV_OPA_90, 16);
    lv_obj_set_size(_card, 300, 50);
    lv_obj_align(_card, LV_ALIGN_BOTTOM_MID, 0, -10);
    lv_obj_add_flag(_card, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_card, [](lv_event_t*) { _open_flag = 100; }, LV_EVENT_CLICKED, nullptr);
    _card_bar = box(_card, C_OFFLINE, LV_OPA_COVER, 2);
    lv_obj_set_size(_card_bar, 4, 30);
    lv_obj_align(_card_bar, LV_ALIGN_LEFT_MID, 12, 0);
    _card_title = text(_card, &lv_font_montserrat_16, C_TEXT);
    lv_obj_set_width(_card_title, 200);
    lv_label_set_long_mode(_card_title, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_card_title, 26, 7);
    _card_sub = text(_card, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_width(_card_sub, 220);
    lv_label_set_long_mode(_card_sub, LV_LABEL_LONG_DOT);
    lv_obj_set_pos(_card_sub, 26, 27);
    _card_age = text(_card, &lv_font_montserrat_14, C_MUTED);
    lv_obj_align(_card_age, LV_ALIGN_TOP_RIGHT, -14, 8);

    // Session list sheet: starts below the screen, slides up on tap.
    _sheet = box(scr, C_SHEET, LV_OPA_COVER, 20);
    lv_obj_set_size(_sheet, 320, 236);
    lv_obj_set_pos(_sheet, 0, 240);
    lv_obj_add_flag(_sheet, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(_sheet, [](lv_event_t*) { _tap_flag = true; }, LV_EVENT_CLICKED, nullptr);

    lv_obj_t* grabber = box(_sheet, 0x3A3A3C, LV_OPA_COVER, 2);
    lv_obj_set_size(grabber, 36, 4);
    lv_obj_align(grabber, LV_ALIGN_TOP_MID, 0, 7);
    lv_obj_t* title = text(_sheet, &lv_font_montserrat_20, C_TEXT);
    lv_label_set_text(title, "Sessions");
    lv_obj_set_pos(title, 16, 18);
    _sheet_count = text(_sheet, &lv_font_montserrat_16, C_MUTED);
    lv_obj_set_pos(_sheet_count, 114, 21);

    lv_obj_t* quit = box(_sheet, C_SURFACE, LV_OPA_COVER, LV_RADIUS_CIRCLE);
    lv_obj_set_size(quit, 30, 30);
    lv_obj_set_pos(quit, 276, 14);
    lv_obj_add_flag(quit, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(quit, 8);
    lv_obj_t* x = text(quit, &lv_font_montserrat_14, C_MUTED);
    lv_label_set_text(x, LV_SYMBOL_CLOSE);
    lv_obj_center(x);
    lv_obj_add_event_cb(quit, [](lv_event_t*) { _quit_flag = true; }, LV_EVENT_CLICKED, nullptr);

    for (int i = 0; i < 4; i++) {
        lv_obj_t* c = box(_sheet, C_SURFACE, LV_OPA_COVER, 12);
        lv_obj_set_size(c, 296, 34);
        lv_obj_set_pos(c, 12, 54 + i * 38);
        _sheet_cards[i] = c;
        _sheet_bars[i]  = box(c, C_OFFLINE, LV_OPA_COVER, 2);
        lv_obj_set_size(_sheet_bars[i], 4, 18);
        lv_obj_align(_sheet_bars[i], LV_ALIGN_LEFT_MID, 10, 0);
        _sheet_names[i] = text(c, &lv_font_montserrat_16, C_TEXT);
        lv_obj_set_width(_sheet_names[i], 130);
        lv_label_set_long_mode(_sheet_names[i], LV_LABEL_LONG_DOT);
        lv_obj_align(_sheet_names[i], LV_ALIGN_LEFT_MID, 22, 0);
        _sheet_details[i] = text(c, &lv_font_montserrat_14, C_MUTED);
        lv_obj_set_width(_sheet_details[i], 134);
        lv_label_set_long_mode(_sheet_details[i], LV_LABEL_LONG_DOT);
        lv_obj_set_style_text_align(_sheet_details[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(_sheet_details[i], LV_ALIGN_RIGHT_MID, -12, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, [](lv_event_t* e) { _open_flag = 1 + (int)(intptr_t)lv_event_get_user_data(e); },
                            LV_EVENT_CLICKED, (void*)(intptr_t)i);
    }
    build_session_screen(scr);
    _sheet_more = text(_sheet, &lv_font_montserrat_14, C_MUTED);
    lv_obj_set_pos(_sheet_more, 18, 208);
}

// ------------------------------------------------------------------ state -> screen

void AppCcMonitor::applyDisplay(const std::string& body)
{
    JsonDocument doc;
    if (deserializeJson(doc, body) || std::string(doc["type"] | "") != "display") {
        return;
    }
    _leds      = doc["leds"] | "off";
    _count     = doc["count"] | 0;
    _more      = doc["more"] | 0;
    _n_needs   = doc["counts"]["needs_you"] | 0;
    _n_working = doc["counts"]["working"] | 0;
    _n_ready   = doc["counts"]["ready"] | 0;
    _quiet     = doc["quiet"] | false;
    _rx_ms     = GetHAL().millis();
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
        row.state        = r["state"] | "";
        row.age_sec      = r["ageSec"] | 0;
        row.ghostty      = r["ghostty"] | false;
        _rows.push_back(row);
    }
    setMood(doc["mood"] | "none");
    renderChip();
    renderTicker(false);
    renderSheet();
    if (doc["chime"] | false) {
        hal_bridge::app_play_sound(OGG_NEW_NOTIFICATION);
    }
}

void AppCcMonitor::setMood(const std::string& mood)
{
    if (mood == _mood) return;
    auto& face = GetStackChan().avatar();
    if (_sweat_id >= 0) {
        face.removeDecorator(_sweat_id);
        _sweat_id = -1;
    }
    auto& motion = GetStackChan().motion();
    if (mood == "needs_you") {
        face.setEmotion(avatar::Emotion::Doubt);
        _sweat_id = face.addDecorator(std::make_unique<avatar::SweatDecorator>(lv_screen_active(), 0, 700));
        motion.lookAtNormalized(0.0f, 0.45f, 350);   // look up at you
    } else if (mood == "working") {
        face.setEmotion(avatar::Emotion::Neutral);
        motion.lookAtNormalized(0.0f, -0.12f, 200);  // head slightly down, concentrating
    } else if (mood == "just_finished") {
        face.setEmotion(avatar::Emotion::Happy);
        face.addDecorator(std::make_unique<avatar::HeartDecorator>(lv_screen_active(), 2600, 450));
        motion.lookAtNormalized(0.0f, -0.35f, 450);  // nod; returns to level in updateHead()
        _nod_until = GetHAL().millis() + 450;
    } else if (mood == "all_done") {
        face.setEmotion(avatar::Emotion::Happy);
        motion.lookAtNormalized(0.0f, 0.0f, 200);
    } else if (mood == "none") {
        face.setEmotion(avatar::Emotion::Sleepy);
        motion.lookAtNormalized(0.0f, -0.2f, 150);
    } else {   // offline
        face.setEmotion(avatar::Emotion::Sad);
        motion.lookAtNormalized(0.0f, -0.1f, 150);
    }
    _mood = mood;
}

void AppCcMonitor::renderChip()
{
    std::string s;
    uint32_t color    = C_OFFLINE;
    uint32_t pulse_ms = 0;
    if (!_connected) {
        s = "Offline";
    } else if (_count == 0) {
        s = "No sessions";
    } else if (_n_needs) {
        s = std::to_string(_n_needs) + (_n_needs == 1 ? " needs you" : " need you");
        if (_n_working) s += "  ·  " + std::to_string(_n_working) + " working";
        color = C_NEEDS, pulse_ms = 900;
    } else if (_n_working) {
        s = std::to_string(_n_working) + " working";
        if (_n_ready) s += "  ·  " + std::to_string(_n_ready) + " ready";
        color = C_WORKING, pulse_ms = 2400;
    } else {
        s     = _n_ready == 1 ? "1 ready" : "All " + std::to_string(_n_ready) + " ready";
        color = C_READY;
    }
    lv_label_set_text(_chip_label, s.c_str());
    lv_obj_set_style_bg_color(_chip_dot, lv_color_hex(color), 0);
    std::string anim = std::to_string(color) + ":" + std::to_string(pulse_ms);
    if (anim != _dot_anim) {
        pulse(_chip_dot, pulse_ms);
        _dot_anim = anim;
    }
    if (_quiet) {
        lv_obj_remove_flag(_chip_mute, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(_chip_mute, LV_OBJ_FLAG_HIDDEN);
    }
}

void AppCcMonitor::renderTicker(bool advance)
{
    std::string title, sub, age;
    uint32_t bar = C_OFFLINE, sub_color = C_MUTED;
    if (!_connected) {
        std::lock_guard<std::mutex> lock(_rx_mutex);
        title = "Not connected";
        sub   = _net_msg.empty() ? "Starting Wi-Fi..." : _net_msg;
    } else if (_rows.empty()) {
        title = "All quiet";
        sub   = "No Claude sessions running";
    } else {
        // Cycle through the sessions that need you if any, otherwise through all of them.
        int pool = 0;
        while (pool < (int)_rows.size() && _rows[pool].state == "needs_you") pool++;
        if (pool == 0) pool = _rows.size();
        if (advance) _ticker_index++;
        _ticker_index %= pool;
        const Row& r = _rows[_ticker_index];
        title        = r.name;
        sub          = r.detail + (r.ghostty ? "" : "  ·  not in Ghostty");
        age          = fmt_age(r.age_sec + (GetHAL().millis() - _rx_ms) / 1000);
        bar = sub_color = state_color(r.state);
    }
    lv_label_set_text(_card_title, title.c_str());
    lv_label_set_text(_card_sub, sub.c_str());
    lv_label_set_text(_card_age, age.c_str());
    lv_obj_set_style_text_color(_card_sub, lv_color_hex(sub_color), 0);
    lv_obj_set_style_bg_color(_card_bar, lv_color_hex(bar), 0);
    std::string key = title + "|" + sub;
    if (key != _ticker_key) {
        fade_in(_card_title, 300);
        fade_in(_card_sub, 300);
        _ticker_key = key;
    }
}

void AppCcMonitor::renderSheet()
{
    lv_label_set_text_fmt(_sheet_count, "%d", _count);
    for (int i = 0; i < 4; i++) {
        if (i < (int)_rows.size()) {
            const Row& r = _rows[i];
            uint32_t c   = state_color(r.state);
            lv_obj_remove_flag(_sheet_cards[i], LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(_sheet_bars[i], lv_color_hex(c), 0);
            lv_label_set_text(_sheet_names[i], r.name.c_str());
            std::string d = r.detail + "  " + fmt_age(r.age_sec + (GetHAL().millis() - _rx_ms) / 1000);
            lv_label_set_text(_sheet_details[i], d.c_str());
            lv_obj_set_style_text_color(_sheet_details[i], lv_color_hex(c), 0);
        } else {
            lv_obj_add_flag(_sheet_cards[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (_more > 0) {
        lv_label_set_text_fmt(_sheet_more, "+ %d more", _more);
    } else if (_rows.empty()) {
        lv_label_set_text(_sheet_more, _connected ? "No Claude sessions running" : "Not connected");
    } else {
        lv_label_set_text(_sheet_more, "");
    }
}

void AppCcMonitor::showSheet(bool show)
{
    _sheet_open      = show;
    _sheet_opened_ms = GetHAL().millis();
    lv_anim_delete(_sheet, anim_y_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, _sheet);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_values(&a, lv_obj_get_y(_sheet), show ? 24 : 240);
    lv_anim_set_duration(&a, show ? 340 : 240);
    lv_anim_set_path_cb(&a, show ? lv_anim_path_overshoot : lv_anim_path_ease_in);
    lv_anim_start(&a);
}

void AppCcMonitor::updateLeds()
{
    uint32_t now = GetHAL().millis();
    if (now - _led_tick < 40) return;   // ~25 fps
    _led_tick = now;
    uint8_t r = 0, g = 0, b = 0;
    if (_leds == "flash") {
        if ((now / 250) % 2 == 0) { r = 200; }                               // red, twice a second
    } else if (_leds == "yellow_breathe") {
        float k = 0.5f - 0.5f * cosf((now % 3000) / 3000.0f * 2.0f * 3.14159f);   // 3 s breath
        k       = 0.08f + 0.92f * k;
        r = (uint8_t)(168 * k); g = (uint8_t)(140 * k);
    } else if (_leds == "green") {
        g = 120;
    } else if (_leds == "dim_white") {
        r = g = b = 20;
    }
    GetStackChan().leftNeonLight().setColor(r, g, b);
    GetStackChan().rightNeonLight().setColor(r, g, b);
}

void AppCcMonitor::updateHead()
{
    if (_nod_until && GetHAL().millis() > _nod_until) {
        _nod_until = 0;
        GetStackChan().motion().lookAtNormalized(0.0f, 0.0f, 300);
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
    } else if (connected != _was_connected || (!connected && now - _ticker_ms > 500)) {
        if (!connected) {
            _leds = "dim_white";
            setMood("offline");
        }
        renderChip();
        renderTicker(false);
        renderSheet();
        _ticker_ms = now;
    }
    _was_connected = connected;
    if (connected && now - _ticker_ms > 4000) {   // rotate the bottom card
        renderTicker(true);
        _ticker_ms = now;
    }
    if (_tap_flag.exchange(false)) {
        showSheet(!_sheet_open);
    }
    if (_sheet_open && now - _sheet_opened_ms > 12000) {
        showSheet(false);
    }
    sessionTick();
    updateLeds();
    updateHead();
    GetStackChan().update();   // pushes face animation, LED colours and servo targets to the hardware
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
    GetStackChan().clearModifiers();
    GetStackChan().avatar().clearDecorators();
    GetStackChan().resetAvatar();
    GetStackChan().leftNeonLight().setColor(0, 0, 0);
    GetStackChan().rightNeonLight().setColor(0, 0, 0);
    GetStackChan().motion().goHome(300);
    lv_obj_clean(lv_screen_active());
    _chip = _card = _sheet = nullptr;
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
            toast(err.empty() ? "Didn't catch that" : "Transcription failed", 0xFF6961);
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
            toast(action == "approve" ? LV_SYMBOL_OK "  Approved" : action == "reject" ? "Rejected" : LV_SYMBOL_OK "  Sent", C_READY);
        } else {
            toast(err, 0xFF6961);
        }
    }
}

void AppCcMonitor::openSession(int row)
{
    if (row < 0 || row >= (int)_rows.size()) return;
    _open_session = _rows[row].session;
    if (_sheet_open) showSheet(false);
    show(_ss_confirm, false);
    renderSession();
    lv_obj_move_foreground(_ss);
    slide_x(_ss, 0);
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
    if (!r) {
        lv_label_set_text(_ss_status, "Session ended or not in the top 4");
        lv_obj_set_style_bg_color(_ss_dot, lv_color_hex(C_OFFLINE), 0);
        for (auto o : {_ss_reject, _ss_approve, _ss_talk, _ss_focus, _ss_ask_box}) show(o, false);
        return;
    }
    uint32_t c = state_color(r->state);
    lv_label_set_text(_ss_title, r->name.c_str());
    std::string sub = r->project + "  ·  " + r->host;
    lv_label_set_text(_ss_sub, sub.c_str());
    lv_obj_set_style_bg_color(_ss_dot, lv_color_hex(c), 0);
    std::string st = r->detail + "  ·  " + fmt_age(r->age_sec + (GetHAL().millis() - _rx_ms) / 1000);
    lv_label_set_text(_ss_status, st.c_str());
    lv_obj_set_style_text_color(_ss_status, lv_color_hex(c), 0);

    bool waiting = r->state == "needs_you";
    std::string ask = !r->ask.empty() ? r->ask
                      : waiting           ? "Waiting for you in the terminal."
                      : r->state == "working" ? "Claude is working..."
                                              : "Ready for your next prompt.";
    lv_label_set_text(_ss_ask, ask.c_str());
    show(_ss_ask_box, true);
    lv_obj_set_height(_ss_ask_box, waiting ? 68 : 110);
    lv_obj_set_height(_ss_ask, waiting ? 54 : 96);
    show(_ss_reject, waiting && r->ghostty);
    show(_ss_approve, waiting && r->ghostty);
    show(_ss_talk, r->ghostty);
    show(_ss_focus, r->ghostty);
    if (!r->ghostty) {
        lv_label_set_text(_ss_note, "Not in Ghostty: view only");
    } else {
        lv_label_set_text(_ss_note, "");
    }
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
    if (open == 100) openSession(_rows.empty() ? -1 : _ticker_index % (int)_rows.size());
    else if (open > 0) openSession(open - 1);
    // Never wait forever for a transcript (server restarted, connection dropped, STT down).
    if (_transcribing && (!_connected || now - _transcribe_ms > 25000)) {
        _transcribing = false;
        mclog::tagWarn(_tag, "talk: no transcript ({})", _connected ? "timeout" : "disconnected");
        if (!_open_session.empty()) {
            toast(_connected ? "No reply, try again" : "Connection lost, try again", 0xFF6961);
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
        toast("Not connected", 0xFF6961);
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
