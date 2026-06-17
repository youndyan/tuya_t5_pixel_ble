/**
 * @file pixel_agent_bridge.c
 * @brief USB serial protocol for PC pixel-agent-bridge
 * @version 1.0
 * @date 2026-06-04
 * @copyright Copyright (c) Tuya Inc.
 *
 * PC -> device (line terminated with \\n):
 *   S:idle | S:thinking | S:working | S:juggling | S:notify | S:error | S:happy
 *   P:tool=Bash          permission pending (optional tool name after =)
 *   P:                   clear permission UI
 *
 * Device -> PC:
 *   B:deny\\n  B:allow\\n  B:always\\n
 *   K:clear\\n  K:backspace\\n  K:enter\\n  T:<text>\\n
 */
#include "pixel_agent_bridge.h"
#include "pixel_agent_ble.h"
#include "tal_uart.h"
#include "tal_api.h"
#include <string.h>
#include <stdio.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define AGENT_UART_NUM           TUYA_UART_NUM_0
#define AGENT_UART_BAUD          115200
#define AGENT_RX_BUF_SIZE        512
#define AGENT_RX_DRAIN_MAX       8
#define AGENT_LINE_BUF_SIZE      128
#define AGENT_PERM_TOOL_MAX      16
#define AGENT_BTN_LINE_MAX       16

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC BOOL_T s_uart_ready = FALSE;
STATIC CHAR_T s_line_buf[AGENT_LINE_BUF_SIZE];
STATIC UINT32_T s_line_pos = 0;
STATIC PIXEL_AGENT_VIS_E s_vis_state = PIXEL_AGENT_VIS_IDLE;
STATIC BOOL_T s_perm_pending = FALSE;
STATIC CHAR_T s_perm_tool[AGENT_PERM_TOOL_MAX] = {0};
STATIC BOOL_T s_voice_recording = FALSE;
STATIC BOOL_T s_rx_from_serial = FALSE;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
STATIC VOID_T __transport_send(CONST UINT8_T *data, UINT32_T len)
{
    BOOL_T ble_ok = FALSE;

    if (data == NULL || len == 0) {
        return;
    }
    if (pixel_agent_ble_is_connected()) {
        if (pixel_agent_ble_send(data, len) == OPRT_OK) {
            ble_ok = TRUE;
        }
    }
    /* Serial fallback only when BLE notify is unavailable */
    if (s_uart_ready && !ble_ok) {
        tal_uart_write(AGENT_UART_NUM, data, len);
    } else if (!ble_ok && !s_uart_ready) {
        PR_WARN("agent bridge: uplink dropped (no BLE notify, no UART)");
    }
}

/**
 * @brief Send one line to PC bridge
 * @param[in] line line without trailing newline
 * @return none
 */
STATIC VOID_T __send_line(CONST CHAR_T *line)
{
    CHAR_T buf[AGENT_BTN_LINE_MAX];

    if (line == NULL) {
        return;
    }
    if (!pixel_agent_ble_is_connected() && !s_uart_ready) {
        return;
    }
    snprintf(buf, sizeof(buf), "%s\n", line);
    __transport_send((CONST UINT8_T *)buf, (UINT32_T)strlen(buf));
}

/**
 * @brief Send a potentially long text line to PC bridge (for T: STT messages)
 * @param[in] prefix single-char prefix (e.g., "T")
 * @param[in] payload text content after the prefix
 * @return none
 */
STATIC VOID_T __send_text_line(CONST CHAR_T *prefix, CONST CHAR_T *payload)
{
    CHAR_T buf[AGENT_TEXT_MAX + 8]; /* "T:" + payload + "\n" + NUL */
    UINT32_T write_len;

    if (prefix == NULL || payload == NULL) {
        return;
    }
    if (!pixel_agent_ble_is_connected() && !s_uart_ready) {
        return;
    }
    INT_T len = snprintf(buf, sizeof(buf), "%s:%s\n", prefix, payload);
    if (len <= 0) {
        return;
    }
    if (len >= (INT_T)sizeof(buf)) {
        write_len = (UINT32_T)(sizeof(buf) - 1);
        buf[write_len - 1] = '\n';
    } else {
        write_len = (UINT32_T)len;
    }
    __transport_send((CONST UINT8_T *)buf, write_len);
}

/**
 * @brief Map protocol state string to visualization enum
 * @param[in] name state name from S: command
 * @return none
 */
/**
 * @brief Map protocol state string to visualization enum
 * @param[in] name state name from S: command
 * @return TRUE if visualization state changed
 */
