#include "ble_voice.h"
#include "sdkconfig.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include <stdatomic.h>
#include <stdint.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_store.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#define QUEUE_FRAMES 8
#define RESERVED_CONTROL_FRAMES 3
#define AUDIO_UP_TYPE 2
#define AUDIO_DOWN_TYPE 3
#define FRAGMENT_FIRST 1
#define FRAGMENT_LAST 2
#define PREFERRED_MTU 247
#define MAX_FRAGMENT_VALUE (PREFERRED_MTU - 3)
#define FRAGMENT_TIMEOUT_US 2000000
#define PAIRING_WINDOW_MS 60000
#define RECONNECT_SECURITY_MS 12000
#define DISCONNECT_GRACE_MS 2000
#define DISCONNECT_ATTEMPTS 3
#define ADV_REFRESH_MS 100
#define ADV_RETRY_MS 1000
#define AUDIO_MAX_AGE_US 250000
#define CONTROL_MAX_AGE_US 1500000

static const char *TAG = "ble_voice";
/* NimBLE stores 128-bit UUIDs in Bluetooth little-endian byte order. */
#define VOICE_UUID(last) BLE_UUID128_INIT(0x38, 0xe7, 0x7d, 0x3a, 0xa2, 0xd4, 0x87, 0xa5, \
                                        0x82, 0x4a, 0xe7, 0x8c, last, 0xa5, 0xc3, 0x55)
static const ble_uuid128_t s_service_uuid = VOICE_UUID(0x00);
static const ble_uuid128_t s_rx_uuid = VOICE_UUID(0x01);
static const ble_uuid128_t s_tx_uuid = VOICE_UUID(0x02);

typedef struct {
    int64_t queued_us;
    uint32_t generation;
    uint16_t length;
    uint8_t bytes[BLE_VOICE_MAX_FRAME_BYTES];
} voice_frame_t;

enum peer_request { PEER_NONE, PEER_PAIR, PEER_REPLACE, PEER_REPAIR, PEER_RECONNECT };

static QueueHandle_t s_tx_queue;
static QueueHandle_t s_rx_queue;
static atomic_bool s_initialized;
static atomic_bool s_host_synced;
static atomic_bool s_ready;
static atomic_uint s_generation;
static atomic_uint s_audio_tx_drops;
static atomic_uint s_audio_rx_drops;
static atomic_uint s_audio_tx_admission_drops;
static atomic_uint s_audio_tx_retries;
static atomic_int s_peer_request;
static atomic_bool s_pair_open;
static atomic_bool s_saved_peer_set;

/* Connection, assembler, and active TX state belong only to the NimBLE host. */
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_handle;
static uint16_t s_mtu = 23;
static uint8_t s_addr_type;
static bool s_conn_closing;
static unsigned s_disconnect_attempts;
static bool s_encrypted;
static bool s_subscribed;
static bool s_peer_approved;
static ble_addr_t s_saved_peer;
static ble_addr_t s_repair_peer;
static enum peer_request s_pair_mode;
static int64_t s_pair_deadline_us;
static bool s_adv_pairing;
static ble_store_write_fn *s_write_bond;
static voice_frame_t s_rx_frame;
static bool s_assembling;
static int64_t s_rx_last_us;
static voice_frame_t s_tx_frame;
static size_t s_tx_offset;
static bool s_tx_active;
static struct ble_npl_event s_tx_event;
static struct ble_npl_event s_peer_event;
static struct ble_npl_callout s_tx_retry;
static struct ble_npl_callout s_pair_timeout;
static struct ble_npl_callout s_adv_retry;
static struct ble_npl_callout s_connection_guard;
static esp_timer_handle_t s_tx_guard;

void ble_store_config_init(void);
static void advertise(void);
static int gap_event(struct ble_gap_event *event, void *arg);

static bool same_identity(const ble_addr_t *first, const ble_addr_t *second)
{
    return (first->type & 1) == (second->type & 1) &&
        memcmp(first->val, second->val, sizeof(first->val)) == 0;
}

static bool pairing_open(void)
{
    return atomic_load(&s_pair_open) && esp_timer_get_time() < s_pair_deadline_us;
}

static bool pairing_allows(const ble_addr_t *peer)
{
    return pairing_open() && (s_pair_mode != PEER_REPAIR || same_identity(peer, &s_repair_peer));
}

/* New credential writes are limited to the locally authorized pairing window. */
static int write_bond(int type, const union ble_store_value *value)
{
    if ((type == BLE_STORE_OBJ_TYPE_OUR_SEC || type == BLE_STORE_OBJ_TYPE_PEER_SEC) &&
        !pairing_allows(&value->sec.peer_addr)) {
        return BLE_HS_EREJECT;
    }
    return s_write_bond(type, value);
}

