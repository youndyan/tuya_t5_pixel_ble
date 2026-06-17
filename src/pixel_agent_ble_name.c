/**
 * @file pixel_agent_ble_name.c
 * @brief Unique BLE advertising name + chip identity for multi-board environments
 * @version 1.3
 * @date 2026-06-15
 * @copyright Copyright (c) Tuya Inc.
 */
#include "pixel_agent_ble.h"
#include "pixel_agent_license.h"
#include "ble_mgr.h"
#include "tal_bluetooth.h"
#include "tal_log.h"
#include "tal_sw_timer.h"
#include "tal_wifi.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define AGENT_BLE_ADV_NAME_MAX   15
#define AGENT_BLE_CHIP_TAG_MAX   8

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC BOOL_T s_adv_name_applied = FALSE;
STATIC CHAR_T s_chip_tag[AGENT_BLE_CHIP_TAG_MAX] = {0};
STATIC TIMER_ID s_name_retry_timer = NULL;

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Delayed retry when identity is not ready at boot
 * @param[in] timer_id timer handle
 * @param[in] arg unused
 * @return none
 */
STATIC VOID_T __adv_name_retry_cb(TIMER_ID timer_id, VOID_T *arg)
{
    (void)timer_id;
    (void)arg;
    pixel_agent_ble_apply_unique_adv_name();
}

/**
 * @brief Lowercase ASCII in place
 * @param[in,out] s string buffer
 * @return none
 */
STATIC VOID_T __tolower_str(CHAR_T *s)
{
    UINT32_T i;

    if (s == NULL) {
        return;
    }
    for (i = 0; s[i] != '\0'; i++) {
        s[i] = (CHAR_T)tolower((unsigned char)s[i]);
    }
}

/**
 * @brief Derive 4-char chip tag from build-time license UUID
 * @param[out] tag output tag buffer
 * @param[in] tag_sz buffer size
 * @return OPRT_OK on success
 */
STATIC OPERATE_RET __chip_tag_from_build(CHAR_T *tag, UINT32_T tag_sz)
{
    size_t ulen;

    if (tag == NULL || tag_sz < 5) {
        return OPRT_INVALID_PARM;
    }
    ulen = strlen(TUYA_OPENSDK_UUID);
    if (ulen < 4) {
        return OPRT_COM_ERROR;
    }
    snprintf(tag, tag_sz, "%.4s", TUYA_OPENSDK_UUID + ulen - 4);
    __tolower_str(tag);
    return OPRT_OK;
}

/**
 * @brief Build TYBLE_XXXX from build UUID suffix, else BLE/WiFi MAC
 * @param[out] name output buffer
 * @param[in] name_sz buffer size
 * @param[out] tag optional chip tag
 * @param[in] tag_sz tag buffer size
 * @return OPRT_OK when a name was built
 */
STATIC OPERATE_RET __build_unique_adv_name(CHAR_T *name, UINT32_T name_sz, CHAR_T *tag, UINT32_T tag_sz)
{
    TAL_BLE_ADDR_T ble_addr = {0};
    NW_MAC_S wifi_mac = {{0}};
    CHAR_T chip[AGENT_BLE_CHIP_TAG_MAX] = {0};
    BOOL_T mac_ok = FALSE;

    if (__chip_tag_from_build(chip, sizeof(chip)) == OPRT_OK) {
        snprintf(name, name_sz, "TYBLE_%s", chip);
        if (tag != NULL && tag_sz > 0) {
            snprintf(tag, tag_sz, "%s", chip);
        }
        return OPRT_OK;
    }
    if (tal_ble_address_get(&ble_addr) == OPRT_OK) {
        if (ble_addr.addr[0] != 0 || ble_addr.addr[4] != 0 || ble_addr.addr[5] != 0) {
            snprintf(name, name_sz, "TYBLE_%02X%02X", ble_addr.addr[4], ble_addr.addr[5]);
            snprintf(chip, sizeof(chip), "%02x%02x", ble_addr.addr[4], ble_addr.addr[5]);
            mac_ok = TRUE;
        }
    }
    if (!mac_ok && tal_wifi_get_mac(WF_STATION, &wifi_mac) == OPRT_OK) {
        snprintf(name, name_sz, "TYBLE_%02X%02X", wifi_mac.mac[4], wifi_mac.mac[5]);
        snprintf(chip, sizeof(chip), "%02x%02x", wifi_mac.mac[4], wifi_mac.mac[5]);
        mac_ok = TRUE;
    }
    if (!mac_ok) {
        return OPRT_COM_ERROR;
    }
    if (tag != NULL && tag_sz > 0) {
        snprintf(tag, tag_sz, "%s", chip);
    }
    return OPRT_OK;
}

/**
 * @brief Set BLE adv name to TYBLE_XXXX and cache chip tag
 * @return OPRT_OK on success
 */
OPERATE_RET pixel_agent_ble_apply_unique_adv_name(VOID_T)
{
    CHAR_T name[AGENT_BLE_ADV_NAME_MAX + 1];
    CHAR_T tag[AGENT_BLE_CHIP_TAG_MAX] = {0};

    if (s_adv_name_applied) {
        return OPRT_OK;
    }
    if (__build_unique_adv_name(name, sizeof(name), tag, sizeof(tag)) != OPRT_OK) {
        PR_WARN("agent ble: identity unavailable, keep default TYBLE (retry in 2s / on MQTT)");
        if (s_name_retry_timer == NULL) {
            if (tal_sw_timer_create(__adv_name_retry_cb, NULL, &s_name_retry_timer) == OPRT_OK) {
                tal_sw_timer_start(s_name_retry_timer, 2000, TAL_TIMER_ONCE);
            }
        }
        return OPRT_COM_ERROR;
    }
    if (tuya_ble_set_device_name(name) != OPRT_OK) {
        PR_WARN("agent ble: set adv name failed");
        return OPRT_COM_ERROR;
    }
    snprintf(s_chip_tag, sizeof(s_chip_tag), "%s", tag);
    s_adv_name_applied = TRUE;
    PR_NOTICE("agent ble: adv name %s chip=%s", name, s_chip_tag);
    return OPRT_OK;
}

/**
 * @brief Notify PC of chip identity (I:tag line)
 * @return OPRT_OK if sent
 */
OPERATE_RET pixel_agent_ble_send_identity(VOID_T)
{
    CHAR_T tag[AGENT_BLE_CHIP_TAG_MAX] = {0};
    CHAR_T line[24];
    UINT32_T len;

    if (s_chip_tag[0] != '\0') {
        snprintf(tag, sizeof(tag), "%s", s_chip_tag);
    } else if (__chip_tag_from_build(tag, sizeof(tag)) != OPRT_OK &&
               __build_unique_adv_name(line, sizeof(line), tag, sizeof(tag)) != OPRT_OK) {
        PR_WARN("agent ble: identity query but tag unknown");
        return OPRT_COM_ERROR;
    }
    len = (UINT32_T)snprintf(line, sizeof(line), "I:%s\n", tag);
    PR_NOTICE("agent ble: identity %s", line);
    return pixel_agent_ble_send((CONST UINT8_T *)line, len);
}