STATIC BOOL_T __set_vis_from_name(CONST CHAR_T *name)
{
    PIXEL_AGENT_VIS_E new_vis;

    if (name == NULL || name[0] == '\0') {
        return FALSE;
    }
    if (strcmp(name, "thinking") == 0) {
        new_vis = PIXEL_AGENT_VIS_THINKING;
    } else if (strcmp(name, "working") == 0) {
        new_vis = PIXEL_AGENT_VIS_WORKING;
    } else if (strcmp(name, "juggling") == 0) {
        new_vis = PIXEL_AGENT_VIS_JUGGLING;
    } else if (strcmp(name, "notify") == 0) {
        new_vis = PIXEL_AGENT_VIS_NOTIFY;
    } else if (strcmp(name, "error") == 0) {
        new_vis = PIXEL_AGENT_VIS_ERROR;
    } else if (strcmp(name, "happy") == 0) {
        new_vis = PIXEL_AGENT_VIS_HAPPY;
    } else {
        new_vis = PIXEL_AGENT_VIS_IDLE;
    }
    if (new_vis == s_vis_state) {
        return FALSE;
    }
    s_vis_state = new_vis;
    PR_DEBUG("agent vis -> %s (%d)", name, (int)s_vis_state);
    return TRUE;
}

/**
 * @brief Handle one complete line from PC
 * @param[in] line null-terminated line
 * @return none
 */
STATIC VOID_T __handle_line(CHAR_T *line)
{
    if (line == NULL || line[0] == '\0') {
        return;
    }

    if (s_rx_from_serial && (line[0] == 'S' || line[0] == 'P')) {
        pixel_agent_ble_on_serial_activity();
    }

    if (line[0] == 'L' && line[1] == ':') {
        pixel_agent_ble_on_host_link(line[2] == '1' ? TRUE : FALSE);
        PR_NOTICE("agent bridge: serial L:%c", line[2]);
        return;
    }

    if (line[0] == 'S' && line[1] == ':') {
        CONST CHAR_T *state_name = line + 2;

        __set_vis_from_name(state_name);
        PR_DEBUG("agent bridge state: %s", state_name);
        return;
    }

    if (line[0] == 'P' && line[1] == ':') {
        CONST CHAR_T *rest = line + 2;
        if (rest[0] == '\0') {
            s_perm_pending = FALSE;
            s_perm_tool[0] = '\0';
            PR_DEBUG("agent bridge perm cleared");
            return;
        }
        if (strncmp(rest, "tool=", 5) == 0) {
            strncpy(s_perm_tool, rest + 5, AGENT_PERM_TOOL_MAX - 1);
            s_perm_tool[AGENT_PERM_TOOL_MAX - 1] = '\0';
            s_perm_pending = TRUE;
            s_vis_state = PIXEL_AGENT_VIS_NOTIFY;
            pixel_agent_bridge_request_monitor_mode();
            PR_NOTICE("agent bridge perm: %s", s_perm_tool);
        }
        return;
    }
}

/**
 * @brief Feed received bytes into line parser
 * @param[in] data rx bytes
 * @param[in] len byte count
 * @return none
 */
__attribute__((weak)) VOID_T pixel_agent_bridge_request_monitor_mode(VOID_T)
{
}

VOID_T pixel_agent_bridge_feed_rx(CONST UINT8_T *data, UINT32_T len)
{
    for (UINT32_T i = 0; i < len; i++) {
        CHAR_T ch = (CHAR_T)data[i];
        if (ch == '\r') {
            continue;
        }
        if (ch == '\n') {
            if (s_line_pos > 0) {
                s_line_buf[s_line_pos] = '\0';
                __handle_line(s_line_buf);
                s_line_pos = 0;
            }
            continue;
        }
        if (s_line_pos < AGENT_LINE_BUF_SIZE - 1) {
            s_line_buf[s_line_pos++] = ch;
        } else {
            s_line_pos = 0;
        }
    }
}