static int keep_saved_bond(struct ble_store_status_event *event, void *arg)
{
    (void)arg;
    return event->event_code == BLE_STORE_EVENT_FULL ? 0 : BLE_HS_ENOMEM;
}

static bool valid_frame(const uint8_t *bytes, size_t length)
{
    return length >= 12 && length <= BLE_VOICE_MAX_FRAME_BYTES &&
        bytes[0] == 0xa5 && bytes[1] == 0xc3 &&
        (size_t)(bytes[6] | ((uint16_t)bytes[7] << 8)) + 12 == length;
}

static void clear_transport(void)
{
    atomic_store(&s_ready, false);
    atomic_fetch_add(&s_generation, 1);
    s_assembling = false;
    s_rx_frame.length = 0;
    s_tx_active = false;
    s_tx_offset = 0;
    xQueueReset(s_tx_queue);
    xQueueReset(s_rx_queue);
    ble_npl_callout_stop(&s_tx_retry);
    if (s_tx_guard) esp_timer_stop(s_tx_guard);
}

static void reset_connection(void)
{
    ble_npl_callout_stop(&s_connection_guard);
    clear_transport();
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_conn_closing = false;
    s_disconnect_attempts = 0;
    s_encrypted = false;
    s_subscribed = false;
    s_peer_approved = false;
}

static void refresh_ready(void)
{
    const bool ready = s_conn != BLE_HS_CONN_HANDLE_NONE && s_peer_approved && s_encrypted && s_subscribed;
    if (ready == atomic_load(&s_ready)) return;
    clear_transport();
    atomic_store(&s_ready, ready);
    if (ready && s_tx_guard) esp_timer_start_periodic(s_tx_guard, 20000);
    ESP_LOGI(TAG, "voice link %s (encrypted=%d subscribed=%d mtu=%u)",
             ready ? "ready" : "closed", s_encrypted, s_subscribed, s_mtu);
}

static void retry_tx(void)
{
    ble_npl_callout_reset(&s_tx_retry, ble_npl_time_ms_to_ticks32(5));
}

static void guard_transmit(void *arg)
{
    (void)arg;
    /* IDF's NPL event queue drops a post when full. A periodic source must
     * survive that case even if the frame queue is now full as well. */
    if (atomic_load(&s_ready)) {
        ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_event);
    }
}

/* Called only by the host event queue. Keep a failed fragment intact until the
 * mbuf/controller has room; never interleave fragments from different frames. */
static void transmit(struct ble_npl_event *event)
{
    (void)event;
    if (!atomic_load(&s_ready)) return;
    for (unsigned burst = 0; burst < 4; ++burst) {
        const uint32_t generation = atomic_load(&s_generation);
        if (!s_tx_active) {
            while (xQueueReceive(s_tx_queue, &s_tx_frame, 0) == pdTRUE) {
                if (s_tx_frame.generation == generation) {
                    s_tx_active = true;
                    s_tx_offset = 0;
                    break;
                }
            }
            if (!s_tx_active) return;
        }
        if (s_tx_frame.generation != generation) {
            s_tx_active = false;
            continue;
        }
        const bool audio = s_tx_frame.bytes[2] == AUDIO_UP_TYPE;
        if (esp_timer_get_time() - s_tx_frame.queued_us > (audio ? AUDIO_MAX_AGE_US : CONTROL_MAX_AGE_US)) {
            s_tx_active = false;
            if (!audio) {
                ESP_LOGW(TAG, "BLE control delivery timed out; closing voice session");
                clear_transport();
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                return;
            }
            atomic_fetch_add_explicit(&s_audio_tx_drops, 1, memory_order_relaxed);
            continue;
        }
        const size_t value_limit = s_mtu < PREFERRED_MTU ? s_mtu - 3 : MAX_FRAGMENT_VALUE;
        const size_t remaining = s_tx_frame.length - s_tx_offset;
        const size_t count = remaining < value_limit - 1 ? remaining : value_limit - 1;
        uint8_t fragment[MAX_FRAGMENT_VALUE];
        fragment[0] = (s_tx_offset == 0 ? FRAGMENT_FIRST : 0) |
            (count == remaining ? FRAGMENT_LAST : 0);
        memcpy(fragment + 1, s_tx_frame.bytes + s_tx_offset, count);
        struct os_mbuf *om = ble_hs_mbuf_from_flat(fragment, count + 1);
        if (om == NULL) {
            if (audio) atomic_fetch_add_explicit(&s_audio_tx_retries, 1, memory_order_relaxed);
            retry_tx();
            return;
        }
        /* notify_custom consumes om even on failure. */
        const int rc = ble_gatts_notify_custom(s_conn, s_tx_handle, om);
        if (rc != 0) {
            if (rc == BLE_HS_ENOMEM || rc == BLE_HS_EBUSY || rc == BLE_HS_EAGAIN) {
                if (audio) atomic_fetch_add_explicit(&s_audio_tx_retries, 1, memory_order_relaxed);
                retry_tx();
            } else if (!audio) {
                ESP_LOGW(TAG, "BLE control delivery failed rc=%d; closing voice session", rc);
                clear_transport();
                ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
            } else {
                ESP_LOGW(TAG, "notify failed rc=%d; dropping current frame", rc);
                atomic_fetch_add_explicit(&s_audio_tx_drops, 1, memory_order_relaxed);
                s_tx_active = false;
                retry_tx();
            }
            return;
        }
        s_tx_offset += count;
        if (s_tx_offset == s_tx_frame.length) s_tx_active = false;
    }
    if (s_tx_active || uxQueueMessagesWaiting(s_tx_queue)) retry_tx();
}

