// Bluetooth build only: see cc_ble.h. Modelled on the stock main/hal/utils/bleprph, which this build leaves off.
#include "cc_monitor_config.h"
#ifdef CC_MONITOR_BLE

#include "cc_ble.h"
#include "frames.h"

#include <atomic>
#include <cstring>
#include <mutex>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

extern "C" void ble_store_config_init(void);

namespace cc_ble {
namespace {

const char* TAG = "CC-BLE";

// e2e5e5e0-1234-5678-1234-56789abcdef0 (the stock StackChan service UUID) and e2e5e5e5-... (our characteristic)
const ble_uuid128_t SVC_UUID = BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34,
                                                0x12, 0xe0, 0xe5, 0xe5, 0xe2);
const ble_uuid128_t CHR_UUID = BLE_UUID128_INIT(0xf0, 0xde, 0xbc, 0x9a, 0x78, 0x56, 0x34, 0x12, 0x78, 0x56, 0x34,
                                                0x12, 0xe5, 0xe5, 0xe5, 0xe2);
// Advertised as manufacturer data (company 0xFFFF, "for testing") while a new Mac may pair.
const uint8_t PAIRING_MFG[3] = {0xFF, 0xFF, 0x01};

Events _ev;
uint16_t _chr_handle = 0;
uint8_t _own_addr_type = 0;
int64_t _start_us = 0;
std::atomic<uint16_t> _conn{BLE_HS_CONN_HANDLE_NONE};
std::atomic<bool> _secure{false};       // encrypted, authenticated and bonded
std::atomic<bool> _subscribed{false};
std::atomic<int> _advertised_pairing{-1};   // what the current advertisement says (-1 = not advertising)
std::atomic<int> _last_state{-1};
frames::Joiner _joiner;                 // only touched on the host task
std::mutex _send_mutex;

bool pairing_open()
{
    return frames::may_pair(usb_serial_jtag_is_connected(), (uint32_t)((esp_timer_get_time() - _start_us) / 1000));
}

bool bonded()
{
    int n = 0;
    return ble_store_util_count(BLE_STORE_OBJ_TYPE_PEER_SEC, &n) == 0 && n > 0;
}

void report_state()
{
    State s = _secure ? State::Connected : bonded() ? State::Waiting : pairing_open() ? State::Pairing : State::NotPaired;
    if (_last_state.exchange((int)s) != (int)s && _ev.on_state) _ev.on_state(s);
}

int gap_event(ble_gap_event* event, void* arg);

void advertise()
{
    if (_conn != BLE_HS_CONN_HANDLE_NONE) return;
    const bool pairing = pairing_open();
    if (ble_gap_adv_active()) {
        if (_advertised_pairing == (int)pairing) return;
        ble_gap_adv_stop();
    }
    ble_hs_adv_fields fields = {};
    fields.flags                = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128             = &SVC_UUID;
    fields.num_uuids128         = 1;
    fields.uuids128_is_complete = 1;
    if (pairing) {
        fields.mfg_data     = PAIRING_MFG;
        fields.mfg_data_len = sizeof(PAIRING_MFG);
    }
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0) {
        ble_hs_adv_fields rsp = {};
        const char* name     = ble_svc_gap_device_name();
        rsp.name             = (const uint8_t*)name;
        rsp.name_len         = strlen(name);
        rsp.name_is_complete = 1;
        rc                   = ble_gap_adv_rsp_set_fields(&rsp);
    }
    if (rc == 0) {
        ble_gap_adv_params params = {};
        params.conn_mode          = BLE_GAP_CONN_MODE_UND;
        params.disc_mode          = BLE_GAP_DISC_MODE_GEN;
        rc = ble_gap_adv_start(_own_addr_type, nullptr, BLE_HS_FOREVER, &params, gap_event, nullptr);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed: %d", rc);
        _advertised_pairing = -1;
        return;
    }
    _advertised_pairing = (int)pairing;
    ESP_LOGI(TAG, "advertising%s", pairing ? " (pairing open)" : "");
}

