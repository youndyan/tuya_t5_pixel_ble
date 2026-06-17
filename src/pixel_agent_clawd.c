/**
 * @file pixel_agent_clawd.c
 * @brief Clawd theme GIF animations for agent monitor (from clawd-on-desk / buddy_pixel)
 * @version 1.0
 * @date 2026-06-05
 * @copyright Copyright (c) Tuya Inc.
 *
 * State mapping (themes/clawd/theme.json + pixel-agent-bridge):
 *   idle         -> clawd-idle
 *   thinking     -> clawd-working-thinking
 *   working      -> clawd-working-typing (gif_clawd_typing)
 *   juggling     -> clawd-working-juggling
 *   notify       -> clawd-notification (permission overlay drawn in tuya_main)
 *   error        -> clawd-error
 *   happy        -> clawd-happy (attention)
 */
#include "pixel_agent_clawd.h"
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
#include "pixel_agent_ble.h"
#endif
#include "board_pixel_api.h"
#include "tdl_pixel_dev_manage.h"
#include "tal_api.h"
#include <string.h>

#include "gif_clawd_idle.h"
#include "gif_clawd_thinking.h"
#include "gif_clawd_typing.h"
#include "gif_clawd_juggling.h"
#include "gif_clawd_notification.h"
#include "gif_clawd_error.h"
#include "gif_clawd_happy.h"

/* ---------------------------------------------------------------------------
 * Macros
 * --------------------------------------------------------------------------- */
#define CLAWD_FRAME_MIN_MS       20u
#define CLAWD_HAPPY_AUTO_IDLE_MS 3000u
#define CLAWD_ROTATE_BUF_SIZE    (32 * 32 * 3)   /* 32x32 RGB rotation scratch */

/* ---------------------------------------------------------------------------
 * Type definitions
 * --------------------------------------------------------------------------- */
typedef struct {
    const uint8_t **frames;
    UINT32_T        frame_count;
    UINT32_T        size;
    UINT32_T        delay_ms;
    UINT32_T        cur_frame;
    UINT32_T        last_advance_ms;
} CLAWD_GIF_SLOT_T;

/* ---------------------------------------------------------------------------
 * File scope variables
 * --------------------------------------------------------------------------- */
STATIC BOOL_T               s_ready = FALSE;
STATIC PIXEL_HANDLE_T       s_px    = NULL;
STATIC PIXEL_FRAME_HANDLE_T s_frame = NULL;

STATIC CLAWD_GIF_SLOT_T s_slot_idle;
STATIC CLAWD_GIF_SLOT_T s_slot_thinking;
STATIC CLAWD_GIF_SLOT_T s_slot_working;
STATIC CLAWD_GIF_SLOT_T s_slot_juggling;
STATIC CLAWD_GIF_SLOT_T s_slot_notify;
STATIC CLAWD_GIF_SLOT_T s_slot_error;
STATIC CLAWD_GIF_SLOT_T s_slot_happy;

STATIC PIXEL_AGENT_VIS_E s_last_vis         = PIXEL_AGENT_VIS_IDLE;
STATIC UINT32_T          s_happy_since_ms   = 0;
STATIC BOOL_T            s_happy_timed_out  = FALSE;
STATIC BOOL_T            s_ble_theme        = FALSE;

/* 90° CCW rotation scratch buffer (reused every frame draw) */
STATIC uint8_t s_rotated_buf[CLAWD_ROTATE_BUF_SIZE];

/* ---------------------------------------------------------------------------
 * Function implementations
 * --------------------------------------------------------------------------- */
/**
 * @brief Initialize one GIF slot from generated C arrays
 * @param[out] slot slot to fill
 * @param[in] frames frame pointer array
 * @param[in] frame_count number of frames
 * @param[in] size width/height in pixels
 * @param[in] delay_ms per-frame delay from gif_to_c.py
 * @return none
 */
STATIC VOID_T __slot_init(CLAWD_GIF_SLOT_T *slot, const uint8_t **frames, UINT32_T frame_count, UINT32_T size,
                          UINT32_T delay_ms)
{
    if (slot == NULL) {
        return;
    }
    memset(slot, 0, sizeof(CLAWD_GIF_SLOT_T));
    slot->frames      = frames;
    slot->frame_count = frame_count;
    slot->size        = size;
    slot->delay_ms    = (delay_ms > CLAWD_FRAME_MIN_MS) ? delay_ms : CLAWD_FRAME_MIN_MS;
}

/**
 * @brief Advance GIF frame when delay elapsed
 * @param[in,out] slot GIF slot
 * @param[in] now_ms current tick ms
 * @return none
 */
STATIC VOID_T __slot_tick(CLAWD_GIF_SLOT_T *slot, UINT32_T now_ms)
{
    if (slot == NULL || slot->frames == NULL || slot->frame_count == 0) {
        return;
    }
    if (slot->last_advance_ms == 0) {
        slot->last_advance_ms = now_ms;
        return;
    }
    if ((now_ms - slot->last_advance_ms) >= slot->delay_ms) {
        slot->cur_frame = (slot->cur_frame + 1) % slot->frame_count;
        slot->last_advance_ms = now_ms;
    }
}