static int receive_fragment(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle;
    (void)arg;
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) return BLE_ATT_ERR_WRITE_NOT_PERMITTED;
    if (conn_handle != s_conn || !s_encrypted) return BLE_ATT_ERR_INSUFFICIENT_ENC;
    if (!atomic_load(&s_ready)) return BLE_ATT_ERR_UNLIKELY;
    uint8_t fragment[MAX_FRAGMENT_VALUE];
    uint16_t length = 0;
    const size_t wire_length = OS_MBUF_PKTLEN(ctxt->om);
    if (wire_length < 2 || wire_length > sizeof(fragment) ||
        ble_hs_mbuf_to_flat(ctxt->om, fragment, sizeof(fragment), &length) != 0) {
        s_assembling = false;
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    const uint8_t flags = fragment[0];
    const int64_t now = esp_timer_get_time();
    if ((flags & ~(FRAGMENT_FIRST | FRAGMENT_LAST)) != 0) {
        s_assembling = false;
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (s_assembling && now - s_rx_last_us > FRAGMENT_TIMEOUT_US) s_assembling = false;
    if (flags & FRAGMENT_FIRST) {
        s_assembling = true;
        s_rx_frame.length = 0;
        s_rx_frame.generation = atomic_load(&s_generation);
    }
    if (!s_assembling || s_rx_frame.length + length - 1 > BLE_VOICE_MAX_FRAME_BYTES) {
        s_assembling = false;
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    memcpy(s_rx_frame.bytes + s_rx_frame.length, fragment + 1, length - 1);
    s_rx_frame.length += length - 1;
    s_rx_last_us = now;
    if (s_rx_frame.length >= 12 &&
        (s_rx_frame.bytes[0] != 0xa5 || s_rx_frame.bytes[1] != 0xc3 ||
         (size_t)(s_rx_frame.bytes[6] | ((uint16_t)s_rx_frame.bytes[7] << 8)) + 12 > BLE_VOICE_MAX_FRAME_BYTES)) {
        s_assembling = false;
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    if (flags & FRAGMENT_LAST) {
        s_assembling = false;
        if (!valid_frame(s_rx_frame.bytes, s_rx_frame.length)) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (xQueueSend(s_rx_queue, &s_rx_frame, 0) != pdTRUE) {
            if (s_rx_frame.bytes[2] == AUDIO_DOWN_TYPE) {
                atomic_fetch_add_explicit(&s_audio_rx_drops, 1, memory_order_relaxed);
            }
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
    }
    return 0;
}

static int notification_access(uint16_t conn_handle, uint16_t attr_handle,
                               struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    /* NimBLE requires a callback even for notify-only value attributes. */
    return BLE_ATT_ERR_READ_NOT_PERMITTED;
}

static const struct ble_gatt_svc_def s_services[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_service_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_rx_uuid.u,
                .access_cb = receive_fragment,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_WRITE_ENC,
            },
            {
                .uuid = &s_tx_uuid.u,
                .access_cb = notification_access,
                .flags = BLE_GATT_CHR_F_NOTIFY | BLE_GATT_CHR_F_NOTIFY_INDICATE_ENC,
                .val_handle = &s_tx_handle,
            },
            {0}
        },
    },
    {0}
};

static void delay_advertising(uint32_t delay_ms)
{
    const int rc = ble_npl_callout_reset(&s_adv_retry, ble_npl_time_ms_to_ticks32(delay_ms));
    if (rc != 0) ESP_LOGE(TAG, "could not schedule advertising retry rc=%d", rc);
}

static void advertise(void)
{
    if (!atomic_load(&s_host_synced) || s_conn != BLE_HS_CONN_HANDLE_NONE) return;
    const bool pairing = pairing_open();
    const bool enabled = s_saved_peer_set || pairing;
    if (ble_gap_adv_active()) {
        if (enabled && s_adv_pairing == pairing) return;
        const int rc = ble_gap_adv_stop();
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "advertising stop failed rc=%d", rc);
            delay_advertising(ADV_RETRY_MS);
        } else if (enabled) {
            /* NimBLE requires the next start in a separate host event. */
            delay_advertising(ADV_REFRESH_MS);
        }
        return;
    }
    if (!enabled) return;
    /* Flags + 128-bit service fit in legacy ADV. Complete name is in scan response. */
    struct ble_hs_adv_fields fields = {0};
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.uuids128 = (ble_uuid128_t *)&s_service_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc == 0) {
        /* AD type 0x21 carries a little-endian service UUID followed by one state byte. */
        uint8_t pairing_data[sizeof(s_service_uuid.value) + 1];
        memcpy(pairing_data, s_service_uuid.value, sizeof(s_service_uuid.value));
        pairing_data[sizeof(s_service_uuid.value)] = pairing ? 1 : 0;
        struct ble_hs_adv_fields response = {0};
        response.name = (uint8_t *)"S3-Voice";
        response.name_len = strlen("S3-Voice");
        response.name_is_complete = 1;
        response.svc_data_uuid128 = pairing_data;
        response.svc_data_uuid128_len = sizeof(pairing_data);
        rc = ble_gap_adv_rsp_set_fields(&response);
    }
    if (rc == 0) {
        const struct ble_gap_adv_params params = {
            .conn_mode = BLE_GAP_CONN_MODE_UND,
            .disc_mode = BLE_GAP_DISC_MODE_GEN,
        };
        rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &params, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "advertising failed rc=%d", rc);
        delay_advertising(ADV_RETRY_MS);
    } else {
        s_adv_pairing = pairing;
        ESP_LOGI(TAG, "advertising for %s", pairing ? "local pairing" : "saved peer reconnect");
    }
}