// Delete every bond except `keep`'s: a new pairing replaces the previous Mac.
void forget_others(const ble_addr_t& keep)
{
    ble_addr_t peers[4];
    int n = 0;
    if (ble_store_util_bonded_peers(peers, &n, 4) != 0) return;
    for (int i = 0; i < n; i++) {
        if (ble_addr_cmp(&peers[i], &keep) != 0) {
            ESP_LOGI(TAG, "forgetting the previously paired Mac");
            ble_store_util_delete_peer(&peers[i]);
        }
    }
}

void drop(uint16_t conn, const char* why)
{
    ESP_LOGW(TAG, "dropping connection: %s", why);
    ble_gap_terminate(conn, BLE_ERR_REM_USER_CONN_TERM);
}

int gap_event(ble_gap_event* event, void*)
{
    ble_gap_conn_desc desc;
    switch (event->type) {
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status != 0) {
                advertise();
                return 0;
            }
            _conn       = event->connect.conn_handle;
            _secure     = false;
            _subscribed = false;
            _joiner     = frames::Joiner();
            _advertised_pairing = -1;
            ESP_LOGI(TAG, "connected");
            if (!bonded() && !pairing_open()) {
                drop(_conn, "not paired and pairing is closed");
                return 0;
            }
            ble_gap_security_initiate(_conn);
            return 0;

        case BLE_GAP_EVENT_DISCONNECT:
            ESP_LOGI(TAG, "disconnected: %d", event->disconnect.reason);
            _conn       = BLE_HS_CONN_HANDLE_NONE;
            _secure     = false;
            _subscribed = false;
            if (_ev.on_passkey) _ev.on_passkey(0);
            report_state();
            advertise();
            return 0;

        case BLE_GAP_EVENT_PASSKEY_ACTION: {
            const uint16_t conn = event->passkey.conn_handle;
            if (event->passkey.params.action != BLE_SM_IOACT_DISP || !pairing_open()) {
                drop(conn, "pairing not allowed now");
                return 0;
            }
            ble_sm_io io = {};
            io.action    = BLE_SM_IOACT_DISP;
            io.passkey   = 1 + esp_random() % 999999;   // never 0: on_passkey(0) means pairing is over
            ble_sm_inject_io(conn, &io);
            ESP_LOGI(TAG, "pairing: showing a code");
            if (_ev.on_passkey) _ev.on_passkey(io.passkey);
            return 0;
        }

        case BLE_GAP_EVENT_ENC_CHANGE: {
            if (_ev.on_passkey) _ev.on_passkey(0);
            const uint16_t conn = event->enc_change.conn_handle;
            if (ble_gap_conn_find(conn, &desc) != 0) return 0;
            const bool ok = event->enc_change.status == 0 && desc.sec_state.encrypted &&
                            desc.sec_state.authenticated && desc.sec_state.bonded;
            ESP_LOGI(TAG, "encryption: status=%d encrypted=%d authenticated=%d bonded=%d", event->enc_change.status,
                     desc.sec_state.encrypted, desc.sec_state.authenticated, desc.sec_state.bonded);
            if (!ok) {
                if (event->enc_change.status == 0 && !desc.sec_state.authenticated) {
                    ble_store_util_delete_peer(&desc.peer_id_addr);   // never keep an unauthenticated bond
                }
                drop(conn, "link not secure");
                return 0;
            }
            if (pairing_open()) forget_others(desc.peer_id_addr);   // one Mac at a time
            _secure = true;
            // Ask for a short connection interval (15-30 ms) so a full update arrives quickly.
            ble_gap_upd_params p = {};
            p.itvl_min = 12, p.itvl_max = 24, p.latency = 0, p.supervision_timeout = 400;
            ble_gap_update_params(conn, &p);
            report_state();
            return 0;
        }

        case BLE_GAP_EVENT_REPEAT_PAIRING:
            // The paired Mac lost its keys (or a new pairing): forget ours and pair again, if pairing is open.
            if (!pairing_open()) return BLE_GAP_REPEAT_PAIRING_IGNORE;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;

        case BLE_GAP_EVENT_SUBSCRIBE:
            if (event->subscribe.attr_handle == _chr_handle) _subscribed = event->subscribe.cur_notify;
            return 0;

        case BLE_GAP_EVENT_MTU:
            ESP_LOGI(TAG, "mtu %d", event->mtu.value);
            return 0;

        case BLE_GAP_EVENT_ADV_COMPLETE:
            advertise();
            return 0;
    }
    return 0;
}