/**
 * @brief Reset slot to first frame
 * @param[in,out] slot GIF slot
 * @return none
 */
STATIC VOID_T __slot_reset(CLAWD_GIF_SLOT_T *slot)
{
    if (slot == NULL) {
        return;
    }
    slot->cur_frame       = 0;
    slot->last_advance_ms = 0;
}

/**
 * @brief Rotate a square RGB bitmap 90° clockwise into s_rotated_buf
 * @param[in] src source bitmap (row-major RGB, size x size)
 * @param[in] size width/height of the square bitmap
 * @return pointer to rotated bitmap (s_rotated_buf)
 *
 * 90° CW transform:  src(x, y) -> dst(y, size-1-x)
 *   - Top row becomes right column
 *   - Left column becomes top row
 */
STATIC CONST uint8_t *__rotate_90_cw(CONST uint8_t *src, UINT32_T size)
{
    UINT32_T x, y;

    for (y = 0; y < size; y++) {
        for (x = 0; x < size; x++) {
            /* Source pixel at row y, col x */
            UINT32_T src_idx = (y * size + x) * 3;
            /* Destination: CW rotate -> row x, col (size-1-y) reversed to row y, col (size-1-x) */
            UINT32_T dst_idx = ((size - 1 - x) * size + y) * 3;

            s_rotated_buf[dst_idx]     = src[src_idx];       /* R */
            s_rotated_buf[dst_idx + 1] = src[src_idx + 1];   /* G */
            s_rotated_buf[dst_idx + 2] = src[src_idx + 2];   /* B */
        }
    }
    return s_rotated_buf;
}

/**
 * @brief Shift RGB toward cool blue (BLE transport indicator)
 * @param[in,out] rgb row-major RGB buffer
 * @param[in] pixel_count number of pixels
 * @return none
 */
STATIC VOID_T __apply_ble_tint(uint8_t *rgb, UINT32_T pixel_count)
{
    UINT32_T i;

    for (i = 0; i < pixel_count; i++) {
        UINT32_T base = i * 3;
        UINT8_T r = rgb[base];
        UINT8_T g = rgb[base + 1];
        UINT8_T b = rgb[base + 2];
        UINT16_T b_boost;

        if (r < 4 && g < 4 && b < 4) {
            continue;
        }
        rgb[base] = (UINT8_T)((r * 22) / 100);
        rgb[base + 1] = (UINT8_T)((g * 35) / 100);
        b_boost = (UINT16_T)b + ((UINT16_T)(255 - b) * 55) / 100;
        rgb[base + 2] = (UINT8_T)(b_boost > 255 ? 255 : b_boost);
    }
}

/**
 * @brief Draw current frame of slot to LED matrix (with 90° CCW rotation)
 * @param[in] slot GIF slot
 * @return none
 */
STATIC VOID_T __slot_draw(CONST CLAWD_GIF_SLOT_T *slot)
{
    CONST uint8_t *bitmap;
    CONST uint8_t *rotated;
    UINT32_T idx;

    if (!s_ready || s_frame == NULL || slot == NULL || slot->frames == NULL || slot->frame_count == 0) {
        return;
    }

    idx = slot->cur_frame;
    if (idx >= slot->frame_count) {
        idx = 0;
    }
    bitmap = slot->frames[idx];
    if (bitmap == NULL) {
        return;
    }

    /* Rotate 90° CW to match desired orientation */
    rotated = __rotate_90_cw(bitmap, slot->size);
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
    if (pixel_agent_ble_should_use_blue_theme()) {
        __apply_ble_tint(s_rotated_buf, slot->size * slot->size);
    }
#else
    if (s_ble_theme) {
        __apply_ble_tint(s_rotated_buf, slot->size * slot->size);
    }
#endif

    board_pixel_frame_clear(s_frame);
    board_pixel_draw_bitmap(s_frame, 0, 0, rotated, slot->size, slot->size);
    board_pixel_frame_render(s_frame);
}

/**
 * @brief Pick GIF slot for visualization state
 * @param[in] vis agent visualization state
 * @return pointer to slot, NULL if none
 */
STATIC CLAWD_GIF_SLOT_T *__slot_for_vis(PIXEL_AGENT_VIS_E vis)
{
    switch (vis) {
    case PIXEL_AGENT_VIS_THINKING:
        return &s_slot_thinking;
    case PIXEL_AGENT_VIS_WORKING:
        return &s_slot_working;
    case PIXEL_AGENT_VIS_JUGGLING:
        return &s_slot_juggling;
    case PIXEL_AGENT_VIS_NOTIFY:
        return &s_slot_notify;
    case PIXEL_AGENT_VIS_ERROR:
        return &s_slot_error;
    case PIXEL_AGENT_VIS_HAPPY:
        return &s_slot_happy;
    case PIXEL_AGENT_VIS_IDLE:
    default:
        return &s_slot_idle;
    }
}