static void retry_advertising(struct ble_npl_event *event)
{
    (void)event;
    advertise();
}

static void close_pairing(void)
{
    atomic_store(&s_pair_open, false);
    s_pair_mode = PEER_NONE;
    ble_hs_cfg.sm_bonding = 0;
    ble_npl_callout_stop(&s_pair_timeout);
}

static void watch_connection(uint32_t delay_ms)
{
    const int rc = ble_npl_callout_reset(&s_connection_guard, ble_npl_time_ms_to_ticks32(delay_ms));
    if (rc != 0) {
        ESP_LOGE(TAG, "could not schedule BLE connection watchdog rc=%d", rc);
        ble_hs_sched_reset(BLE_HS_ETIMEOUT);
    }
}

static void release_connection(void)
{
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    s_conn_closing = true;
    s_peer_approved = false;
    refresh_ready();
    ++s_disconnect_attempts;
    const int rc = ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    if (rc == BLE_HS_ENOTCONN) {
        reset_connection();
        advertise();
    } else {
        if (rc != 0 && rc != BLE_HS_EALREADY) {
            ESP_LOGW(TAG, "could not release BLE connection rc=%d attempt=%u", rc, s_disconnect_attempts);
        }
        watch_connection(DISCONNECT_GRACE_MS);
    }
}

static void guard_connection(struct ble_npl_event *event)
{
    (void)event;
    if (s_conn == BLE_HS_CONN_HANDLE_NONE) return;
    if (s_conn_closing && s_disconnect_attempts >= DISCONNECT_ATTEMPTS) {
        ESP_LOGW(TAG, "BLE disconnect did not complete, resynchronizing host");
        ble_hs_sched_reset(BLE_HS_ETIMEOUT);
    } else if (s_conn_closing || !s_peer_approved) {
        if (!s_conn_closing) ESP_LOGW(TAG, "encrypted reconnect timed out");
        release_connection();
    }
}

static void expire_pairing(struct ble_npl_event *event)
{
    (void)event;
    if (!atomic_load(&s_pair_open)) return;
    close_pairing();
    if (s_conn != BLE_HS_CONN_HANDLE_NONE && !s_peer_approved && !s_conn_closing) {
        release_connection();
    }
    advertise();
    ESP_LOGI(TAG, "BLE pairing window closed");
}

