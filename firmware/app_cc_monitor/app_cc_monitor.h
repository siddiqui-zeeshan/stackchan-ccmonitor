/*
 * CC Monitor: shows live Claude Code session state from cc-monitor.
 * A full-colour screen with a pixel face per mood, one big number, one sentence about the session that
 * matters most, a slide-up session list and a per-session screen.
 */
#pragma once
#include <mooncake.h>
#include <lvgl.h>
#include <string>
#include <vector>

class AppCcMonitor : public mooncake::AppAbility {
public:
    AppCcMonitor();

    void onCreate() override;
    void onOpen() override;
    void onRunning() override;
    void onClose() override;

private:
    struct Row {
        std::string session, name, project, host, detail, state, ask;
        std::string say, what;        // main-screen sentence: "<name><say>", then `what` (or ask minus `cut` bytes)
        int cut = 0;
        int age_sec = 0;
        bool ghostty = false;
        bool waiting_long = false;    // needs you and has been left a while: orange instead of red
    };

    void buildUi();
    void applyDisplay(const std::string& json);
    void setMood(const std::string& mood);
    void renderMain(bool advance);
    void renderSheet();
    void showSheet(bool show);
    bool isDizzy();
    void updateFace();
    void updateLeds();
    void updateHead();
    void handleMessage(const std::string& json);
    void openSession(int row);
    void renderSession();
    void closeSession();
    void sessionTick();
    std::string _open_session;

    // Server state
    std::string _mood = "";
    std::string _leds = "dim_white";
    std::vector<Row> _rows;
    int _count = 0, _more = 0;
    int _big = 0;                              // the big number (0 = none)
    std::string _big_text, _rest_text;         // "need you" / "3 working\n1 ready"
    std::string _spot_bold, _spot_text, _spot_sub, _spot_session;   // sentence when not cycling rows
    bool _has_spot = false;
    bool _quiet = false;                       // quiet mode: mute mark
    bool _hush = false;                        // quiet mode or quiet hours: no "thinking" head moves
    uint32_t _rx_ms = 0;                       // when _rows arrived (ages advance from here)

    // Main screen
    lv_obj_t* _canvas = nullptr;
    lv_obj_t* _big_label = nullptr;
    lv_obj_t* _rest_label = nullptr;
    lv_obj_t* _line1 = nullptr;
    lv_obj_t* _line2 = nullptr;
    lv_obj_t* _mute = nullptr;
    int _spot_index = 0;
    uint32_t _spot_ms = 0;
    std::string _shown_session;                // session the sentence is about ("" = none)
    uint32_t _face_ms = 0;
    uint32_t _face_tick = 0;
    bool _face_dirty = true;

    // Session list
    lv_obj_t* _sheet = nullptr;
    lv_obj_t* _sheet_band = nullptr;
    lv_obj_t* _sheet_title = nullptr;
    lv_obj_t* _sheet_count = nullptr;
    lv_obj_t* _sheet_more = nullptr;
    lv_obj_t* _sheet_list = nullptr;
    lv_obj_t* _sheet_cards[12]{};
    lv_obj_t* _sheet_faces[12]{};
    lv_obj_t* _sheet_names[12]{};
    lv_obj_t* _sheet_tags[12]{};
    lv_obj_t* _sheet_details[12]{};
    std::string _sheet_keys[12];
    std::string _sheet_band_key;
    bool _sheet_open = false;
    uint32_t _sheet_opened_ms = 0;

    // Head / lights
    uint32_t _led_tick = 0;
    uint32_t _nod_at = 0;        // when a pending nod starts (0 = none)
    uint32_t _nod_until = 0;     // when the nod's dip ends and the head returns
    uint32_t _think_at = 0;      // next small "thinking" move while working
    bool _was_hold = false;      // list or session screen was open last time we looked: head held still
    bool _pose_pending = false;  // the mood changed while held: take its resting pose once free
    bool _was_connected = false;
    uint32_t _dizzy_until = 0;   // shaken: show the dizzy face until then
    bool _was_dizzy = false;
};