/**
 * @brief On visualization state change, reset the active GIF slot
 * @param[in] vis new visualization state
 * @return none
 */
STATIC VOID_T __on_vis_change(PIXEL_AGENT_VIS_E vis)
{
    CLAWD_GIF_SLOT_T *slot;

    if (vis == s_last_vis) {
        return;
    }

    if (vis == PIXEL_AGENT_VIS_HAPPY && s_last_vis != PIXEL_AGENT_VIS_HAPPY) {
        s_happy_since_ms = tal_system_get_millisecond();
        s_happy_timed_out = FALSE;
    }
    if (vis != PIXEL_AGENT_VIS_HAPPY) {
        s_happy_timed_out = FALSE;
    }

    slot = __slot_for_vis(vis);
    if (slot != NULL) {
        __slot_reset(slot);
    }

    s_last_vis = vis;
}

/**
 * @brief Load Clawd GIF assets and create frame buffer
 * @return OPRT_OK on success
 */
OPERATE_RET pixel_agent_clawd_init(VOID_T)
{
    OPERATE_RET rt;

    rt = board_pixel_get_handle(&s_px);
    if (rt != OPRT_OK || s_px == NULL) {
        PR_ERR("pixel_agent_clawd: no pixel handle");
        return rt;
    }

    s_frame = board_pixel_frame_create();
    if (s_frame == NULL) {
        PR_WARN("pixel_agent_clawd: frame create failed");
        return OPRT_COM_ERROR;
    }

    __slot_init(&s_slot_idle, clawd_idle_frames, clawd_idle_frame_count, clawd_idle_size, clawd_idle_delay_ms);
    __slot_init(&s_slot_thinking, clawd_thinking_frames, clawd_thinking_frame_count, clawd_thinking_size,
                clawd_thinking_delay_ms);
    __slot_init(&s_slot_working, clawd_typing_frames, clawd_typing_frame_count, clawd_typing_size,
                clawd_typing_delay_ms);
    __slot_init(&s_slot_juggling, clawd_juggling_frames, clawd_juggling_frame_count, clawd_juggling_size,
                clawd_juggling_delay_ms);
    __slot_init(&s_slot_notify, clawd_notification_frames, clawd_notification_frame_count, clawd_notification_size,
                clawd_notification_delay_ms);
    __slot_init(&s_slot_error, clawd_error_frames, clawd_error_frame_count, clawd_error_size, clawd_error_delay_ms);
    __slot_init(&s_slot_happy, clawd_happy_frames, clawd_happy_frame_count, clawd_happy_size, clawd_happy_delay_ms);

    s_last_vis  = PIXEL_AGENT_VIS_IDLE;
    s_ready     = TRUE;
    PR_NOTICE("pixel_agent_clawd: ready (idle/thinking/typing-work/juggling/notify/error/happy)");
    return OPRT_OK;
}

/**
 * @brief Whether Clawd GIF renderer is ready
 * @return TRUE if initialized
 */
BOOL_T pixel_agent_clawd_ready(VOID_T)
{
    return s_ready;
}

VOID_T pixel_agent_clawd_set_ble_theme(BOOL_T enabled)
{
    if (s_ble_theme == enabled) {
        return;
    }
    s_ble_theme = enabled;
    PR_NOTICE("agent clawd: theme %s", enabled ? "ble (blue)" : "serial (default)");
}

/**
 * @brief Draw one animation frame for the given agent visualization state
 * @param[in] vis state from pixel_agent_bridge
 * @return none
 */
VOID_T pixel_agent_clawd_render(PIXEL_AGENT_VIS_E vis)
{
    CLAWD_GIF_SLOT_T *slot;
    UINT32_T now_ms;
    PIXEL_AGENT_VIS_E draw_vis;

    if (!s_ready) {
        return;
    }

    __on_vis_change(vis);

    draw_vis = vis;
    if (vis == PIXEL_AGENT_VIS_HAPPY) {
        now_ms = tal_system_get_millisecond();
        if ((now_ms - s_happy_since_ms) >= CLAWD_HAPPY_AUTO_IDLE_MS) {
            if (!s_happy_timed_out) {
                s_happy_timed_out = TRUE;
                __slot_reset(&s_slot_idle);
                PR_NOTICE("agent clawd: happy -> idle after %ums", (unsigned)CLAWD_HAPPY_AUTO_IDLE_MS);
            }
            draw_vis = PIXEL_AGENT_VIS_IDLE;
        }
    }

    slot = __slot_for_vis(draw_vis);
    if (slot == NULL) {
        return;
    }

    now_ms = tal_system_get_millisecond();
    __slot_tick(slot, now_ms);
    __slot_draw(slot);
}
