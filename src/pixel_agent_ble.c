/**
 * @file pixel_agent_ble.c
 * @brief BLE line transport for agent monitor (S:/P:/B:/K:/T: protocol)
 * @version 1.1
 * @date 2026-06-12
 * @copyright Copyright (c) Tuya Inc.
 *
 * Uses Tuya command service (FD50) notify/write alongside ble_mgr.
 * PC writes ASCII lines to Tuya write char; device notifies on Tuya notify char.
 */
#include "pixel_agent_ble.h"
#include "pixel_agent_bridge.h"
#include "pixel_agent_clawd.h"
#include "ble_hs.h"

extern int tuya_ble_adv_update(void);
extern void tuya_ble_agent_keep_connection(void);
#include "tal_bluetooth.h"
#include "tal_bluetooth_def.h"
#include "tal_log.h"
#include "tal_memory.h"
#include "tal_mutex.h"
#include "tal_workq_service.h"
#include <string.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define AGENT_BLE_MTU_DEFAULT    23
#define AGENT_BLE_MTU_MAX        247
#define AGENT_BLE_RX_JOB_MAX     128

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC volatile BOOL_T s_ble_connected = FALSE;
STATIC volatile BOOL_T s_ble_subscribed = FALSE;
STATIC volatile BOOL_T s_ble_transport_active = FALSE;
STATIC volatile UINT16_T s_ble_mtu = AGENT_BLE_MTU_DEFAULT;
STATIC volatile UINT16_T s_tuya_write_handle = 0;
STATIC volatile UINT16_T s_tuya_notify_handle = 0;
STATIC volatile UINT16_T s_nus_notify_handle = 0;
STATIC TAL_BLE_PEER_INFO_T s_ble_peer = {0};
STATIC MUTEX_HANDLE s_tx_mutex = NULL;
STATIC volatile BOOL_T s_ble_chime_played = FALSE;
STATIC volatile BOOL_T s_ble_host_linked = FALSE;

typedef struct {
    UINT16_T len;
    UINT8_T data[AGENT_BLE_RX_JOB_MAX];
} AGENT_BLE_RX_JOB_T;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Work-queue job: deliver agent line to bridge parser (main thread)
 * @param[in] arg heap AGENT_BLE_RX_JOB_T
 * @return none
 */
STATIC VOID_T __agent_rx_job(VOID_T *arg)
{
    AGENT_BLE_RX_JOB_T *job = (AGENT_BLE_RX_JOB_T *)arg;

    if (job == NULL) {
        return;
    }
    if (job->len > 0) {
        pixel_agent_bridge_feed_rx(job->data, job->len);
    }
    tal_free(job);
}
/**
 * @brief Check if payload is agent line protocol from PC (S: or P:)
 * @param[in] data payload bytes
 * @param[in] len byte count
 * @return TRUE if agent command line
 */
STATIC BOOL_T __is_agent_line(CONST UINT8_T *data, UINT32_T len)
{
    if (data == NULL || len < 3) {
        return FALSE;
    }
    if (data[1] != ':') {
        return FALSE;
    }
    return (data[0] == 'S' || data[0] == 'P' || data[0] == 'L');
}

/**
 * @brief Chunked notify on Tuya command characteristic
 * @param[in] data payload bytes
 * @param[in] len byte count
 * @return OPRT_OK if any chunk sent
 */
STATIC OPERATE_RET __tuya_notify_chunks(CONST UINT8_T *data, UINT32_T len)
{
    UINT32_T offset = 0;
    UINT16_T chunk_max;
    OPERATE_RET rt = OPRT_RESOURCE_NOT_READY;
    TAL_BLE_DATA_T pkt;

    if (!s_ble_connected || !s_ble_subscribed || data == NULL || len == 0) {
        return OPRT_RESOURCE_NOT_READY;
    }

    chunk_max = (s_ble_mtu > 3) ? (UINT16_T)(s_ble_mtu - 3) : 20;

    tal_mutex_lock(s_tx_mutex);
    while (offset < len) {
        UINT32_T remain = len - offset;
        UINT16_T chunk = (remain > chunk_max) ? chunk_max : (UINT16_T)remain;

        pkt.len = chunk;
        pkt.p_data = (UINT8_T *)(data + offset);
        if (tal_ble_server_common_send(&pkt) == OPRT_OK ||
            tal_ble_server_nus_send(&pkt) == OPRT_OK) {
            rt = OPRT_OK;
        }
        offset += chunk;
    }
    tal_mutex_unlock(s_tx_mutex);
    return rt;
}

/**
 * @brief Apply clawd blue theme from host-link latch and GATT subscribe state
 * @return none
 */
STATIC VOID_T __apply_ble_display_theme(VOID_T)
{
    pixel_agent_clawd_set_ble_theme(s_ble_host_linked || pixel_agent_ble_is_connected());
}