static void handle_peer_request(struct ble_npl_event *event)
{
    (void)event;
    const enum peer_request request = atomic_exchange(&s_peer_request, PEER_NONE);
    if (request == PEER_NONE || !atomic_load(&s_host_synced) || atomic_load(&s_pair_open) ||
        (request != PEER_PAIR) != s_saved_peer_set) return;
    if (request == PEER_RECONNECT) {
        if (s_conn == BLE_HS_CONN_HANDLE_NONE) advertise();
        else if (!atomic_load(&s_ready)) release_connection();
        return;
    }
    const int rc = ble_npl_callout_reset(&s_pair_timeout, ble_npl_time_ms_to_ticks32(PAIRING_WINDOW_MS));
    if (rc != 0) {
        ESP_LOGE(TAG, "could not schedule pairing window rc=%d", rc);
        return;
    }
    /* This event is generated by the touch screen, never by a remote peer. */
    if (request == PEER_REPLACE) {
        const int unpair_rc = ble_gap_unpair(&s_saved_peer);
        if (unpair_rc != 0 && unpair_rc != BLE_HS_ENOENT) {
            ESP_LOGE(TAG, "could not forget previous BLE peer rc=%d", unpair_rc);
            ble_npl_callout_stop(&s_pair_timeout);
            return;
        }
        s_saved_peer_set = false;
    }
    if (request == PEER_REPAIR) s_repair_peer = s_saved_peer;
    s_pair_mode = request;
    s_pair_deadline_us = esp_timer_get_time() + PAIRING_WINDOW_MS * INT64_C(1000);
    ble_hs_cfg.sm_bonding = 1;
    atomic_store(&s_pair_open, true);
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
        release_connection();
    } else {
        advertise();
    }
    ESP_LOGI(TAG, "BLE %s is open for %d seconds", request == PEER_REPAIR ? "pairing repair" : "pairing",
             PAIRING_WINDOW_MS / 1000);
}

