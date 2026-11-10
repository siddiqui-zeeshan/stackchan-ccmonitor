/*
 * CC Monitor: shows live Claude Code session state from cc-monitor.
 * Animated StackChan face (emotion per mood), status chip, rotating session card, slide-up session list.
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
        int age_sec = 0;
        bool ghostty = false;
    };

    void buildUi();
    void applyDisplay(const std::string& json);
    void setMood(const std::string& mood);
    void renderChip();
    void renderTicker(bool animate);
    void renderSheet();
    void showSheet(bool show);
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
    int _count = 0, _more = 0, _n_needs = 0, _n_working = 0, _n_ready = 0;
    bool _quiet = false;
    uint32_t _rx_ms = 0;          // when _rows arrived (ages advance from here)

    // UI
    lv_obj_t* _chip = nullptr;
    lv_obj_t* _chip_dot = nullptr;
    lv_obj_t* _chip_label = nullptr;
    lv_obj_t* _chip_mute = nullptr;
    lv_obj_t* _card = nullptr;
    lv_obj_t* _card_bar = nullptr;
    lv_obj_t* _card_title = nullptr;
    lv_obj_t* _card_sub = nullptr;
    lv_obj_t* _card_age = nullptr;
    lv_obj_t* _sheet = nullptr;
    lv_obj_t* _sheet_count = nullptr;
    lv_obj_t* _sheet_more = nullptr;
    lv_obj_t* _sheet_cards[4]{};
    lv_obj_t* _sheet_bars[4]{};
    lv_obj_t* _sheet_names[4]{};
    lv_obj_t* _sheet_details[4]{};
    bool _sheet_open = false;
    uint32_t _sheet_opened_ms = 0;
    int _ticker_index = 0;
    uint32_t _ticker_ms = 0;
    std::string _ticker_key;
    std::string _dot_anim;

    // Face / motion
    int _sweat_id = -1;
    uint32_t _led_tick = 0;
    uint32_t _nod_until = 0;
    bool _was_connected = false;
};