/**
 * @brief Handle TAL BLE events forwarded from ble_mgr hook
 * @param[in] msg BLE event
 * @return none
 */
STATIC VOID_T __on_ble_event(TAL_BLE_EVT_PARAMS_T *msg)
{
    if (msg == NULL) {
        return;
    }

    switch (msg->type) {
    case TAL_BLE_EVT_PERIPHERAL_CONNECT:
        s_ble_connected = (msg->ble_event.connect.result == 0);
        s_ble_subscribed = FALSE;
        s_ble_mtu = AGENT_BLE_MTU_DEFAULT;
        s_ble_peer = msg->ble_event.connect.peer;
        s_tuya_write_handle = msg->ble_event.connect.peer.char_handle[TAL_COMMON_WRITE_CHAR_INDEX];
        s_tuya_notify_handle = msg->ble_event.connect.peer.char_handle[TAL_COMMON_NOTIFY_CHAR_INDEX];
        s_nus_notify_handle = msg->ble_event.connect.peer.char_handle[TAL_NUS_NOTIFY_CHAR_INDEX];
        tuya_ble_agent_keep_connection();
        if (s_ble_connected) {
            s_ble_transport_active = TRUE;
        }
        PR_NOTICE("agent ble: connected (fd50 wr=0x%04x notify=0x%04x nus=0x%04x)",
                  (unsigned)s_tuya_write_handle, (unsigned)s_tuya_notify_handle,
                  (unsigned)s_nus_notify_handle);
        break;

    case TAL_BLE_EVT_DISCONNECT:
        s_ble_connected = FALSE;
        s_ble_subscribed = FALSE;
        s_ble_transport_active = FALSE;
        s_ble_chime_played = FALSE;
        s_ble_host_linked = FALSE;
        s_ble_mtu = AGENT_BLE_MTU_DEFAULT;
        pixel_agent_clawd_set_ble_theme(FALSE);
        PR_NOTICE("agent ble: disconnected");
        break;

    case TAL_BLE_EVT_MTU_REQUEST: {
        UINT16_T cli_mtu = msg->ble_event.exchange_mtu.mtu;
        s_ble_mtu = (cli_mtu < AGENT_BLE_MTU_MAX) ? cli_mtu : AGENT_BLE_MTU_MAX;
        if (s_ble_mtu < AGENT_BLE_MTU_DEFAULT) {
            s_ble_mtu = AGENT_BLE_MTU_DEFAULT;
        }
        PR_NOTICE("agent ble: MTU %u", (unsigned)s_ble_mtu);
        tuya_ble_agent_keep_connection();
        break;
    }

    case TAL_BLE_EVT_SUBSCRIBE: {
        UINT16_T ch = msg->ble_event.subscribe.char_handle;
        BOOL_T cur_notify = msg->ble_event.subscribe.cur_notify ? TRUE : FALSE;
        BOOL_T is_write = (s_tuya_write_handle != 0 && ch == s_tuya_write_handle);

        /* Mac may report CCCD attr_handle, not chr value handle — accept any notify sub */
        if (is_write) {
            break;
        }
        s_ble_subscribed = cur_notify;
        if (cur_notify) {
            s_ble_transport_active = TRUE;
            tuya_ble_agent_keep_connection();
            PR_NOTICE("agent ble: notify enabled (handle=0x%04x, notify=0x%04x)",
                      (unsigned)ch, (unsigned)s_tuya_notify_handle);
            s_ble_host_linked = TRUE;
            __apply_ble_display_theme();
            pixel_agent_ble_on_link_ready();
        } else {
            PR_NOTICE("agent ble: notify disabled (handle=0x%04x)", (unsigned)ch);
            s_ble_host_linked = FALSE;
            if (!s_ble_connected) {
                s_ble_transport_active = FALSE;
            }
            __apply_ble_display_theme();
        }
        break;
    }

    case TAL_BLE_EVT_WRITE_REQ: {
        UINT16_T wr = msg->ble_event.write_report.peer.char_handle[0];
        if (s_tuya_write_handle == 0 && wr != 0) {
            s_tuya_write_handle = wr;
        }
        break;
    }

    default:
        break;
    }
}

/**
 * @brief ble_mgr weak hook — receives all TAL BLE events after core handling
 * @param[in] msg BLE event copy
 * @return none
 */
void tal_ble_agent_event_hook(TAL_BLE_EVT_PARAMS_T *msg)
{
    __on_ble_event(msg);
}

/**
 * @brief ble_mgr passthrough for raw S:/P: agent lines on Tuya write char
 * @param[in] data write payload
 * @param[in] len byte count
 * @return none
 */