// When the bond store is full, NimBLE would push out the oldest bond (the paired Mac) for a new pairing. Outside
// the pairing window that pairing must fail instead.
int store_status(ble_store_status_event* event, void* arg)
{
    if (event->event_code == BLE_STORE_EVENT_OVERFLOW && !pairing_open()) return BLE_HS_ESTORE_CAP;
    return ble_store_util_status_rr(event, arg);
}

int chr_access(uint16_t conn, uint16_t, ble_gatt_access_ctxt* ctxt, void*)
{
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_UNLIKELY;
    if (!_secure || conn != _conn) return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
    uint8_t buf[600];
    uint16_t n = 0;
    if (ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &n) != 0) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    switch (_joiner.push(buf, n)) {
        case frames::Joiner::Got::Message:
            if (_ev.on_message) _ev.on_message(_joiner.take());
            break;
        case frames::Joiner::Got::Error:
            ESP_LOGW(TAG, "bad packet, message dropped");
            break;
        case frames::Joiner::Got::Partial:
            break;
    }
    return 0;
}

const ble_gatt_chr_def CHRS[] = {
    {
        .uuid       = &CHR_UUID.u,
        .access_cb  = chr_access,
        .arg        = nullptr,
        .descriptors = nullptr,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC |
                 BLE_GATT_CHR_F_WRITE_AUTHEN | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 16,
        .val_handle   = &_chr_handle,
    },
    {},
};

const ble_gatt_svc_def SVCS[] = {
    {.type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &SVC_UUID.u, .includes = nullptr, .characteristics = CHRS},
    {},
};

void on_sync()
{
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &_own_addr_type);
    report_state();
    advertise();
}

void host_task(void*)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

void tick(void*)   // once a second: the pairing window opens/closes with USB and time
{
    if (!ble_hs_synced()) return;
    report_state();
    advertise();
}

}   // namespace

void start(Events ev)
{
    static bool started = false;
    if (started) return;
    started   = true;
    _ev       = std::move(ev);
    _start_us = esp_timer_get_time();

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed");
        return;
    }
    ble_hs_cfg.sync_cb         = on_sync;
    ble_hs_cfg.store_status_cb = store_status;
    ble_hs_cfg.sm_io_cap       = BLE_HS_IO_DISPLAY_ONLY;
    ble_hs_cfg.sm_bonding      = 1;
    ble_hs_cfg.sm_mitm         = 1;
    ble_hs_cfg.sm_sc           = 1;
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    ble_gatts_count_cfg(SVCS);
    ble_gatts_add_svcs(SVCS);
    ble_svc_gap_device_name_set("StackChan CC");
    ble_store_config_init();
    nimble_port_freertos_init(host_task);

    static esp_timer_handle_t timer;
    esp_timer_create_args_t args = {};
    args.callback = tick;
    args.name     = "cc_ble_tick";
    esp_timer_create(&args, &timer);
    esp_timer_start_periodic(timer, 1000 * 1000);
}

bool send(const std::string& json)
{
    std::lock_guard<std::mutex> lock(_send_mutex);
    const uint16_t conn = _conn;
    if (!_secure || !_subscribed || conn == BLE_HS_CONN_HANDLE_NONE) return false;
    for (const auto& p : frames::split(json, ble_att_mtu(conn) - 3)) {
        os_mbuf* om = ble_hs_mbuf_from_flat(p.data(), p.size());
        if (!om || ble_gatts_notify_custom(conn, _chr_handle, om) != 0) return false;
    }
    return true;
}

}   // namespace cc_ble

#endif   // CC_MONITOR_BLE
