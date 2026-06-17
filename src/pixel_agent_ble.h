/**
 * @file pixel_agent_ble.h
 * @brief BLE NUS transport for pixel-agent-bridge (coexists with Tuya ble_mgr)
 * @version 1.0
 * @date 2026-06-12
 * @copyright Copyright (c) Tuya Inc.
 */
#ifndef __PIXEL_AGENT_BLE_H__
#define __PIXEL_AGENT_BLE_H__

#include "tuya_cloud_types.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Set NimBLE security to open (no bond) before tuya_ble_init
 * @return none
 * @note Call before netmgr_init()
 */
VOID_T pixel_agent_ble_configure_security(VOID_T);

/**
 * @brief Refresh BLE advertising after MQTT online (bound device must stay connectable)
 * @return none
 */
VOID_T pixel_agent_ble_on_cloud_online(VOID_T);

/**
 * @brief PC bridge linked (notify subscribed) — connection chime in app
 * @return none
 */
VOID_T pixel_agent_ble_on_link_ready(VOID_T);

/**
 * @brief PC bridge reports BLE notify ready (L:1) or down (L:0)
 * @param[in] linked TRUE when PC uses BLE transport
 * @return none
 */
VOID_T pixel_agent_ble_on_host_link(BOOL_T linked);

/**
 * @brief Register BLE event hook (called once at boot)
 * @return OPRT_OK on success
 */
OPERATE_RET pixel_agent_ble_init(VOID_T);

/**
 * @brief Set BLE adv name to TYBLE_XXXX from license/MAC
 * @return OPRT_OK on success
 */
OPERATE_RET pixel_agent_ble_apply_unique_adv_name(VOID_T);

/**
 * @brief Reply I:chip tag to PC for bind verification
 * @return OPRT_OK if notify sent
 */
OPERATE_RET pixel_agent_ble_send_identity(VOID_T);

/**
 * @brief Whether PC is connected and subscribed to NUS TX notifications
 * @return TRUE if ready to send/receive agent lines
 */
BOOL_T pixel_agent_ble_is_connected(VOID_T);

/**
 * @brief Whether agent lines should use BLE (blue) color theme
 * @return TRUE when BLE notify is active or recent BLE agent traffic
 */
BOOL_T pixel_agent_ble_transport_active(VOID_T);

/**
 * @brief Whether GAP link to central is up (real-time connection state)
 * @return TRUE when BLE connected
 */
BOOL_T pixel_agent_ble_link_up(VOID_T);

/**
 * @brief Consume one BLE link chime slot per connection session
 * @return TRUE if chime should play now
 */
BOOL_T pixel_agent_ble_take_link_chime(VOID_T);

/**
 * @brief Whether clawd should render BLE blue theme
 * @return TRUE when host sent L:1 or GATT notify is active
 */
BOOL_T pixel_agent_ble_should_use_blue_theme(VOID_T);

/**
 * @brief Refresh clawd blue/default theme from live BLE link state
 * @return none
 * @note Call from agent monitor render loop each frame
 */
VOID_T pixel_agent_ble_sync_display_theme(VOID_T);

/**
 * @brief PC sent agent data over USB serial — use default clawd colors
 * @return none
 */
VOID_T pixel_agent_ble_on_serial_activity(VOID_T);

/**
 * @brief Send raw bytes to PC via NUS notify (may split by MTU)
 * @param[in] data payload bytes
 * @param[in] len byte count
 * @return OPRT_OK if at least one chunk was sent
 */
OPERATE_RET pixel_agent_ble_send(CONST UINT8_T *data, UINT32_T len);

#ifdef __cplusplus
}
#endif

#endif /* __PIXEL_AGENT_BLE_H__ */