static int repeat_pairing(const struct ble_gap_repeat_pairing *request)
{
    struct ble_gap_conn_desc desc;
    if (s_pair_mode != PEER_REPAIR || !pairing_open() || !s_saved_peer_set ||
        s_peer_approved || s_conn_closing || request->conn_handle != s_conn ||
        !request->new_bonding || request->new_key_size < request->cur_key_size ||
        (request->cur_sc && !request->new_sc) || (request->cur_authenticated && !request->new_authenticated) ||
        ble_gap_conn_find(s_conn, &desc) != 0 || !same_identity(&desc.peer_id_addr, &s_repair_peer)) {
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    /* Drop only the conflicting bond and leave SMP running on this connection. */
    const int rc = ble_store_util_delete_peer(&s_saved_peer);
    if (rc != 0) {
        ble_addr_t peer;
        int count = 0;
        if (ble_store_util_bonded_peers(&peer, &count, 1) == 0 && count == 0) s_saved_peer_set = false;
        ESP_LOGW(TAG, "could not remove conflicting BLE bond rc=%d", rc);
        release_connection();
        return BLE_GAP_REPEAT_PAIRING_IGNORE;
    }
    s_saved_peer_set = false;
    ESP_LOGI(TAG, "removed conflicting BLE bond for authorized repair");
    return BLE_GAP_REPEAT_PAIRING_RETRY;
}

static void log_connection_params(const char *phase, const struct ble_gap_conn_desc *desc)
{
    ESP_LOGI(TAG, "connection %s: interval=%u us latency=%u supervision=%u ms",
             phase, (unsigned)desc->conn_itvl * 1250u, (unsigned)desc->conn_latency,
             (unsigned)desc->supervision_timeout * 10u);
}

static void request_voice_link_params(void)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = 12, .itvl_max = 24, .latency = 0,
        .supervision_timeout = 400, .min_ce_len = 0, .max_ce_len = 0,
    };
    const int rc = ble_gap_update_params(s_conn, &params);
    if (rc != 0) ESP_LOGW(TAG, "connection parameter request rc=%d", rc);
}

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    struct ble_gap_conn_desc desc;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status != 0) {
            ESP_LOGW(TAG, "connection attempt failed status=%d", event->connect.status);
            advertise();
            break;
        }
        if (s_conn != BLE_HS_CONN_HANDLE_NONE) {
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        if (ble_gap_conn_find(event->connect.conn_handle, &desc) != 0 ||
            (!pairing_allows(&desc.peer_id_addr) &&
             (!s_saved_peer_set || !same_identity(&desc.peer_id_addr, &s_saved_peer)))) {
            ESP_LOGW(TAG, "connection rejected before encryption: no matching saved peer or local pairing window");
            ble_gap_terminate(event->connect.conn_handle, BLE_ERR_REM_USER_CONN_TERM);
            break;
        }
        reset_connection();
        s_conn = event->connect.conn_handle;
        s_mtu = 23;
        log_connection_params("established", &desc);
        if (!pairing_open()) watch_connection(RECONNECT_SECURITY_MS);
        {
            const int rc = ble_gap_security_initiate(s_conn);
            if (rc != 0 && rc != BLE_HS_EALREADY) {
                ESP_LOGW(TAG, "security request failed rc=%d", rc);
                release_connection();
            }
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        if (event->disconnect.conn.conn_handle != s_conn) {
            advertise();
            break;
        }
        ESP_LOGI(TAG, "central disconnected reason=%d, saved_peer=%d", event->disconnect.reason,
                 atomic_load(&s_saved_peer_set));
        reset_connection();
        advertise();
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        if (event->conn_update.conn_handle != s_conn) break;
        if (event->conn_update.status != 0) {
            ESP_LOGW(TAG, "connection parameter update status=%d", event->conn_update.status);
        } else if (ble_gap_conn_find(s_conn, &desc) == 0) {
            log_connection_params("updated", &desc);
        }
        break;
    case BLE_GAP_EVENT_DATA_LEN_CHG:
        if (event->data_len_chg.conn_handle == s_conn) {
            ESP_LOGI(TAG, "data length updated: tx=%u B/%u us rx=%u B/%u us",
                     (unsigned)event->data_len_chg.max_tx_octets,
                     (unsigned)event->data_len_chg.max_tx_time,
                     (unsigned)event->data_len_chg.max_rx_octets,
                     (unsigned)event->data_len_chg.max_rx_time);
        }
        break;
    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        if (event->phy_updated.conn_handle == s_conn) {
            if (event->phy_updated.status != 0) {
                ESP_LOGW(TAG, "PHY update status=%d", event->phy_updated.status);
            } else {
                ESP_LOGI(TAG, "PHY updated: tx=%u rx=%u (1=1M, 2=2M, 3=Coded)",
                         (unsigned)event->phy_updated.tx_phy, (unsigned)event->phy_updated.rx_phy);
            }
        }
        break;
    case BLE_GAP_EVENT_ENC_CHANGE: {
        if (event->enc_change.conn_handle != s_conn || s_conn_closing) break;
        const bool have_peer = ble_gap_conn_find(s_conn, &desc) == 0;
        const bool was_approved = s_peer_approved;
        s_encrypted = event->enc_change.status == 0 && have_peer && desc.sec_state.encrypted && desc.sec_state.bonded;
        s_peer_approved = false;
        if (!s_saved_peer_set && s_encrypted && pairing_allows(&desc.peer_id_addr)) {
            ble_addr_t peer;
            int count = 0;
            if (ble_store_util_bonded_peers(&peer, &count, 1) == 0 && count == 1 &&
                same_identity(&peer, &desc.peer_id_addr)) {
                s_saved_peer = peer;
                s_saved_peer_set = true;
                close_pairing();
                ESP_LOGI(TAG, "BLE peer approved and saved");
            }
        }
        if (s_encrypted && s_saved_peer_set && same_identity(&desc.peer_id_addr, &s_saved_peer)) {
            s_peer_approved = true;
            ble_npl_callout_stop(&s_connection_guard);
            /* Windows can already be exchanging keys when it opens the ACL. */
            if (!was_approved) request_voice_link_params();
        }
        refresh_ready();
        if (!s_peer_approved) {
            ESP_LOGW(TAG, "BLE link authorization failed status=%d encrypted=%d bonded=%d saved=%d pairing=%d",
                     event->enc_change.status, have_peer && desc.sec_state.encrypted,
                     have_peer && desc.sec_state.bonded, atomic_load(&s_saved_peer_set), pairing_open());
            release_connection();
        }
        break;
    }
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.conn_handle == s_conn && event->subscribe.attr_handle == s_tx_handle) {
            s_subscribed = event->subscribe.cur_notify;
            refresh_ready();
        }
        break;
    case BLE_GAP_EVENT_MTU:
        if (event->mtu.conn_handle == s_conn) {
            s_mtu = event->mtu.value;
            ESP_LOGI(TAG, "MTU=%u", s_mtu);
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        advertise();
        break;
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        return repeat_pairing(&event->repeat_pairing);
    default:
        break;
    }
    return 0;
}

static void on_reset(int reason)
{
    atomic_store(&s_host_synced, false);
    atomic_store(&s_peer_request, PEER_NONE);
    ble_npl_callout_stop(&s_adv_retry);
    close_pairing();
    reset_connection();
    ESP_LOGW(TAG, "NimBLE host reset reason=%d", reason);
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &s_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "BLE identity unavailable rc=%d", rc);
        return;
    }
    int count = 0;
    rc = ble_store_util_bonded_peers(&s_saved_peer, &count, 1);
    s_saved_peer_set = rc == 0 && count == 1;
    if (rc != 0) ESP_LOGW(TAG, "could not read saved BLE peer rc=%d", rc);
    else ESP_LOGI(TAG, "saved BLE peer %s", count == 1 ? "restored" : "not configured");
    atomic_store(&s_host_synced, true);
    advertise();
}