void tuya_ble_agent_line_passthrough(const uint8_t *data, uint16_t len)
{
    AGENT_BLE_RX_JOB_T *job;
    UINT16_T copy_len;

    if (data == NULL || len == 0) {
        return;
    }
    if (data[0] == 'I' && len >= 2 && data[1] == (uint8_t)':') {
        pixel_agent_ble_send_identity();
        return;
    }
    if (!__is_agent_line(data, len)) {
        return;
    }
    if (!s_ble_connected) {
        s_ble_connected = TRUE;
    }
    tuya_ble_agent_keep_connection();
    s_ble_transport_active = TRUE;

    if (data[0] == 'L' && len >= 3 && data[1] == ':') {
        BOOL_T linked = (data[2] == (uint8_t)'1') ? TRUE : FALSE;
        PR_NOTICE("agent ble: passthrough L:%c", linked ? '1' : '0');
        pixel_agent_ble_on_host_link(linked);
        return;
    }

    s_ble_host_linked = TRUE;
    __apply_ble_display_theme();
    PR_NOTICE("agent ble: RX %.*s", (int)(len > 32 ? 32 : len), (const char *)data);

    copy_len = (len > AGENT_BLE_RX_JOB_MAX) ? AGENT_BLE_RX_JOB_MAX : len;
    job = (AGENT_BLE_RX_JOB_T *)tal_malloc(sizeof(AGENT_BLE_RX_JOB_T));
    if (job == NULL) {
        pixel_agent_bridge_feed_rx(data, len);
        return;
    }
    memcpy(job->data, data, copy_len);
    job->len = copy_len;
    if (tal_workq_schedule(WORKQ_SYSTEM, __agent_rx_job, job) != OPRT_OK) {
        tal_free(job);
        pixel_agent_bridge_feed_rx(data, len);
    }
}

/**
 * @brief ble_mgr weak hook — keep advertising after MQTT so Mac can connect
 * @return TRUE
 */
bool tal_ble_agent_keep_adv(void)
{
    return true;
}

VOID_T pixel_agent_ble_configure_security(VOID_T)
{
    tuya_ble_hs_cfg.sm_bonding = 0;
    tuya_ble_hs_cfg.sm_mitm = 0;
    tuya_ble_hs_cfg.sm_sc = 0;
    tuya_ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    PR_NOTICE("agent ble: open security (no bond / no passkey)");
}

VOID_T pixel_agent_ble_on_cloud_online(VOID_T)
{
    pixel_agent_ble_apply_unique_adv_name();
    tuya_ble_adv_update();
    PR_NOTICE("agent ble: adv refresh after cloud online");
}

OPERATE_RET pixel_agent_ble_init(VOID_T)
{
    OPERATE_RET rt;

    s_ble_connected = FALSE;
    s_ble_subscribed = FALSE;
    s_ble_mtu = AGENT_BLE_MTU_DEFAULT;
    s_tuya_write_handle = 0;
    s_tuya_notify_handle = 0;
    s_nus_notify_handle = 0;

    rt = tal_mutex_create_init(&s_tx_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("agent ble: mutex create failed %d", rt);
        return rt;
    }

    tuya_ble_adv_update();
    PR_NOTICE("agent ble: Tuya FD50 hook ready (adv on for agent)");
    return OPRT_OK;
}

BOOL_T pixel_agent_ble_is_connected(VOID_T)
{
    return s_ble_connected && s_ble_subscribed;
}

BOOL_T pixel_agent_ble_transport_active(VOID_T)
{
    return s_ble_transport_active || (s_ble_connected && s_ble_subscribed);
}

BOOL_T pixel_agent_ble_link_up(VOID_T)
{
    return s_ble_connected;
}

BOOL_T pixel_agent_ble_take_link_chime(VOID_T)
{
    if (s_ble_chime_played) {
        return FALSE;
    }
    s_ble_chime_played = TRUE;
    return TRUE;
}

BOOL_T pixel_agent_ble_should_use_blue_theme(VOID_T)
{
    return s_ble_host_linked || pixel_agent_ble_is_connected();
}

VOID_T pixel_agent_ble_sync_display_theme(VOID_T)
{
    __apply_ble_display_theme();
}

VOID_T pixel_agent_ble_on_host_link(BOOL_T linked)
{
    if (linked) {
        s_ble_host_linked = TRUE;
        __apply_ble_display_theme();
        PR_NOTICE("agent ble: host link L:1");
        return;
    }
    s_ble_host_linked = FALSE;
    __apply_ble_display_theme();
    PR_NOTICE("agent ble: host link L:0");
}

VOID_T pixel_agent_ble_on_serial_activity(VOID_T)
{
    if (pixel_agent_ble_link_up() || pixel_agent_ble_should_use_blue_theme()) {
        /* BLE up or host sent L:1 — ignore stale serial S:/P: */
        return;
    }
    s_ble_transport_active = FALSE;
    pixel_agent_clawd_set_ble_theme(FALSE);
}

OPERATE_RET pixel_agent_ble_send(CONST UINT8_T *data, UINT32_T len)
{
    return __tuya_notify_chunks(data, len);
}
