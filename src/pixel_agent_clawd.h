/**
 * @file pixel_agent_clawd.h
 * @brief Clawd theme GIF animations for agent monitor mode (clawd-on-desk states)
 * @version 1.0
 * @date 2026-06-05
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __PIXEL_AGENT_CLAWD_H__
#define __PIXEL_AGENT_CLAWD_H__

#include "pixel_agent_bridge.h"
#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------------------
 * Function declarations
 * --------------------------------------------------------------------------- */
/**
 * @brief Load Clawd GIF assets and create frame buffer (call after pixel LED init)
 * @return OPRT_OK on success
 */
OPERATE_RET pixel_agent_clawd_init(VOID_T);

/**
 * @brief Draw one animation frame for the given agent visualization state
 * @param[in] vis state from pixel_agent_bridge
 * @return none
 * @note Uses clawd-on-desk theme mapping; WORKING uses clawd-working-typing GIF.
 *       HAPPY auto-switches to idle animation after 3 seconds on screen.
 */
VOID_T pixel_agent_clawd_render(PIXEL_AGENT_VIS_E vis);

/**
 * @brief Whether Clawd GIF renderer is ready
 * @return TRUE if pixel_agent_clawd_init succeeded
 */
BOOL_T pixel_agent_clawd_ready(VOID_T);

/**
 * @brief Use blue color theme when PC is on BLE (serial link keeps default colors)
 * @param[in] enabled TRUE for BLE transport, FALSE for serial / disconnected
 * @return none
 * @note Re-renders on next agent monitor frame; do not call from BLE ISR context.
 */
VOID_T pixel_agent_clawd_set_ble_theme(BOOL_T enabled);

#ifdef __cplusplus
}
#endif

#endif /* __PIXEL_AGENT_CLAWD_H__ */