static void host_task(void *arg)
{
    (void)arg;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_voice_init(void)
{
    if (atomic_load(&s_initialized)) return ESP_ERR_INVALID_STATE;
    /* Full frame copies live in PSRAM; NimBLE mbufs/controller remain internal. */
    s_tx_queue = xQueueCreateWithCaps(QUEUE_FRAMES, sizeof(voice_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_rx_queue = xQueueCreateWithCaps(QUEUE_FRAMES, sizeof(voice_frame_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_tx_queue == NULL || s_rx_queue == NULL) {
        if (s_tx_queue) vQueueDeleteWithCaps(s_tx_queue);
        if (s_rx_queue) vQueueDeleteWithCaps(s_rx_queue);
        s_tx_queue = s_rx_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) goto fail;
    ble_npl_event_init(&s_tx_event, transmit, NULL);
    ble_npl_event_init(&s_peer_event, handle_peer_request, NULL);
    if (ble_npl_callout_init(&s_tx_retry, nimble_port_get_dflt_eventq(), transmit, NULL) != 0) {
        nimble_port_deinit();
        err = ESP_ERR_NO_MEM;
        goto fail;
    }
    if (ble_npl_callout_init(&s_pair_timeout, nimble_port_get_dflt_eventq(), expire_pairing, NULL) != 0) {
        err = ESP_ERR_NO_MEM;
        goto fail_tx_retry;
    }
    if (ble_npl_callout_init(&s_adv_retry, nimble_port_get_dflt_eventq(), retry_advertising, NULL) != 0) {
        err = ESP_ERR_NO_MEM;
        goto fail_pair_timeout;
    }
    if (ble_npl_callout_init(&s_connection_guard, nimble_port_get_dflt_eventq(), guard_connection, NULL) != 0) {
        err = ESP_ERR_NO_MEM;
        goto fail_adv_retry;
    }
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.store_status_cb = keep_saved_bond;
    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding = 0;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_svc_gap_device_name_set("S3-Voice");
    if (rc == 0) rc = ble_att_set_preferred_mtu(PREFERRED_MTU);
    if (rc == 0) rc = ble_gatts_count_cfg(s_services);
    if (rc == 0) rc = ble_gatts_add_svcs(s_services);
    if (rc != 0) {
        ESP_LOGE(TAG, "GATT setup failed rc=%d", rc);
        err = ESP_FAIL;
        goto fail_connection_guard;
    }
    const esp_timer_create_args_t guard = {
        .callback = guard_transmit,
        .name = "ble_tx_guard",
        .skip_unhandled_events = true,
    };
    err = esp_timer_create(&guard, &s_tx_guard);
    if (err != ESP_OK) goto fail_connection_guard;
    ble_store_config_init();
    s_write_bond = ble_hs_cfg.store_write_cb;
    ble_hs_cfg.store_write_cb = write_bond;
    err = esp_nimble_enable(host_task);
    if (err != ESP_OK) {
        esp_timer_delete(s_tx_guard);
        s_tx_guard = NULL;
        goto fail_connection_guard;
    }
    atomic_store(&s_initialized, true);
    ESP_LOGI(TAG, "initialized S3-Voice; encrypted Opus transport, MTU preference %d", PREFERRED_MTU);
    return ESP_OK;
fail_connection_guard:
    ble_npl_callout_deinit(&s_connection_guard);
fail_adv_retry:
    ble_npl_callout_deinit(&s_adv_retry);
fail_pair_timeout:
    ble_npl_callout_deinit(&s_pair_timeout);
fail_tx_retry:
    ble_npl_callout_deinit(&s_tx_retry);
    nimble_port_deinit();
fail:
    vQueueDeleteWithCaps(s_tx_queue);
    vQueueDeleteWithCaps(s_rx_queue);
    s_tx_queue = s_rx_queue = NULL;
    return err;
}

static bool request_peer_action(enum peer_request request)
{
    if (!atomic_load(&s_initialized) || !atomic_load(&s_host_synced) || atomic_load(&s_pair_open)) return false;
    if ((request != PEER_PAIR) != atomic_load(&s_saved_peer_set)) return false;
    int expected = PEER_NONE;
    if (!atomic_compare_exchange_strong(&s_peer_request, &expected, request)) return false;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_peer_event);
    return true;
}

bool ble_voice_open_pairing(bool replace_saved_peer)
{
    return request_peer_action(replace_saved_peer ? PEER_REPLACE : PEER_PAIR);
}

bool ble_voice_repair_pairing(void)
{
    return request_peer_action(PEER_REPAIR);
}

bool ble_voice_retry_connection(void)
{
    return request_peer_action(PEER_RECONNECT);
}

bool ble_voice_is_pairing(void)
{
    /* Repost on UI refresh if the bounded NimBLE event queue lost a request. */
    if (atomic_load(&s_peer_request) != PEER_NONE) ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_peer_event);
    return atomic_load(&s_pair_open);
}

bool ble_voice_has_saved_peer(void)
{
    return atomic_load(&s_saved_peer_set);
}

bool ble_voice_is_connected(void)
{
    return atomic_load(&s_ready);
}

ble_voice_audio_drops_t ble_voice_audio_drops(void)
{
    return (ble_voice_audio_drops_t){
        .tx = atomic_load_explicit(&s_audio_tx_drops, memory_order_relaxed),
        .rx = atomic_load_explicit(&s_audio_rx_drops, memory_order_relaxed),
        .tx_admission_drops = atomic_load_explicit(&s_audio_tx_admission_drops, memory_order_relaxed),
        .tx_retries = atomic_load_explicit(&s_audio_tx_retries, memory_order_relaxed),
    };
}

esp_err_t ble_voice_send(const void *frame, size_t length, TickType_t timeout)
{
    if (frame == NULL || !valid_frame(frame, length)) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_initialized) || !atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    const bool audio = ((const uint8_t *)frame)[2] == AUDIO_UP_TYPE;
    if (audio && uxQueueSpacesAvailable(s_tx_queue) <= RESERVED_CONTROL_FRAMES) {
        atomic_fetch_add_explicit(&s_audio_tx_admission_drops, 1, memory_order_relaxed);
        return ESP_ERR_TIMEOUT;
    }
    voice_frame_t item = {0};
    item.queued_us = esp_timer_get_time();
    item.generation = atomic_load(&s_generation);
    item.length = length;
    memcpy(item.bytes, frame, length);
    if (xQueueSend(s_tx_queue, &item, timeout) != pdTRUE) {
        if (audio) atomic_fetch_add_explicit(&s_audio_tx_admission_drops, 1, memory_order_relaxed);
        return ESP_ERR_TIMEOUT;
    }
    if (!atomic_load(&s_ready) || item.generation != atomic_load(&s_generation)) return ESP_ERR_INVALID_STATE;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &s_tx_event);
    return ESP_OK;
}