OPERATE_RET pixel_agent_bridge_init(VOID_T)
{
    OPERATE_RET rt = OPRT_OK;
    TAL_UART_CFG_T uart_cfg = {0};

    uart_cfg.base_cfg.baudrate = AGENT_UART_BAUD;
    uart_cfg.base_cfg.databits = TUYA_UART_DATA_LEN_8BIT;
    uart_cfg.base_cfg.stopbits = TUYA_UART_STOP_LEN_1BIT;
    uart_cfg.base_cfg.parity = TUYA_UART_PARITY_TYPE_NONE;
    uart_cfg.rx_buffer_size = AGENT_RX_BUF_SIZE;
    uart_cfg.open_mode = O_BLOCK;

    rt = tal_uart_init(AGENT_UART_NUM, &uart_cfg);
    if (rt != OPRT_OK) {
        if (tal_uart_get_rx_data_size(AGENT_UART_NUM) >= 0) {
            PR_WARN("agent bridge: UART0 already open, reusing for RX");
            s_uart_ready = TRUE;
        } else {
            PR_ERR("agent bridge UART init failed: %d", rt);
            s_uart_ready = FALSE;
            return rt;
        }
    } else {
        s_uart_ready = TRUE;
    }
    s_line_pos = 0;
    s_vis_state = PIXEL_AGENT_VIS_IDLE;
    s_perm_pending = FALSE;
    s_perm_tool[0] = '\0';
    PR_NOTICE("Agent bridge UART%d ready (%u baud) — PC bridge must use CH342 1st port",
              (int)TUYA_UART_GET_PORT_NUMBER(AGENT_UART_NUM), (unsigned)AGENT_UART_BAUD);
    return OPRT_OK;
}

VOID_T pixel_agent_bridge_poll(VOID_T)
{
    UINT8_T chunk[64];
    INT_T read_len;
    UINT32_T drain;

    if (!s_uart_ready) {
        return;
    }

    for (drain = 0; drain < AGENT_RX_DRAIN_MAX; drain++) {
        if (tal_uart_get_rx_data_size(AGENT_UART_NUM) <= 0) {
            break;
        }
        read_len = tal_uart_read(AGENT_UART_NUM, chunk, sizeof(chunk));
        if (read_len > 0) {
            s_rx_from_serial = TRUE;
            pixel_agent_bridge_feed_rx(chunk, (UINT32_T)read_len);
            s_rx_from_serial = FALSE;
        } else {
            break;
        }
    }
}

PIXEL_AGENT_VIS_E pixel_agent_bridge_get_vis_state(VOID_T)
{
    return s_vis_state;
}

BOOL_T pixel_agent_bridge_perm_pending(VOID_T)
{
    return s_perm_pending;
}

CONST CHAR_T *pixel_agent_bridge_perm_tool_name(VOID_T)
{
    return s_perm_tool;
}

VOID_T pixel_agent_bridge_on_button_deny(VOID_T)
{
    if (!s_perm_pending) {
        return;
    }
    __send_line("B:deny");
    s_perm_pending = FALSE;
    s_perm_tool[0] = '\0';
    PR_NOTICE("agent bridge: sent deny");
}

VOID_T pixel_agent_bridge_on_button_allow(VOID_T)
{
    if (!s_perm_pending) {
        return;
    }
    __send_line("B:allow");
    s_perm_pending = FALSE;
    s_perm_tool[0] = '\0';
    PR_NOTICE("agent bridge: sent allow");
}

VOID_T pixel_agent_bridge_on_button_always(VOID_T)
{
    if (!s_perm_pending) {
        return;
    }
    __send_line("B:always");
    s_perm_pending = FALSE;
    s_perm_tool[0] = '\0';
    PR_NOTICE("agent bridge: sent always");
}

/**
 * @brief User pressed B (non-permission) — clear PC input field
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_clear(VOID_T)
{
    if (s_perm_pending) {
        return;
    }
    __send_line("K:clear");
    PR_NOTICE("agent bridge: sent key clear");
}

/**
 * @brief User pressed A (non-permission) — backspace one char in PC input
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_backspace(VOID_T)
{
    if (s_perm_pending) {
        return;
    }
    __send_line("K:backspace");
    PR_NOTICE("agent bridge: sent key backspace");
}

/**
 * @brief User pressed OK (non-permission) — send/submit PC input (Return key)
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_enter(VOID_T)
{
    if (s_perm_pending) {
        return;
    }
    __send_line("K:enter");
    PR_NOTICE("agent bridge: sent key enter");
}

/* ---------------------------------------------------------------------------
 * Voice-to-Text (STT) transcript bridge
 * --------------------------------------------------------------------------- */

VOID_T pixel_agent_bridge_send_text(CONST CHAR_T *text)
{
    if (text == NULL || text[0] == '\0') {
        PR_NOTICE("agent bridge: STT text empty, skipping");
        return;
    }
    PR_NOTICE("agent bridge: sending STT text (%d chars): %.80s",
              (int)strlen(text), text);
    __send_text_line("T", text);
}

VOID_T pixel_agent_bridge_set_recording(BOOL_T recording)
{
    s_voice_recording = recording;
}

BOOL_T pixel_agent_bridge_is_recording(VOID_T)
{
    return s_voice_recording;
}
