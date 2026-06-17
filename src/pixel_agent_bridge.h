/**
 * @file pixel_agent_bridge.h
 * @brief USB serial bridge to PC pixel-agent-bridge (Cursor / Claude Code states)
 * @version 1.0
 * @date 2026-06-04
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __PIXEL_AGENT_BRIDGE_H__
#define __PIXEL_AGENT_BRIDGE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef enum {
    PIXEL_AGENT_VIS_IDLE = 0,
    PIXEL_AGENT_VIS_THINKING,
    PIXEL_AGENT_VIS_WORKING,
    PIXEL_AGENT_VIS_JUGGLING,
    PIXEL_AGENT_VIS_NOTIFY,
    PIXEL_AGENT_VIS_ERROR,
    PIXEL_AGENT_VIS_HAPPY,
} PIXEL_AGENT_VIS_E;

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize UART0 for agent bridge (115200 8N1)
 * @return OPRT_OK on success
 */
OPERATE_RET pixel_agent_bridge_init(VOID_T);

/**
 * @brief Poll UART RX and parse line protocol from PC bridge
 * @return none
 */
VOID_T pixel_agent_bridge_poll(VOID_T);

/**
 * @brief Feed RX bytes from BLE NUS (or other transport) into line parser
 * @param[in] data rx bytes
 * @param[in] len byte count
 * @return none
 */
VOID_T pixel_agent_bridge_feed_rx(CONST UINT8_T *data, UINT32_T len);

/**
 * @brief Switch display to agent monitor when PC sends S: state (weak, app overrides)
 * @return none
 */
VOID_T pixel_agent_bridge_request_monitor_mode(VOID_T);

/**
 * @brief Current visualization state for agent monitor mode
 * @return PIXEL_AGENT_VIS_E
 */
PIXEL_AGENT_VIS_E pixel_agent_bridge_get_vis_state(VOID_T);

/**
 * @brief Whether a Claude permission request is waiting for button input
 * @return TRUE if permission UI active
 */
BOOL_T pixel_agent_bridge_perm_pending(VOID_T);

/**
 * @brief Short tool name for permission overlay (empty if none)
 * @return read-only C string, max 15 chars + NUL
 */
CONST CHAR_T *pixel_agent_bridge_perm_tool_name(VOID_T);

/**
 * @brief User pressed B — deny pending permission
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_deny(VOID_T);

/**
 * @brief User pressed A — allow pending permission
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_allow(VOID_T);

/**
 * @brief User pressed OK — always allow (addRules) pending permission
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_always(VOID_T);

/**
 * @brief User pressed B (non-permission) — clear PC input field
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_clear(VOID_T);

/**
 * @brief User pressed A (non-permission) — backspace one char in PC input
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_backspace(VOID_T);

/**
 * @brief User pressed OK (non-permission) — send/submit PC input (Return key)
 * @return none
 */
VOID_T pixel_agent_bridge_on_button_enter(VOID_T);

/* ---------------------------------------------------------------------------
 * Voice-to-Text (STT) transcript bridge
 * --------------------------------------------------------------------------- */
#define AGENT_TEXT_MAX        512   /* max STT text payload per line */

/**
 * @brief Send STT transcript text to PC for keyboard typing
 * @param[in] text UTF-8 recognized text from cloud STT (may be Chinese)
 * @return none
 */
VOID_T pixel_agent_bridge_send_text(CONST CHAR_T *text);

/**
 * @brief Set voice recording state (called by OK long-press / release in agent mode)
 * @param[in] recording TRUE when OK is held and mic is capturing
 * @return none
 */
VOID_T pixel_agent_bridge_set_recording(BOOL_T recording);

/**
 * @brief Check if voice recording is currently active (B held)
 * @return TRUE if recording in progress
 */
BOOL_T pixel_agent_bridge_is_recording(VOID_T);

#ifdef __cplusplus
}
#endif

#endif /* __PIXEL_AGENT_BRIDGE_H__ */