esp_err_t ble_voice_receive(void *frame, size_t capacity, size_t *length, TickType_t timeout)
{
    if (length) *length = 0;
    if (frame == NULL || length == NULL || capacity < BLE_VOICE_MAX_FRAME_BYTES) return ESP_ERR_INVALID_ARG;
    if (!atomic_load(&s_initialized) || !atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    voice_frame_t item;
    const TickType_t started = xTaskGetTickCount();
    TickType_t remaining = timeout;
    for (;;) {
        if (xQueueReceive(s_rx_queue, &item, remaining) != pdTRUE) return ESP_ERR_TIMEOUT;
        if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
        if (item.generation == atomic_load(&s_generation)) {
            memcpy(frame, item.bytes, item.length);
            *length = item.length;
            return ESP_OK;
        }
        if (timeout != portMAX_DELAY) {
            const TickType_t elapsed = xTaskGetTickCount() - started;
            if (elapsed >= timeout) return ESP_ERR_TIMEOUT;
            remaining = timeout - elapsed;
        }
    }
}

#else

/* Existing sdkconfig files can explicitly disable Bluetooth. Keep UDP usable. */
esp_err_t ble_voice_init(void) { return ESP_ERR_NOT_SUPPORTED; }
bool ble_voice_open_pairing(bool replace_saved_peer) { (void)replace_saved_peer; return false; }
bool ble_voice_repair_pairing(void) { return false; }
bool ble_voice_retry_connection(void) { return false; }
bool ble_voice_is_pairing(void) { return false; }
bool ble_voice_has_saved_peer(void) { return false; }
bool ble_voice_is_connected(void) { return false; }
ble_voice_audio_drops_t ble_voice_audio_drops(void) { return (ble_voice_audio_drops_t){0}; }

esp_err_t ble_voice_send(const void *frame, size_t length, TickType_t timeout)
{
    (void)frame;
    (void)length;
    (void)timeout;
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t ble_voice_receive(void *frame, size_t capacity, size_t *length, TickType_t timeout)
{
    (void)frame;
    (void)capacity;
    (void)timeout;
    if (length != NULL) *length = 0;
    return ESP_ERR_NOT_SUPPORTED;
}

#endif
