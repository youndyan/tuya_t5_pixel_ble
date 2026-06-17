#include "tal_api.h"
#include "tkl_output.h"
#include "tal_cli.h"
#include "board_com_api.h"
#include "board_pixel_api.h"
#include "board_buzzer_api.h"
#include "tdl_button_manage.h"
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "led_font.h"
#include "board_bmi270_api.h"
#include "tdl_audio_manage.h"
#include "tuya_ringbuf.h"
#include "cJSON.h"
#include "tuya_iot.h"
#include "tuya_iot_dp.h"
#include "netmgr.h"
#include "tuya_authorize.h"
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
#include "netconn_wifi.h"
#endif
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
#include "netcfg.h"
#include "ble_mgr.h"
#endif
#include "ai_chat_main.h"
#include "ai_manage_mode.h"
#include "ai_user_event.h"
#include "ai_audio_input.h"
#include "ai_audio_player.h"
#include "tuya_ai_agent.h"
#include "tal_event_info.h"

static void __ai_chat_event_cb(AI_NOTIFY_EVENT_T *event);
static void spectrum_reset_state(void);
OPERATE_RET ai_agent_init(void);

/* Agent STT readiness (deferred ai_chat_init + MQTT / AI client) */
static volatile bool s_agent_ai_chat_ready = false;
static volatile bool s_agent_ai_client_ready = false;
static THREAD_HANDLE s_ai_init_thrd = NULL;

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#include "pixel_art/pixel_art_types.h"
#include "pixel_art/resource/mao.h"
#include "pixel_art/resource/laughing_cat.h"
#include "pixel_art/resource/rolling_cat.h"
#include "pixel_art/resource/super_mario_kart_mario.h"
#include "pixel_art/resource/cute_cat_white.h"
#include "pixel_art/resource/smallbwop_bwop.h"
#include "pixel_art/resource/wander.h"
#include "pixel_art/resource/Italian_Beach.h"
#include "pixel_art/resource/Italian_Pixel_Art.h"
#include "pixel_art/resource/Nintendo_Mario.h"
#include "pixel_art/resource/Cat_Meme.h"
#include "pixel_agent_bridge.h"
#include "pixel_agent_clawd.h"
#include "pixel_agent_ble.h"
#if defined(ENABLE_LED) && (ENABLE_LED == 1)
#include "tdl_led_manage.h"
#endif

/***********************************************************
************************macro define************************
***********************************************************/

/* P47 board LED is used by AI hold-mode; suppress it in agent mode (pixel screen shows status) */
#define AGENT_STT_SUPPRESS_BOARD_LED  1

#define LED_PIXELS_TOTAL_NUM 1027
#define COLOR_RESOLUTION     1000u
#define BRIGHTNESS           0.05f // ~5% — safe for most full-screen effects
#define AGENT_BRIGHTNESS     0.02f // ~2% — agent mode (avoid brownout on all-on frames)
/* UART0 is shared with USB debug port — tal_cli would consume bridge traffic */
#define PIXEL_AGENT_OWN_UART0  1

#include "pixel_agent_license.h"

// Network reset: power cycle 3 times within 5s each to trigger
#define RESET_NETCNT_NAME "rst_cnt"
#define RESET_NETCNT_MAX  3

/***********************************************************
***********************variable define**********************
***********************************************************/

// Tuya IoT client
static tuya_iot_client_t g_iot_client;
static tuya_iot_license_t g_license;
static uint8_t g_need_reset = 0;
static volatile bool g_mqtt_connected = false;
static bool g_iot_initialized = false;

static TDL_BUTTON_HANDLE g_button_ok_handle = NULL;
static TDL_BUTTON_HANDLE g_button_a_handle = NULL;
static TDL_BUTTON_HANDLE g_button_b_handle = NULL;

#define EFFECT_ANIMATION_COUNT 10     // Number of effect animations (0-9, includes Sharingan)
#define SHARINGAN_MODE           9     // Procedural Sharingan effect (OK cycle)
#define HOME_ANIMATION_MODE      EFFECT_ANIMATION_COUNT // mao.gif — first registered pixel art

static PIXEL_HANDLE_T g_pixels_handle = NULL;
static THREAD_HANDLE g_pixels_thrd = NULL;
static volatile uint32_t g_animation_mode = HOME_ANIMATION_MODE; // Default: mao.gif home page
static volatile bool g_animation_loop = false;
static volatile bool g_animation_running = false;
static volatile uint32_t g_pixel_art_index = 0;

// Pixel art animation registration system
#define MAX_PIXEL_ART_ANIMATIONS 16
static const pixel_art_t *g_registered_pixel_arts[MAX_PIXEL_ART_ANIMATIONS] = {NULL};
static uint32_t g_registered_pixel_art_count = 0;
#define SAND_PHYSICS_MODE      0xFFFF // Special mode for sand physics demo
#define TETRIS_MODE            0xFFFE // Tetris game mode
#define SNAKE_MODE             0xFFFD // Snake game mode
#define NINJA_MODE             0xFFFC // Ninja runner game mode
#define SPECTRUM_MODE          0xFFFB // Audio spectrum analyzer mode (standalone mic)
#define AGENT_MONITOR_MODE     0xFFFA // Cursor / Claude agent status via USB serial bridge
#define AI_CHAT_SPECTRUM_MODE  0xFFF9 // AI voice chat with spectrum visualization

// Sand physics system (integer grid-based cellular automaton)
#define MATRIX_WIDTH       32
#define MATRIX_HEIGHT      32
#define SAND_MAX_PARTICLES 225 // 15x15 square

typedef struct {
    int8_t x, y;     // Integer grid position (0-31)
    uint8_t r, g, b; // Particle color
    bool active;
} sand_particle_t;

static sand_particle_t g_sand_particles[SAND_MAX_PARTICLES];
static int16_t g_sand_grid[MATRIX_WIDTH][MATRIX_HEIGHT]; // -1=empty, >=0=particle index
static bool g_sand_initialized = false;

// IMU sensor (BMI260, compatible with BMI270 driver after chip_id patch)
static bool g_imu_ready = false;
static bmi270_dev_t *g_imu_handle = NULL;

// Audio spectrum analyzer system
#define SPECTRUM_SAMPLE_RATE   16000
#define SPECTRUM_FFT_SIZE      128
#define SPECTRUM_NUM_BANDS     16
#define SPECTRUM_BAND_WIDTH    2     // 2 pixels per band (16*2=32)
#define SPECTRUM_FRAME_BYTES   320   // 10ms at 16kHz mono 16-bit
#define SPECTRUM_RINGBUF_SIZE  (SPECTRUM_FRAME_BYTES * 32)
#define SPECTRUM_MAG_SCALE_MIC      10000.0f
#define SPECTRUM_MAG_SCALE_TTS      10000.0f
#define SPECTRUM_TTS_PCM_SHIFT      3       /* attenuate decoded PCM by 8x before FFT */
#define SPECTRUM_TTS_NORM_GAIN      0.40f   /* extra display gain after log compress */
#define SPECTRUM_TTS_MAX_BAR        18      /* cap TTS bar height (of 32) */

#ifndef AUDIO_CODEC_NAME
#define AUDIO_CODEC_NAME "audio"
#endif

static TDL_AUDIO_HANDLE_T g_audio_handle = NULL;
static TUYA_RINGBUFF_T g_audio_ringbuf = NULL;
static MUTEX_HANDLE g_audio_rb_mutex = NULL;
static bool g_spectrum_initialized = false;
static bool g_spectrum_ringbuf_ready = false;
static volatile bool g_spectrum_ok_holding = false;
static int16_t g_spectrum_audio_buf[SPECTRUM_FFT_SIZE];
static float g_spectrum_fft_real[SPECTRUM_FFT_SIZE];
static float g_spectrum_fft_imag[SPECTRUM_FFT_SIZE];
static float g_spectrum_band_mag[SPECTRUM_NUM_BANDS];
static float g_spectrum_band_peak[SPECTRUM_NUM_BANDS];

static const float g_spectrum_band_start[SPECTRUM_NUM_BANDS] = {
    0, 125, 250, 500, 750, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 6000, 7000
};
static const float g_spectrum_band_end[SPECTRUM_NUM_BANDS] = {
    125, 250, 500, 750, 1000, 1500, 2000, 2500, 3000, 3500, 4000, 4500, 5000, 6000, 7000, 8000
};

// Tetris game system
#define TETRIS_COLS       10
#define TETRIS_ROWS       20
// After 90° CCW rotation: game becomes 20 wide x 10 tall on screen
// Layout: left 6 cols for score, then 20 cols for game, then 6 cols for next piece
#define TETRIS_BOARD_SX   6   // Board start X on screen (after rotation)
#define TETRIS_BOARD_SY   11  // Board start Y on screen (center 10-tall in 32)
#define TETRIS_DROP_MIN   5   // Fastest speed (frames between drops)
#define TETRIS_DROP_MAX   40  // Slowest speed
#define TETRIS_DROP_DEFAULT 15 // Default speed (~300ms)

// 7 Tetromino types: I, O, T, S, Z, J, L
// Each piece has 4 rotations, each rotation is 4 (x,y) offsets
static const int8_t g_tetris_pieces[7][4][4][2] = {
    // I piece
    {{{0,1},{1,1},{2,1},{3,1}}, {{2,0},{2,1},{2,2},{2,3}}, {{0,2},{1,2},{2,2},{3,2}}, {{1,0},{1,1},{1,2},{1,3}}},
    // O piece
    {{{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}, {{0,0},{1,0},{0,1},{1,1}}},
    // T piece
    {{{1,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{2,1},{1,2}}, {{1,0},{0,1},{1,1},{1,2}}},
    // S piece
    {{{1,0},{2,0},{0,1},{1,1}}, {{1,0},{1,1},{2,1},{2,2}}, {{1,1},{2,1},{0,2},{1,2}}, {{0,0},{0,1},{1,1},{1,2}}},
    // Z piece
    {{{0,0},{1,0},{1,1},{2,1}}, {{2,0},{1,1},{2,1},{1,2}}, {{0,1},{1,1},{1,2},{2,2}}, {{1,0},{0,1},{1,1},{0,2}}},
    // J piece
    {{{0,0},{0,1},{1,1},{2,1}}, {{1,0},{2,0},{1,1},{1,2}}, {{0,1},{1,1},{2,1},{2,2}}, {{1,0},{1,1},{0,2},{1,2}}},
    // L piece
    {{{2,0},{0,1},{1,1},{2,1}}, {{1,0},{1,1},{1,2},{2,2}}, {{0,1},{1,1},{2,1},{0,2}}, {{0,0},{1,0},{1,1},{1,2}}}
};

// Piece colors (R, G, B) - classic Tetris colors
static const uint8_t g_tetris_colors[7][3] = {
    {0, 255, 255},   // I - cyan
    {255, 255, 0},   // O - yellow
    {160, 0, 255},   // T - purple
    {0, 255, 0},     // S - green
    {255, 0, 0},     // Z - red
    {0, 0, 255},     // J - blue
    {255, 165, 0}    // L - orange
};

typedef struct {
    uint8_t grid[TETRIS_COLS][TETRIS_ROWS]; // 0=empty, 1-7=piece color index+1
    int8_t  cur_type;       // Current piece type (0-6)
    int8_t  cur_rot;        // Current rotation (0-3)
    int8_t  cur_x;          // Current piece X position
    int8_t  cur_y;          // Current piece Y position
    int8_t  next_type;      // Next piece type (0-6)
    uint32_t drop_timer;    // Frame counter for auto-drop
    uint32_t drop_speed;    // Frames between auto-drops
    uint32_t score;
    uint8_t  phase;         // 0=speed select, 1=playing, 2=game over
    bool     initialized;
    bool     fast_drop;     // Long-press OK = fast drop
} tetris_state_t;

static tetris_state_t g_tetris = {0};
static volatile uint8_t g_tetris_input = 0; // 1=left, 2=right, 3=rotate/confirm, 4=fast drop start, 5=fast drop stop

// Snake game system
#define SNAKE_MAX_LEN    256  // Max snake body length
#define SNAKE_GRID_W     32
#define SNAKE_GRID_H     32
#define SNAKE_MOVE_FRAMES 10  // Frames between moves (~200ms)

typedef enum { SNAKE_UP = 0, SNAKE_DOWN, SNAKE_LEFT, SNAKE_RIGHT } snake_dir_t;

typedef struct {
    uint8_t x[SNAKE_MAX_LEN];
    uint8_t y[SNAKE_MAX_LEN];
    uint16_t len;
    uint16_t head;          // Index of head in circular buffer
    snake_dir_t dir;
    uint8_t food_x, food_y;
    uint32_t score;
    uint32_t move_timer;
    bool game_over;
    bool initialized;
} snake_state_t;

static snake_state_t g_snake = {0};

// Ninja runner game system
#define NINJA_MAX_OBSTACLES 16
#define NINJA_LINE_GY      16   // Ground line Y in game coords
#define NINJA_ABOVE_GY     12   // Player center Y when above line
#define NINJA_BELOW_GY     20   // Player center Y when below line
#define NINJA_PLAYER_GX    6    // Player X position in game coords
#define NINJA_JUMP_HEIGHT  10   // Jump height in pixels
#define NINJA_JUMP_FRAMES  36   // Total jump duration in frames
#define NINJA_MOVE_SPEED   3    // Fixed obstacle move speed (frames between moves)

typedef struct {
    int8_t gx;          // Game X position (scrolls 31 down to -2)
    uint8_t type;       // 0=above-line, 1=below-line, 2=full-height
    bool active;
} ninja_obstacle_t;

typedef struct {
    bool above_line;            // true=above line, false=below line
    int8_t jump_timer;          // >0 means jumping, counts down
    bool jumping;
    ninja_obstacle_t obstacles[NINJA_MAX_OBSTACLES];
    uint32_t score;
    uint32_t frame_count;
    uint32_t move_timer;        // Frames since last obstacle move
    uint32_t spawn_timer;       // Frames since last spawn
    uint32_t next_spawn;        // Frames until next spawn
    uint8_t  difficulty;        // 1-5, controls obstacle spawn frequency
    uint8_t  phase;             // 0=difficulty select, 1=playing, 2=game over
    bool     initialized;
} ninja_state_t;

static ninja_state_t g_ninja = {0};
static volatile uint8_t g_ninja_input = 0; // 1=A(below), 2=B(above), 3=OK(jump), 4=OK confirm

/***********************************************************
********************function declaration********************
***********************************************************/

static void pixel_art_register_animation(const pixel_art_t *art);
static void pixel_art_init_registrations(void);
static void buzzer_demo_init_buttons(void);
static void buzzer_button_ok_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc);

static void pixel_led_animation_task(void *args);
static OPERATE_RET pixel_led_init(void);
static void __breathing_color_effect(void);
static void __running_light_effect(void);
static void __color_wave_effect(void);
static void __2d_wave_effect(void);
static void __snowflake_effect(void);
static void __breathing_circle_effect(void);
static void __ripple_effect(void);
static void __scan_animation_effect(void);
static void __scrolling_text_effect(void);
static void render_char(int32_t x, int32_t y, char ch, float hue);
static void __pixel_art_effect(const pixel_art_t *art);
static void __sand_physics_effect(void);
static void __tetris_effect(void);
static void __snake_effect(void);
static void __ninja_effect(void);
static void __sharingan_effect(void);
static void __spectrum_effect(void);
static void __agent_set_pixel(int32_t sx, int32_t sy, uint8_t r, uint8_t g, uint8_t b);
static void __agent_standby_effect(void);
static void __agent_monitor_effect(void);
static void __agent_monitor_draw_perm_label(void);
static void spectrum_init(void);
static void spectrum_audio_callback(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data, uint32_t len);
static void sand_update_physics(void);
static void sand_render(void);

static void buzzer_button_a_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc);
static void buzzer_button_b_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc);

// Network provisioning
static void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event);
static bool user_network_check(void);
static int  reset_netconfig_start(void);
static int  reset_netconfig_check(void);

/***********************************************************
***********************function define**********************
***********************************************************/

/**
 * @brief Register a pixel art animation
 */
static void pixel_art_register_animation(const pixel_art_t *art)
{
    if (art == NULL) {
        PR_ERR("Cannot register NULL pixel art");
        return;
    }

    if (g_registered_pixel_art_count >= MAX_PIXEL_ART_ANIMATIONS) {
        PR_ERR("Maximum pixel art animations (%d) reached", MAX_PIXEL_ART_ANIMATIONS);
        return;
    }

    g_registered_pixel_arts[g_registered_pixel_art_count] = art;
    g_registered_pixel_art_count++;
    PR_NOTICE("Registered pixel art animation %d (frames: %d)", g_registered_pixel_art_count - 1, art->frame_count);
}

/**
 * @brief Initialize all pixel art animation registrations
 */
static void pixel_art_init_registrations(void)
{
    // Register all pixel art animations (mao first = default home page)
    pixel_art_register_animation(&mao);
    pixel_art_register_animation(&laughing_cat);
    pixel_art_register_animation(&rolling_cat);
    pixel_art_register_animation(&super_mario_kart_mario);
    pixel_art_register_animation(&cute_cat_white);
    pixel_art_register_animation(&smallbwop_bwop);
    pixel_art_register_animation(&wander);
    pixel_art_register_animation(&Italian_Beach);
    pixel_art_register_animation(&Italian_Pixel_Art);
    pixel_art_register_animation(&Nintendo_Mario);
    pixel_art_register_animation(&Cat_Meme);

    PR_NOTICE("Registered %d pixel art animations", g_registered_pixel_art_count);
}

/**
 * @brief AI cloud client connected — STT may be used after this
 * @param[in] data unused
 * @return 0
 */
static int __agent_ai_client_run_cb(void *data)
{
    (void)data;
    s_agent_ai_client_ready = true;
    PR_NOTICE("Agent STT: AI cloud client ready");
    return 0;
}

#if defined(ENABLE_LED) && (ENABLE_LED == 1) && (AGENT_STT_SUPPRESS_BOARD_LED == 1)
/**
 * @brief Turn off P47 board LED (AI hold mode flashes it during LISTEN/THINK)
 * @return none
 */
static void __agent_board_led_off(void)
{
    TDL_LED_HANDLE_T led = tdl_led_find_dev(LED_NAME);

    if (led != NULL) {
        tdl_led_set_status(led, TDL_LED_OFF);
    }
}
#else
#define __agent_board_led_off() ((void)0)
#endif

/**
 * @brief Bootstrap agent + hold mode when MQTT was already up before ai_chat_init
 * @return OPRT_OK on success
 */
static OPERATE_RET __agent_ai_bootstrap(void)
{
    AI_CHAT_MODE_E mode = AI_CHAT_MODE_HOLD;

    if (ai_mode_get_curr_mode(&mode) == OPRT_OK) {
        return OPRT_OK;
    }
    if (!g_mqtt_connected) {
        PR_WARN("Agent STT: MQTT not connected");
        return OPRT_RESOURCE_NOT_READY;
    }
    if (ai_agent_init() != OPRT_OK) {
        PR_ERR("Agent STT: ai_agent_init failed");
        return OPRT_COM_ERROR;
    }
    if (ai_mode_init(AI_CHAT_MODE_HOLD) != OPRT_OK) {
        PR_ERR("Agent STT: ai_mode_init failed");
        return OPRT_COM_ERROR;
    }
    PR_NOTICE("Agent STT: hold mode bootstrapped");
    return OPRT_OK;
}

/**
 * @brief Start hold-to-talk STT (OK long press)
 * @return none
 */
static void __agent_stt_start(void)
{
    if (!s_agent_ai_chat_ready) {
        PR_WARN("Agent STT: AI not ready (wait after entering agent mode)");
        return;
    }
    if (__agent_ai_bootstrap() != OPRT_OK) {
        return;
    }
    if (!tuya_ai_agent_is_ready()) {
        PR_WARN("Agent STT: cloud client not ready (wait ~5s after MQTT)");
        return;
    }

    pixel_agent_bridge_set_recording(TRUE);
    ai_mode_handle_key(TDL_BUTTON_LONG_PRESS_START, NULL);
    ai_mode_task_running(NULL);
    __agent_board_led_off();
    PR_NOTICE("Agent STT: recording started");
}

/**
 * @brief Stop hold-to-talk STT and upload audio (OK release)
 * @return none
 */
static void __agent_stt_stop(void)
{
    pixel_agent_bridge_set_recording(FALSE);
    if (!s_agent_ai_chat_ready) {
        PR_WARN("Agent STT: AI not ready, skip stop");
        return;
    }
    ai_mode_handle_key(TDL_BUTTON_PRESS_UP, NULL);
    ai_mode_task_running(NULL);
    __agent_board_led_off();
    PR_NOTICE("Agent STT: recording stopped, uploading");
}

/**
 * @brief Apply speaker volume for current AI mode (agent mute / chat audible)
 * @return none
 */
static void __ai_chat_apply_mode_volume(void)
{
    if (!s_agent_ai_chat_ready) {
        return;
    }
    if (g_animation_mode == AGENT_MONITOR_MODE) {
        ai_chat_set_volume(0);
    } else if (g_animation_mode == AI_CHAT_SPECTRUM_MODE) {
        ai_chat_set_volume(50);
    }
}

/**
 * @brief Start hold-to-talk in AI chat spectrum mode (OK long press)
 * @return none
 */
static void __ai_chat_hold_start(void)
{
    if (!s_agent_ai_chat_ready) {
        PR_WARN("AI chat: not ready (wait after entering mode)");
        return;
    }
    if (__agent_ai_bootstrap() != OPRT_OK) {
        return;
    }
    if (!tuya_ai_agent_is_ready()) {
        PR_WARN("AI chat: cloud client not ready");
        return;
    }

    g_spectrum_ok_holding = true;
    ai_mode_handle_key(TDL_BUTTON_LONG_PRESS_START, NULL);
    ai_mode_task_running(NULL);
    __agent_board_led_off();
    PR_NOTICE("AI chat: recording started");
}

/**
 * @brief Stop hold-to-talk in AI chat spectrum mode (OK release)
 * @return none
 */
static void __ai_chat_hold_stop(void)
{
    g_spectrum_ok_holding = false;
    if (!s_agent_ai_chat_ready) {
        return;
    }
    ai_mode_handle_key(TDL_BUTTON_PRESS_UP, NULL);
    ai_mode_task_running(NULL);
    __agent_board_led_off();
    spectrum_reset_state();
    PR_NOTICE("AI chat: recording stopped");
}

/* Thread function to initialize AI chat (deferred from button callback to avoid stack overflow) */
static void __ai_chat_init_thread(void *args)
{
    AI_CHAT_MODE_CFG_T ai_chat_cfg = {
        .default_mode = AI_CHAT_MODE_HOLD,
        .default_vol  = 50,
        .evt_cb       = __ai_chat_event_cb,
    };

    (void)args;

    if (ai_chat_init(&ai_chat_cfg) != OPRT_OK) {
        PR_ERR("AI voice chat init failed");
        tal_thread_delete(s_ai_init_thrd);
        s_ai_init_thrd = NULL;
        return;
    }
    s_agent_ai_chat_ready = true;
    PR_NOTICE("AI voice chat initialized");

    tal_event_subscribe(EVENT_AI_CLIENT_RUN, "pixel_agent_stt", __agent_ai_client_run_cb, SUBSCRIBE_TYPE_NORMAL);

    if (g_mqtt_connected) {
        __agent_ai_bootstrap();
    }
    if (tuya_ai_agent_is_ready()) {
        s_agent_ai_client_ready = true;
    }
    __ai_chat_apply_mode_volume();
    tal_thread_delete(s_ai_init_thrd);
    s_ai_init_thrd = NULL;
}

/**
 * @brief Start AI chat init on a dedicated thread (button stack is too small)
 * @return OPRT_OK on success
 */
static OPERATE_RET __ai_chat_init_deferred(void)
{
    OPERATE_RET rt;
    THREAD_CFG_T cfg = {
        .priority    = THREAD_PRIO_3,
        .stackDepth  = 4096,
        .thrdname    = "ai_init",
    };

    if (s_ai_init_thrd != NULL || s_agent_ai_chat_ready) {
        return OPRT_OK;
    }

    rt = tal_thread_create_and_start(&s_ai_init_thrd, NULL, NULL, __ai_chat_init_thread, NULL, &cfg);
    if (rt != OPRT_OK) {
        PR_ERR("AI init thread create failed: %d", rt);
        s_ai_init_thrd = NULL;
    }
    return rt;
}

/**
 * @brief Button OK callback - controls pixel LED animations
 */
static void buzzer_button_ok_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    if (g_animation_mode == TETRIS_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
            g_tetris_input = 3;
        } else if (event == TDL_BUTTON_LONG_PRESS_START) {
            g_tetris_input = 4; // fast drop start
        } else if (event == TDL_BUTTON_PRESS_UP) {
            g_tetris_input = 5; // fast drop stop
        }
        return;
    }

    if (g_animation_mode == NINJA_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
            g_ninja_input = (g_ninja.phase == 0) ? 4 : 3;
        }
        return;
    }

    if (g_animation_mode == AGENT_MONITOR_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
            if (pixel_agent_bridge_perm_pending()) {
                pixel_agent_bridge_on_button_always();
            } else {
                pixel_agent_bridge_on_button_enter();
            }
            board_buzzer_play_note_duration(NOTE_C5, 40);
        } else if (event == TDL_BUTTON_LONG_PRESS_START) {
            PR_NOTICE("OK Button: start cloud STT (AI HOLD mode)");
            __agent_stt_start();
            board_buzzer_play_note_duration(NOTE_C5, 60);
        } else if (event == TDL_BUTTON_PRESS_UP) {
            PR_NOTICE("OK Button: stop cloud STT");
            __agent_stt_stop();
            board_buzzer_play_note_duration(NOTE_G5, 40);
        }
        return;
    }

    if (g_animation_mode == AI_CHAT_SPECTRUM_MODE) {
        if (event == TDL_BUTTON_LONG_PRESS_START) {
            PR_NOTICE("OK Button: start AI chat (hold-to-talk)");
            __ai_chat_hold_start();
            board_buzzer_play_note_duration(NOTE_C5, 60);
        } else if (event == TDL_BUTTON_PRESS_UP) {
            PR_NOTICE("OK Button: stop AI chat recording");
            __ai_chat_hold_stop();
            board_buzzer_play_note_duration(NOTE_G5, 40);
        }
        return;
    }

    if (event == TDL_BUTTON_PRESS_SINGLE_CLICK) {
        uint32_t total_animations = EFFECT_ANIMATION_COUNT + g_registered_pixel_art_count;
        g_animation_mode = (g_animation_mode + 1) % total_animations;
        PR_NOTICE("OK Button: Changed to animation mode %d", g_animation_mode);
    } else if (event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
        g_animation_mode = AGENT_MONITOR_MODE;
        g_animation_loop = true;
        if (!s_agent_ai_chat_ready) {
            if (__ai_chat_init_deferred() == OPRT_OK) {
                PR_NOTICE("OK Button: agent monitor mode - AI init started");
            } else {
                PR_ERR("OK Button: agent monitor mode - AI init thread failed");
            }
        } else {
            __ai_chat_apply_mode_volume();
            PR_NOTICE("OK Button: agent monitor mode (Cursor/Claude via USB)");
        }
        board_buzzer_play_note_duration(NOTE_E5, 50);
    } else if (event == TDL_BUTTON_LONG_PRESS_START) {
        g_ninja.initialized = false;
        g_animation_mode = NINJA_MODE;
        PR_NOTICE("OK Button: Switched to Ninja runner mode");
    } else if (event == TDL_BUTTON_PRESS_UP) {
    }
}

/**
 * @brief Button A callback - switches between pixel art animations
 */
static void buzzer_button_a_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    if (g_animation_mode == TETRIS_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
            g_tetris_input = 1; // left
        }
        return;
    }

    if (g_animation_mode == SNAKE_MODE) {
        return; // Snake uses IMU, no button input needed
    }

    if (g_animation_mode == NINJA_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
            g_ninja_input = 1; // below line / speed down
        }
        return;
    }

    if (g_animation_mode == AGENT_MONITOR_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
            if (pixel_agent_bridge_perm_pending()) {
                pixel_agent_bridge_on_button_allow();
            } else {
                pixel_agent_bridge_on_button_backspace();
            }
            board_buzzer_play_note_duration(NOTE_G5, 40);
        }
        return;
    }

    if (g_animation_mode == AI_CHAT_SPECTRUM_MODE) {
        return;
    }

    if (event == TDL_BUTTON_LONG_PRESS_START) {
        // Long press A: enter Snake game
        g_snake.initialized = false;
        g_animation_mode = SNAKE_MODE;
        PR_NOTICE("A Button: Switched to Snake mode");
        return;
    }

    if (event == TDL_BUTTON_PRESS_SINGLE_CLICK) {
        g_pixel_art_index = (g_pixel_art_index + 1) % g_registered_pixel_art_count;
        g_animation_mode = EFFECT_ANIMATION_COUNT + g_pixel_art_index;
        PR_NOTICE("A Button: Changed to pixel art %d (mode %d)", g_pixel_art_index, g_animation_mode);
    } else if (event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
        PR_NOTICE("A Button: Double click - entering AI chat spectrum mode");
        g_animation_mode = AI_CHAT_SPECTRUM_MODE;
        g_animation_loop = true;
        g_spectrum_ok_holding = false;
        if (!s_agent_ai_chat_ready) {
            if (__ai_chat_init_deferred() == OPRT_OK) {
                PR_NOTICE("A Button: AI chat mode - init started");
            } else {
                PR_ERR("A Button: AI chat mode - init thread failed");
            }
        } else {
            __ai_chat_apply_mode_volume();
        }
        board_buzzer_play_note_duration(NOTE_E5, 50);
    }
}

/**
 * @brief Button B callback
 * In games: single/double click = game input, long press = return to home
 * Non-game: long press = enter Tetris, double click = Sand Physics
 */
static void buzzer_button_b_cb(char *name, TDL_BUTTON_TOUCH_EVENT_E event, void *argc)
{
    if (g_animation_mode == AGENT_MONITOR_MODE) {
        if (event == TDL_BUTTON_PRESS_SINGLE_CLICK) {
            if (pixel_agent_bridge_perm_pending()) {
                pixel_agent_bridge_on_button_deny();
            } else {
                pixel_agent_bridge_on_button_clear();
            }
            board_buzzer_play_note_duration(NOTE_A4, 40);
        } else if (event == TDL_BUTTON_LONG_PRESS_START) {
            /* Long press B: exit agent monitor mode */
            g_animation_mode = HOME_ANIMATION_MODE;
            g_animation_loop = false;
            PR_NOTICE("B Button: exit agent monitor mode");
            board_buzzer_play_note_duration(NOTE_A4, 40);
        }
        return;
    }

    if (g_animation_mode == AI_CHAT_SPECTRUM_MODE) {
        if (event == TDL_BUTTON_LONG_PRESS_START) {
            g_spectrum_ok_holding = false;
            g_animation_mode = HOME_ANIMATION_MODE;
            g_animation_loop = false;
            PR_NOTICE("B Button: exit AI chat spectrum mode");
            board_buzzer_play_note_duration(NOTE_A4, 40);
        }
        return;
    }

    // In Tetris
    if (g_animation_mode == TETRIS_MODE) {
        if (g_tetris.phase == 0 || g_tetris.phase == 1) {
            if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
                g_tetris_input = 2; // phase 0: faster, phase 1: right
                return;
            }
        }
        if (event == TDL_BUTTON_LONG_PRESS_START) {
            board_buzzer_stop_sequence();
            board_buzzer_stop();
            g_tetris.initialized = false;
            g_animation_mode = HOME_ANIMATION_MODE;
            PR_NOTICE("B Button: Long press - return to home from Tetris");
        }
        return;
    }

    // In Snake
    if (g_animation_mode == SNAKE_MODE) {
        if (event == TDL_BUTTON_LONG_PRESS_START) {
            board_buzzer_stop_sequence();
            board_buzzer_stop();
            g_snake.initialized = false;
            g_animation_mode = HOME_ANIMATION_MODE;
            PR_NOTICE("B Button: Long press - return to home from Snake");
        }
        return;
    }

    // In Ninja
    if (g_animation_mode == NINJA_MODE) {
        if (g_ninja.phase == 0 || g_ninja.phase == 1) {
            if (event == TDL_BUTTON_PRESS_SINGLE_CLICK || event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
                g_ninja_input = 2; // phase 0: difficulty up, phase 1: above line
                return;
            }
        }
        if (event == TDL_BUTTON_LONG_PRESS_START) {
            board_buzzer_stop_sequence();
            board_buzzer_stop();
            g_ninja.initialized = false;
            g_animation_mode = HOME_ANIMATION_MODE;
            PR_NOTICE("B Button: Long press - return to home from Ninja");
        }
        return;
    }

    // Non-game modes
    if (event == TDL_BUTTON_LONG_PRESS_START) {
        // Long press B in non-game: enter Tetris
        g_tetris.initialized = false;
        g_animation_mode = TETRIS_MODE;
        PR_NOTICE("B Button: Long press - entering Tetris mode");
    } else if (event == TDL_BUTTON_PRESS_DOUBLE_CLICK) {
        // Double click B: enter Sand Physics
        g_sand_initialized = false;
        g_animation_mode = SAND_PHYSICS_MODE;
        PR_NOTICE("B Button: Double click - entering Sand Physics mode");
    }
}

/**
 * @brief Initialize buttons and register callbacks
 */
static void buzzer_demo_init_buttons(void)
{
    OPERATE_RET rt = OPRT_OK;
    TDL_BUTTON_CFG_T button_cfg = {.long_start_valid_time = 1000, // 1 second for long press
                                   .long_keep_timer = 500,
                                   .button_debounce_time = 50,
                                   .button_repeat_valid_count = 2,
                                   .button_repeat_valid_time = 500};

    // Initialize OK button
    rt = tdl_button_create(BUTTON_NAME, &button_cfg, &g_button_ok_handle);
    if (OPRT_OK == rt) {
        tdl_button_event_register(g_button_ok_handle, TDL_BUTTON_PRESS_SINGLE_CLICK, buzzer_button_ok_cb);
        tdl_button_event_register(g_button_ok_handle, TDL_BUTTON_PRESS_DOUBLE_CLICK, buzzer_button_ok_cb);
        tdl_button_event_register(g_button_ok_handle, TDL_BUTTON_LONG_PRESS_START, buzzer_button_ok_cb);
        tdl_button_event_register(g_button_ok_handle, TDL_BUTTON_PRESS_UP, buzzer_button_ok_cb);
        PR_NOTICE("OK button initialized");
    } else {
        PR_ERR("Failed to create OK button: %d", rt);
    }

    // Initialize A button
    rt = tdl_button_create(BUTTON_NAME_2, &button_cfg, &g_button_a_handle);
    if (OPRT_OK == rt) {
        tdl_button_event_register(g_button_a_handle, TDL_BUTTON_PRESS_SINGLE_CLICK, buzzer_button_a_cb);
        tdl_button_event_register(g_button_a_handle, TDL_BUTTON_PRESS_DOUBLE_CLICK, buzzer_button_a_cb);
        tdl_button_event_register(g_button_a_handle, TDL_BUTTON_LONG_PRESS_START, buzzer_button_a_cb);
        tdl_button_event_register(g_button_a_handle, TDL_BUTTON_PRESS_UP, buzzer_button_a_cb);
        PR_NOTICE("A button initialized");
    } else {
        PR_ERR("Failed to create A button: %d", rt);
    }

    // Initialize B button
    PR_NOTICE("Initializing B button with name: %s", BUTTON_NAME_3);
    rt = tdl_button_create(BUTTON_NAME_3, &button_cfg, &g_button_b_handle);
    if (OPRT_OK == rt) {
        PR_NOTICE("B button created successfully, handle: %p", g_button_b_handle);
        // Register all possible events to catch any button activity
        tdl_button_event_register(g_button_b_handle, TDL_BUTTON_PRESS_DOWN, buzzer_button_b_cb);
        tdl_button_event_register(g_button_b_handle, TDL_BUTTON_PRESS_UP, buzzer_button_b_cb);
        tdl_button_event_register(g_button_b_handle, TDL_BUTTON_PRESS_SINGLE_CLICK, buzzer_button_b_cb);
        tdl_button_event_register(g_button_b_handle, TDL_BUTTON_PRESS_DOUBLE_CLICK, buzzer_button_b_cb);
        tdl_button_event_register(g_button_b_handle, TDL_BUTTON_LONG_PRESS_START, buzzer_button_b_cb);
        PR_NOTICE("B button initialized and all events registered successfully");
    } else {
        PR_ERR("Failed to create B button '%s': %d", BUTTON_NAME_3, rt);
        PR_ERR("Make sure BUTTON_NAME_3 is registered in board_register_hardware()");
    }
}

/**
 * @brief Initialize pixel LED driver using BSP
 */
static OPERATE_RET pixel_led_init(void)
{
    OPERATE_RET rt = OPRT_OK;

    tal_system_sleep(100);
    rt = board_pixel_get_handle(&g_pixels_handle);
    if (OPRT_OK != rt) {
        PR_ERR("Failed to get pixel device handle: %d", rt);
        return rt;
    }

    PR_NOTICE("Pixel LED initialized: %d pixels", LED_PIXELS_TOTAL_NUM);

    rt = pixel_agent_clawd_init();
    if (OPRT_OK != rt) {
        PR_WARN("Clawd agent GIF init failed (%d) — agent mode uses standby pulse", rt);
    }

    return OPRT_OK;
}

/**
 * @brief Breathing color effect
 */
static void __breathing_color_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }

    static const PIXEL_COLOR_T cCOLOR_ARR[] = {
        {.warm = 0, .cold = 0, .red = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .green = 0, .blue = 0},
        {.warm = 0, .cold = 0, .red = 0, .green = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .blue = 0},
        {.warm = 0, .cold = 0, .red = 0, .green = 0, .blue = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS)},
    };

    static uint32_t static_intensity = 0;
    static int32_t static_direction = 1;
    static uint32_t static_cycle_count = 0;
    static uint32_t static_color_index = 0;
    static bool animation_complete = false;
    static uint32_t max_cycles = 3;
    uint32_t step = 20;
    uint32_t color_num = 3;

    if (animation_complete && !g_animation_loop) {
        return;
    }

    if (animation_complete) {
        static_intensity = 0;
        static_direction = 1;
        static_cycle_count = 0;
        static_color_index = 0;
        animation_complete = false;
    }

    static_intensity += (static_direction * step);

    if (static_intensity >= COLOR_RESOLUTION) {
        static_intensity = COLOR_RESOLUTION;
        static_direction = -1;
    } else if (static_intensity <= 0) {
        static_intensity = 0;
        static_direction = 1;
        static_cycle_count++;
        static_color_index = (static_color_index + 1) % color_num;

        if (static_cycle_count >= max_cycles) {
            animation_complete = true;
        }
    }

    PIXEL_COLOR_T current_color = {0};
    current_color.red = (cCOLOR_ARR[static_color_index].red * static_intensity) / COLOR_RESOLUTION;
    current_color.green = (cCOLOR_ARR[static_color_index].green * static_intensity) / COLOR_RESOLUTION;
    current_color.blue = (cCOLOR_ARR[static_color_index].blue * static_intensity) / COLOR_RESOLUTION;

    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &current_color);
    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Running light effect
 */
static void __running_light_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static const PIXEL_COLOR_T cCOLOR_ARR[] = {
        {.warm = 0, .cold = 0, .red = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .green = 0, .blue = 0},
        {.warm = 0, .cold = 0, .red = 0, .green = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .blue = 0},
        {.warm = 0, .cold = 0, .red = 0, .green = 0, .blue = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS)},
    };

    static uint32_t current_led = 1;
    static uint32_t cycle_count = 0;
    static uint32_t max_cycles = 1;
    static uint32_t color_index = 0;
    static uint32_t color_num = 3;
    static uint32_t color_change_interval = 50;
    static bool animation_complete = false;

    if (animation_complete && !g_animation_loop) {
        return;
    }

    if (animation_complete) {
        current_led = 1;
        cycle_count = 0;
        color_index = 0;
        animation_complete = false;
    }

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    if ((current_led - 1) % color_change_interval == 0) {
        color_index = (color_index + 1) % color_num;
    }

    PIXEL_COLOR_T current_color = cCOLOR_ARR[color_index];
    tdl_pixel_set_single_color(g_pixels_handle, current_led, 1, &current_color);
    tdl_pixel_dev_refresh(g_pixels_handle);

    current_led++;
    if (current_led > 1023) {
        current_led = 1;
        cycle_count++;
        if (cycle_count >= max_cycles) {
            animation_complete = true;
        }
    }
}

/**
 * @brief Color wave effect
 */
static void __color_wave_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static const PIXEL_COLOR_T cCOLOR_ARR[] = {
        {.warm = 0, .cold = 0, .red = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .green = 0, .blue = 0},
        {.warm = 0, .cold = 0, .red = 0, .green = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .blue = 0},
        {.warm = 0, .cold = 0, .red = 0, .green = 0, .blue = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS)},
    };

    static uint32_t wave_position = 0;
    static uint32_t cycle_count = 0;
    static uint32_t max_cycles = 2;
    static bool animation_complete = false;
    uint32_t wave_length = 20;
    uint32_t color_num = 3;

    if (animation_complete && !g_animation_loop) {
        return;
    }

    if (animation_complete) {
        wave_position = 0;
        cycle_count = 0;
        animation_complete = false;
    }

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    for (uint32_t i = 0; i < wave_length; i++) {
        uint32_t led_pos = (wave_position + i) % LED_PIXELS_TOTAL_NUM;
        uint32_t color_index = (i * color_num) / wave_length;
        PIXEL_COLOR_T current_color = cCOLOR_ARR[color_index];
        tdl_pixel_set_single_color(g_pixels_handle, led_pos, 1, &current_color);
    }

    tdl_pixel_dev_refresh(g_pixels_handle);

    wave_position++;
    if (wave_position >= LED_PIXELS_TOTAL_NUM) {
        wave_position = 0;
        cycle_count++;
        if (cycle_count >= max_cycles) {
            animation_complete = true;
        }
    }
}

/**
 * @brief 2D wave effect
 */
static void __2d_wave_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static uint32_t static_cycle_count = 0;
    static float static_wave_radius = 0.0f;
    static float static_color_hue = 0.0f;
    static bool animation_complete = false;
    uint32_t max_cycles = 2;
    float max_radius = 23.0f;
    float wave_speed = 0.5f;

    if (animation_complete && !g_animation_loop) {
        return;
    }

    if (animation_complete) {
        static_cycle_count = 0;
        static_wave_radius = 0.0f;
        static_color_hue = 0.0f;
        animation_complete = false;
    }

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    static_wave_radius += wave_speed;
    if (static_wave_radius > max_radius) {
        static_wave_radius = 0.0f;
        static_cycle_count++;
        if (static_cycle_count >= max_cycles) {
            animation_complete = true;
        }
    }

    static_color_hue += 2.0f;
    if (static_color_hue >= 360.0f) {
        static_color_hue = 0.0f;
    }

    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            float dx = (float)x - 15.5f;
            float dy = (float)y - 15.5f;
            float distance = sqrtf(dx * dx + dy * dy);

            if (distance <= static_wave_radius) {
                float distance_hue = (distance / max_radius) * 180.0f;
                float current_hue = static_color_hue - distance_hue;
                if (current_hue < 0.0f)
                    current_hue += 360.0f;

                PIXEL_COLOR_T color =
                    board_pixel_hsv_to_pixel_color(current_hue, 1.0f, 1.0f, BRIGHTNESS, COLOR_RESOLUTION);

                uint32_t led_index = board_pixel_matrix_coord_to_led_index(x, y);
                if (led_index < LED_PIXELS_TOTAL_NUM) {
                    tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &color);
                }
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Snowflake effect
 */
static void __snowflake_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static float angle = 0.0f;
    angle += 0.05f;

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            float dx = (float)x - 15.5f;
            float dy = (float)y - 15.5f;
            float distance = sqrtf(dx * dx + dy * dy);
            float point_angle = atan2f(dy, dx) + angle;

            float snowflake = sinf(6.0f * point_angle) * 0.3f + 0.7f;
            float radius = 12.0f * snowflake;

            if (distance <= radius) {
                float intensity = 1.0f - (distance / radius) * 0.3f;
                PIXEL_COLOR_T color = {.red = (uint32_t)(COLOR_RESOLUTION * intensity * 0.9f * BRIGHTNESS),
                                       .green = (uint32_t)(COLOR_RESOLUTION * intensity * 0.9f * BRIGHTNESS),
                                       .blue = (uint32_t)(COLOR_RESOLUTION * intensity * 1.0f * BRIGHTNESS),
                                       .warm = 0,
                                       .cold = (uint32_t)(COLOR_RESOLUTION * intensity * 0.6f * BRIGHTNESS)};

                uint32_t led_index = board_pixel_matrix_coord_to_led_index(x, y);
                if (led_index < LED_PIXELS_TOTAL_NUM) {
                    tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &color);
                }
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Breathing circle effect
 */
static void __breathing_circle_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static float breath = 0.0f;
    breath += 0.1f;
    float radius = 6.0f + 4.0f * sinf(breath);

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            float dx = (float)x - 15.5f;
            float dy = (float)y - 15.5f;
            float distance = sqrtf(dx * dx + dy * dy);

            if (distance <= radius) {
                float intensity = 1.0f - (distance / radius) * 0.5f;
                float hue = fmodf((breath * 0.5f + distance * 0.3f) * 60.0f, 360.0f);
                float saturation = 0.9f;
                float value = intensity;

                PIXEL_COLOR_T color =
                    board_pixel_hsv_to_pixel_color(hue, saturation, value, BRIGHTNESS, COLOR_RESOLUTION);

                uint32_t led_index = board_pixel_matrix_coord_to_led_index(x, y);
                if (led_index < LED_PIXELS_TOTAL_NUM) {
                    tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &color);
                }
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Ripple effect
 */
static void __ripple_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static float time = 0.0f;
    static float ripple_center_x = 16.0f, ripple_center_y = 16.0f;
    time += 0.2f;

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    for (uint32_t y = 0; y < 32; y++) {
        for (uint32_t x = 0; x < 32; x++) {
            float dx = (float)x - ripple_center_x;
            float dy = (float)y - ripple_center_y;
            float distance = sqrtf(dx * dx + dy * dy);

            float ripple = sinf(distance * 0.8f - time * 2.0f) * 0.5f + 0.5f;

            if (ripple > 0.3f) {
                float intensity = (ripple - 0.3f) / 0.7f;
                PIXEL_COLOR_T color = {.red = (uint32_t)(COLOR_RESOLUTION * intensity * 0.1f * BRIGHTNESS),
                                       .green = (uint32_t)(COLOR_RESOLUTION * intensity * 0.6f * BRIGHTNESS),
                                       .blue = (uint32_t)(COLOR_RESOLUTION * intensity * 1.0f * BRIGHTNESS),
                                       .warm = 0,
                                       .cold = (uint32_t)(COLOR_RESOLUTION * intensity * 0.8f * BRIGHTNESS)};

                uint32_t led_index = board_pixel_matrix_coord_to_led_index(x, y);
                if (led_index < LED_PIXELS_TOTAL_NUM) {
                    tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &color);
                }
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Scan animation effect
 */
static void __scan_animation_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    static uint32_t frame_count = 0;
    static uint32_t column_index = 0;
    static uint32_t row_index = 0;
    static bool column_phase = true;

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    frame_count++;
    if (frame_count >= 10) {
        frame_count = 0;

        if (column_phase) {
            column_index++;
            if (column_index >= 32) {
                column_index = 0;
                column_phase = false;
            }
        } else {
            row_index++;
            if (row_index >= 32) {
                row_index = 0;
                column_phase = true;
            }
        }
    }

    if (column_phase) {
        PIXEL_COLOR_T red_color = {
            .red = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .green = 0, .blue = 0, .warm = 0, .cold = 0};
        for (uint32_t y = 0; y < 32; y++) {
            uint32_t led_index = board_pixel_matrix_coord_to_led_index(column_index, y);
            if (led_index < LED_PIXELS_TOTAL_NUM) {
                tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &red_color);
            }
        }
    } else {
        PIXEL_COLOR_T blue_color = {
            .red = 0, .green = 0, .blue = (uint32_t)(COLOR_RESOLUTION * BRIGHTNESS), .warm = 0, .cold = 0};
        for (uint32_t x = 0; x < 32; x++) {
            uint32_t led_index = board_pixel_matrix_coord_to_led_index(x, row_index);
            if (led_index < LED_PIXELS_TOTAL_NUM) {
                tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &blue_color);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Render a single character
 */
static void render_char(int32_t x, int32_t y, char ch, float hue)
{
    char upper_ch = (ch >= 'a' && ch <= 'z') ? (ch - 'a' + 'A') : ch;
    const LED_FONT_CHAR_T *font_char = get_font_char(upper_ch);

    for (int row = 0; row < 8; row++) {
        int display_y = y + row;
        if (display_y < 0 || display_y >= 32)
            continue;

        uint8_t row_data = font_char->data[row];
        for (int col = 0; col < 8; col++) {
            int display_x = x + col;
            if (display_x < 0 || display_x >= 32)
                continue;

            if (row_data & (0x80 >> col)) {
                float pixel_hue = fmodf(hue + (float)display_x * 12.0f, 360.0f);

                PIXEL_COLOR_T color =
                    board_pixel_hsv_to_pixel_color(pixel_hue, 1.0f, 1.0f, BRIGHTNESS, COLOR_RESOLUTION);

                uint32_t led_index = board_pixel_matrix_coord_to_led_index((uint32_t)display_x, (uint32_t)display_y);
                if (led_index < LED_PIXELS_TOTAL_NUM) {
                    tdl_pixel_set_single_color(g_pixels_handle, led_index, 1, &color);
                }
            }
        }
    }
}

/**
 * @brief Scrolling text effect
 */
static void __scrolling_text_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }
    const char *message = "Hi! it's TuyaOpen";

    static int32_t scroll_pos = 32;
    static float base_hue = 0.0f;
    static uint32_t frame_count = 0;
    static uint32_t text_width = 0;
    static bool text_width_calculated = false;

    if (!text_width_calculated) {
        text_width = calculate_text_width(message);
        text_width_calculated = true;
    }

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    frame_count++;
    if (frame_count >= 1) {
        frame_count = 0;
        scroll_pos--;
        if (scroll_pos < -(int32_t)text_width) {
            scroll_pos = 32;
        }
    }

    int32_t char_x = scroll_pos;
    for (const char *p = message; *p; p++) {
        char ch = *p;
        const LED_FONT_CHAR_T *font_char = get_font_char((ch >= 'a' && ch <= 'z') ? (ch - 'a' + 'A') : ch);

        if (char_x + (int32_t)font_char->width >= 0 && char_x < 32) {
            render_char(char_x, 12, ch, base_hue);
        }

        char_x += font_char->width;
    }

    tdl_pixel_dev_refresh(g_pixels_handle);

    base_hue += 3.0f;
    if (base_hue > 360.0f)
        base_hue -= 360.0f;
}

/**
 * @brief Render pixel art animation
 */
static void __pixel_art_effect(const pixel_art_t *art)
{
    if (g_pixels_handle == NULL || art == NULL) {
        return;
    }

    static uint32_t frame_index[MAX_PIXEL_ART_ANIMATIONS] = {0}; // Separate frame index for each pixel art
    static uint32_t frame_counter = 0;
    static int last_art_index = -1;
    const uint32_t frame_delay = 0; // Delay between frames (2 * 50ms = 100ms per frame) - faster animation

    // Find the art index in the registered array by comparing pointers
    int art_index = -1;
    for (uint32_t i = 0; i < g_registered_pixel_art_count; i++) {
        if (g_registered_pixel_arts[i] == art) {
            art_index = (int)i;
            break;
        }
    }

    if (art_index < 0) {
        PR_ERR("Pixel art not found in registered animations");
        return; // Unknown pixel art
    }

    // Reset frame index if switching to a different pixel art
    if (last_art_index != art_index) {
        frame_index[art_index] = 0;
        last_art_index = art_index;
    }

    // Get the appropriate frame index for this pixel art
    uint32_t current_frame_index = frame_index[art_index];

    // Clear all pixels
    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    // Get current frame
    if (current_frame_index >= art->frame_count) {
        current_frame_index = 0;
    }

    const pixel_frame_t *frame = &art->frames[current_frame_index];
    const pixel_rgb_t *pixels = frame->pixels;

    // Render frame to LED matrix (32x32)
    for (uint32_t y = 0; y < frame->height && y < 32; y++) {
        for (uint32_t x = 0; x < frame->width && x < 32; x++) {
            uint32_t pixel_idx = y * frame->width + x;
            if (pixel_idx >= (frame->width * frame->height)) {
                continue;
            }

            const pixel_rgb_t *pixel = &pixels[pixel_idx];

            // Convert RGB (0-255) to COLOR_RESOLUTION scale with brightness
            // Note: LED hardware expects GRB order, so we swap red and green
            PIXEL_COLOR_T color = {0};
            color.red =
                (uint32_t)((pixel->g * COLOR_RESOLUTION * BRIGHTNESS) / 255); // Swap: red channel gets green data
            color.green =
                (uint32_t)((pixel->r * COLOR_RESOLUTION * BRIGHTNESS) / 255); // Swap: green channel gets red data
            color.blue = (uint32_t)((pixel->b * COLOR_RESOLUTION * BRIGHTNESS) / 255); // Blue stays the same

            // Convert 2D matrix coordinates to 1D LED index
            uint32_t led_idx = board_pixel_matrix_coord_to_led_index(x, y);
            if (led_idx < LED_PIXELS_TOTAL_NUM) {
                tdl_pixel_set_single_color(g_pixels_handle, led_idx, 1, &color);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);

    // Advance to next frame
    frame_counter++;
    if (frame_counter >= frame_delay) {
        frame_counter = 0;
        current_frame_index++;
        if (current_frame_index >= art->frame_count) {
            // Always loop pixel art animations
            current_frame_index = 0;
        }

        // Update the appropriate frame index
        frame_index[art_index] = current_frame_index;
    }
}

/**
 * @brief Update sand particle physics based on IMU sensor data
 * Uses classic falling-sand cellular automaton with diagonal spreading
 * and multi-step iteration for natural sand pile behavior.
 */
static void sand_update_physics(void)
{
    float acc_x = 0.0f, acc_y = 0.0f, acc_z = 0.0f;

    if (g_imu_ready && g_imu_handle) {
        board_bmi270_read_accel(g_imu_handle, &acc_x, &acc_y, &acc_z);
    }

    // Map accelerometer to screen gravity
    float gx = -acc_y;
    float gy = acc_x;

    float abs_gx = fabsf(gx);
    float abs_gy = fabsf(gy);

    // Quantize primary gravity direction per axis
    int8_t dir_x = 0, dir_y = 0;
    if (gx > 0.5f) dir_x = 1;
    else if (gx < -0.5f) dir_x = -1;
    if (gy > 0.5f) dir_y = 1;
    else if (gy < -0.5f) dir_y = -1;
    if (dir_x == 0 && dir_y == 0) return;

    // Per-axis step count: same tilt angle → same speed regardless of direction
    int steps_x = (dir_x != 0) ? (int)(abs_gx / 2.0f) + 1 : 0;
    int steps_y = (dir_y != 0) ? (int)(abs_gy / 2.0f) + 1 : 0;
    if (steps_x > 5) steps_x = 5;
    if (steps_y > 5) steps_y = 5;
    int max_steps = (steps_x > steps_y) ? steps_x : steps_y;
    if (max_steps <= 0) return;

    // Scan order: process from gravity "bottom" so leading-edge particles move first
    int sx, ex, dx, sy, ey, dy;
    if (dir_x > 0) { sx = MATRIX_WIDTH - 1; ex = -1; dx = -1; }
    else { sx = 0; ex = MATRIX_WIDTH; dx = 1; }
    if (dir_y > 0) { sy = MATRIX_HEIGHT - 1; ey = -1; dy = -1; }
    else { sy = 0; ey = MATRIX_HEIGHT; dy = 1; }

    for (int step = 0; step < max_steps; step++) {
        // Determine which axes are active this step
        int8_t mx = (step < steps_x) ? dir_x : 0;
        int8_t my = (step < steps_y) ? dir_y : 0;
        if (mx == 0 && my == 0) break;

        // Compute spread and wall-slide directions based on active movement
        int8_t spr_ax, spr_ay, spr_bx, spr_by;
        int8_t wall_ax, wall_ay, wall_bx, wall_by;
        if (mx != 0 && my != 0) {
            spr_ax = mx; spr_ay = 0;
            spr_bx = 0;  spr_by = my;
            wall_ax = -my; wall_ay = mx;
            wall_bx = my;  wall_by = -mx;
        } else if (mx != 0) {
            spr_ax = mx; spr_ay = 1;
            spr_bx = mx; spr_by = -1;
            wall_ax = 0; wall_ay = 1;
            wall_bx = 0; wall_by = -1;
        } else {
            spr_ax = 1;  spr_ay = my;
            spr_bx = -1; spr_by = my;
            wall_ax = 1;  wall_ay = 0;
            wall_bx = -1; wall_by = 0;
        }

        for (int y = sy; y != ey; y += dy) {
            for (int x = sx; x != ex; x += dx) {
                int16_t idx = g_sand_grid[x][y];
                if (idx < 0) continue;

                int nx, ny;

                // 1) Try primary direction (mx, my)
                nx = x + mx; ny = y + my;
                if (nx >= 0 && nx < MATRIX_WIDTH && ny >= 0 && ny < MATRIX_HEIGHT
                    && g_sand_grid[nx][ny] < 0) {
                    g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                    g_sand_particles[idx].x = (int8_t)nx;
                    g_sand_particles[idx].y = (int8_t)ny;
                    continue;
                }

                // 2) If diagonal primary, try each axis alone
                if (mx != 0 && my != 0) {
                    nx = x + mx; ny = y;
                    if (nx >= 0 && nx < MATRIX_WIDTH && g_sand_grid[nx][ny] < 0) {
                        g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                        g_sand_particles[idx].x = (int8_t)nx;
                        g_sand_particles[idx].y = (int8_t)ny;
                        continue;
                    }
                    nx = x; ny = y + my;
                    if (ny >= 0 && ny < MATRIX_HEIGHT && g_sand_grid[nx][ny] < 0) {
                        g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                        g_sand_particles[idx].x = (int8_t)nx;
                        g_sand_particles[idx].y = (int8_t)ny;
                        continue;
                    }
                }

                // 3) Diagonal spread (randomize to form natural slopes)
                int8_t sa_x, sa_y, sb_x, sb_y;
                if (rand() & 1) {
                    sa_x = spr_ax; sa_y = spr_ay; sb_x = spr_bx; sb_y = spr_by;
                } else {
                    sa_x = spr_bx; sa_y = spr_by; sb_x = spr_ax; sb_y = spr_ay;
                }
                nx = x + sa_x; ny = y + sa_y;
                if (nx >= 0 && nx < MATRIX_WIDTH && ny >= 0 && ny < MATRIX_HEIGHT
                    && g_sand_grid[nx][ny] < 0) {
                    g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                    g_sand_particles[idx].x = (int8_t)nx;
                    g_sand_particles[idx].y = (int8_t)ny;
                    continue;
                }
                nx = x + sb_x; ny = y + sb_y;
                if (nx >= 0 && nx < MATRIX_WIDTH && ny >= 0 && ny < MATRIX_HEIGHT
                    && g_sand_grid[nx][ny] < 0) {
                    g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                    g_sand_particles[idx].x = (int8_t)nx;
                    g_sand_particles[idx].y = (int8_t)ny;
                    continue;
                }

                // 4) Wall sliding: perpendicular scatter
                if (rand() & 1) {
                    sa_x = wall_ax; sa_y = wall_ay; sb_x = wall_bx; sb_y = wall_by;
                } else {
                    sa_x = wall_bx; sa_y = wall_by; sb_x = wall_ax; sb_y = wall_ay;
                }
                nx = x + sa_x; ny = y + sa_y;
                if (nx >= 0 && nx < MATRIX_WIDTH && ny >= 0 && ny < MATRIX_HEIGHT
                    && g_sand_grid[nx][ny] < 0) {
                    g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                    g_sand_particles[idx].x = (int8_t)nx;
                    g_sand_particles[idx].y = (int8_t)ny;
                    continue;
                }
                nx = x + sb_x; ny = y + sb_y;
                if (nx >= 0 && nx < MATRIX_WIDTH && ny >= 0 && ny < MATRIX_HEIGHT
                    && g_sand_grid[nx][ny] < 0) {
                    g_sand_grid[nx][ny] = idx; g_sand_grid[x][y] = -1;
                    g_sand_particles[idx].x = (int8_t)nx;
                    g_sand_particles[idx].y = (int8_t)ny;
                    continue;
                }
            }
        }
    }
}

/**
 * @brief Render sand particles (no border)
 */
static void sand_render(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }

    PIXEL_COLOR_T off_color = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off_color);

    for (uint32_t i = 0; i < SAND_MAX_PARTICLES; i++) {
        sand_particle_t *p = &g_sand_particles[i];
        if (!p->active) continue;

        int px = p->x;
        int py = p->y;
        if (px >= 0 && px < MATRIX_WIDTH && py >= 0 && py < MATRIX_HEIGHT) {
            uint32_t led_idx = board_pixel_matrix_coord_to_led_index((uint32_t)px, (uint32_t)py);
            if (led_idx < LED_PIXELS_TOTAL_NUM) {
                PIXEL_COLOR_T color = {0};
                color.red = (uint32_t)((p->g * COLOR_RESOLUTION * BRIGHTNESS) / 255);
                color.green = (uint32_t)((p->r * COLOR_RESOLUTION * BRIGHTNESS) / 255);
                color.blue = (uint32_t)((p->b * COLOR_RESOLUTION * BRIGHTNESS) / 255);
                tdl_pixel_set_single_color(g_pixels_handle, led_idx, 1, &color);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/***********************************************************
 * Tetris Game Implementation
 ***********************************************************/

// Tiny 3x5 digit font for score display (digits 0-9)
static const uint8_t g_tiny_digits[10][5] = {
    {0x7,0x5,0x5,0x5,0x7}, // 0
    {0x2,0x6,0x2,0x2,0x7}, // 1
    {0x7,0x1,0x7,0x4,0x7}, // 2
    {0x7,0x1,0x7,0x1,0x7}, // 3
    {0x5,0x5,0x7,0x1,0x1}, // 4
    {0x7,0x4,0x7,0x1,0x7}, // 5
    {0x7,0x4,0x7,0x5,0x7}, // 6
    {0x7,0x1,0x1,0x1,0x1}, // 7
    {0x7,0x5,0x7,0x5,0x7}, // 8
    {0x7,0x5,0x7,0x1,0x7}, // 9
};

/**
 * @brief Helper to set a pixel on the LED matrix with given RGB color
 */
static void tetris_set_pixel(int32_t sx, int32_t sy, uint8_t r, uint8_t g, uint8_t b)
{
    if (sx < 0 || sx >= 32 || sy < 0 || sy >= 32) return;
    uint32_t led_idx = board_pixel_matrix_coord_to_led_index((uint32_t)sx, (uint32_t)sy);
    if (led_idx >= LED_PIXELS_TOTAL_NUM) return;
    PIXEL_COLOR_T color = {0};
    color.red   = (uint32_t)((g * COLOR_RESOLUTION * BRIGHTNESS) / 255);
    color.green = (uint32_t)((r * COLOR_RESOLUTION * BRIGHTNESS) / 255);
    color.blue  = (uint32_t)((b * COLOR_RESOLUTION * BRIGHTNESS) / 255);
    tdl_pixel_set_single_color(g_pixels_handle, led_idx, 1, &color);
}

/**
 * @brief Draw a single 3x5 digit rotated 90° CCW (becomes 5 wide x 3 tall)
 * Original (col, row) → CCW rotated to (row, 2-col)
 */
static void tetris_draw_digit_ccw(int32_t sx, int32_t sy, uint8_t digit, uint8_t r, uint8_t g, uint8_t b)
{
    if (digit > 9) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = g_tiny_digits[digit][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (0x4 >> col)) {
                tetris_set_pixel(sx + row, sy + 2 - col, r, g, b);
            }
        }
    }
}

/**
 * @brief Draw a number rotated 90° CCW
 * In CCW rotation, reading direction is bottom→top, so MSB at bottom.
 * sy_bottom is the Y of the bottom-most digit. Number grows upward.
 * Each rotated digit is 5 wide x 3 tall, 4px vertical spacing.
 */
static void tetris_draw_number_ccw(int32_t sx, int32_t sy_bottom, uint32_t num, uint8_t r, uint8_t g, uint8_t b)
{
    char buf[6];
    int len = 0;
    if (num == 0) { buf[0] = 0; len = 1; }
    else {
        uint32_t tmp = num;
        while (tmp > 0 && len < 5) { buf[len++] = tmp % 10; tmp /= 10; }
    }
    // buf[0]=ones(LSB) at top, buf[len-1]=MSB at bottom (read bottom→top = correct order)
    for (int i = 0; i < len; i++) {
        tetris_draw_digit_ccw(sx, sy_bottom - (len - 1 - i) * 4, buf[i], r, g, b);
    }
}

/**
 * @brief Convert game coords to screen coords (90° CCW rotation)
 * Game: x=0..9 (cols), y=0..19 (rows, 0=top)
 * Screen after CCW: screen_x = (TETRIS_ROWS-1) - game_y, screen_y = game_x
 */
static inline void tetris_game_to_screen(int8_t gx, int8_t gy, int32_t *sx, int32_t *sy)
{
    *sx = TETRIS_BOARD_SX + gy;              // game y=0 at left, y=19 at right (fall left→right)
    *sy = TETRIS_BOARD_SY + gx;
}

/**
 * @brief Check collision
 */
static bool tetris_check_collision(int8_t type, int8_t rot, int8_t x, int8_t y)
{
    for (int i = 0; i < 4; i++) {
        int8_t bx = x + g_tetris_pieces[type][rot][i][0];
        int8_t by = y + g_tetris_pieces[type][rot][i][1];
        if (bx < 0 || bx >= TETRIS_COLS || by >= TETRIS_ROWS) return true;
        if (by >= 0 && g_tetris.grid[bx][by] != 0) return true;
    }
    return false;
}

/**
 * @brief Spawn a new piece at the top, promote next to current
 */
static void tetris_spawn_piece(void)
{
    g_tetris.cur_type = g_tetris.next_type;
    g_tetris.next_type = rand() % 7;
    g_tetris.cur_rot = 0;
    g_tetris.cur_x = TETRIS_COLS / 2 - 2;
    g_tetris.cur_y = -1;
    g_tetris.drop_timer = 0;

    if (tetris_check_collision(g_tetris.cur_type, g_tetris.cur_rot,
                               g_tetris.cur_x, g_tetris.cur_y + 1)) {
        g_tetris.phase = 2; // game over
    }
}

/**
 * @brief Lock current piece into the grid
 */
static void tetris_lock_piece(void)
{
    uint8_t color_idx = g_tetris.cur_type + 1;
    for (int i = 0; i < 4; i++) {
        int8_t bx = g_tetris.cur_x + g_tetris_pieces[g_tetris.cur_type][g_tetris.cur_rot][i][0];
        int8_t by = g_tetris.cur_y + g_tetris_pieces[g_tetris.cur_type][g_tetris.cur_rot][i][1];
        if (bx >= 0 && bx < TETRIS_COLS && by >= 0 && by < TETRIS_ROWS) {
            g_tetris.grid[bx][by] = color_idx;
        }
    }
}

// Short sound effects for games (non-blocking via async sequencer)
static const BUZZER_SEQ_ENTRY_T g_sfx_line_clear[] = {
    {NOTE_E5, 60, 0}, {NOTE_G5, 60, 0}, {NOTE_C6, 120, 0}
};
static BUZZER_SEQUENCE_T g_seq_line_clear = {
    .entries = g_sfx_line_clear, .count = 3, .loop = false
};

static const BUZZER_SEQ_ENTRY_T g_sfx_eat[] = {
    {NOTE_C5, 40, 0}, {NOTE_E5, 60, 0}
};
static BUZZER_SEQUENCE_T g_seq_eat = {
    .entries = g_sfx_eat, .count = 2, .loop = false
};

// Game over melody (descending notes)
static const BUZZER_SEQ_ENTRY_T g_sfx_game_over[] = {
    {NOTE_C5, 150, 0}, {NOTE_A4, 150, 0}, {NOTE_F4, 150, 0}, {NOTE_C4, 300, 0}
};
static BUZZER_SEQUENCE_T g_seq_game_over = {
    .entries = g_sfx_game_over, .count = 4, .loop = false
};

/**
 * @brief Clear completed lines
 */
static void tetris_clear_lines(void)
{
    bool cleared_any = false;
    for (int y = TETRIS_ROWS - 1; y >= 0; y--) {
        bool full = true;
        for (int x = 0; x < TETRIS_COLS; x++) {
            if (g_tetris.grid[x][y] == 0) { full = false; break; }
        }
        if (full) {
            cleared_any = true;
            for (int sy = y; sy > 0; sy--) {
                for (int x = 0; x < TETRIS_COLS; x++) {
                    g_tetris.grid[x][sy] = g_tetris.grid[x][sy - 1];
                }
            }
            for (int x = 0; x < TETRIS_COLS; x++) {
                g_tetris.grid[x][0] = 0;
            }
            g_tetris.score += 100;
            y++;
        }
    }
    if (cleared_any) {
        board_buzzer_play_sequence_async(&g_seq_line_clear);
    }
}

/**
 * @brief Process one Tetris game tick (phase 1 = playing)
 */
static void tetris_update(void)
{
    if (g_tetris.phase != 1) return;

    uint8_t input = g_tetris_input;
    g_tetris_input = 0;

    if (input == 1) { // left
        if (!tetris_check_collision(g_tetris.cur_type, g_tetris.cur_rot,
                                     g_tetris.cur_x - 1, g_tetris.cur_y))
            g_tetris.cur_x--;
    } else if (input == 2) { // right
        if (!tetris_check_collision(g_tetris.cur_type, g_tetris.cur_rot,
                                     g_tetris.cur_x + 1, g_tetris.cur_y))
            g_tetris.cur_x++;
    } else if (input == 3) { // rotate
        int8_t new_rot = (g_tetris.cur_rot + 1) % 4;
        if (!tetris_check_collision(g_tetris.cur_type, new_rot,
                                     g_tetris.cur_x, g_tetris.cur_y))
            g_tetris.cur_rot = new_rot;
    } else if (input == 4) { // long-press OK: fast drop on
        g_tetris.fast_drop = true;
    } else if (input == 5) { // release OK: fast drop off
        g_tetris.fast_drop = false;
    }

    uint32_t effective_speed = g_tetris.fast_drop ? 2 : g_tetris.drop_speed;
    g_tetris.drop_timer++;
    if (g_tetris.drop_timer >= effective_speed) {
        g_tetris.drop_timer = 0;
        if (!tetris_check_collision(g_tetris.cur_type, g_tetris.cur_rot,
                                     g_tetris.cur_x, g_tetris.cur_y + 1)) {
            g_tetris.cur_y++;
        } else {
            tetris_lock_piece();
            tetris_clear_lines();
            tetris_spawn_piece();
        }
    }
}

/**
 * @brief Render speed selection screen (phase 0)
 */
static void tetris_render_speed_select(void)
{
    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    // Vertical speed bar (symmetrically centered)
    uint32_t bar_len = (uint32_t)((TETRIS_DROP_MAX - g_tetris.drop_speed) * 20 / (TETRIS_DROP_MAX - TETRIS_DROP_MIN));
    for (uint32_t i = 0; i < 20; i++) {
        uint8_t r = (i < bar_len) ? 0 : 40;
        uint8_t g = (i < bar_len) ? 255 : 40;
        uint8_t b = 0;
        tetris_set_pixel(15, 6 + (int32_t)i, r, g, b);
        tetris_set_pixel(16, 6 + (int32_t)i, r, g, b);
    }

    // Speed value (ms) on right side, vertically centered
    uint32_t speed_ms = g_tetris.drop_speed * 20;
    tetris_draw_number_ccw(25, 19, speed_ms, 200, 200, 0);

    // A-/B+ hints
    tetris_set_pixel(15, 4, 100, 100, 255);  // A (slow) above
    tetris_set_pixel(16, 4, 100, 100, 255);
    tetris_set_pixel(15, 27, 255, 100, 100); // B (fast) below
    tetris_set_pixel(16, 27, 255, 100, 100);

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Render game playing / game over (phase 1 & 2)
 */
static void tetris_render_game(void)
{
    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    // --- Draw board border (dim gray) ---
    // After CCW rotation: board is 20 wide (x) x 10 tall (y)
    // Left wall (game y=19 maps to screen x = BOARD_SX)
    // Right wall (game y=0 maps to screen x = BOARD_SX + 19)
    // Bottom wall (game x=9 maps to screen y = BOARD_SY + 9)
    for (int i = -1; i <= TETRIS_ROWS; i++) {
        tetris_set_pixel(TETRIS_BOARD_SX + i, TETRIS_BOARD_SY - 1, 30, 30, 30);         // top border
        tetris_set_pixel(TETRIS_BOARD_SX + i, TETRIS_BOARD_SY + TETRIS_COLS, 30, 30, 30); // bottom border
    }
    for (int i = 0; i < TETRIS_COLS; i++) {
        tetris_set_pixel(TETRIS_BOARD_SX - 1, TETRIS_BOARD_SY + i, 30, 30, 30);          // left border
        tetris_set_pixel(TETRIS_BOARD_SX + TETRIS_ROWS, TETRIS_BOARD_SY + i, 30, 30, 30); // right border
    }

    // --- Draw placed blocks ---
    for (int gx = 0; gx < TETRIS_COLS; gx++) {
        for (int gy = 0; gy < TETRIS_ROWS; gy++) {
            uint8_t ci = g_tetris.grid[gx][gy];
            if (ci > 0) {
                int32_t sx, sy;
                tetris_game_to_screen(gx, gy, &sx, &sy);
                const uint8_t *c = g_tetris_colors[ci - 1];
                tetris_set_pixel(sx, sy, c[0], c[1], c[2]);
            }
        }
    }

    // --- Draw current falling piece ---
    if (g_tetris.phase == 1) {
        const uint8_t *c = g_tetris_colors[g_tetris.cur_type];
        for (int i = 0; i < 4; i++) {
            int8_t bx = g_tetris.cur_x + g_tetris_pieces[g_tetris.cur_type][g_tetris.cur_rot][i][0];
            int8_t by = g_tetris.cur_y + g_tetris_pieces[g_tetris.cur_type][g_tetris.cur_rot][i][1];
            if (by >= 0) {
                int32_t sx, sy;
                tetris_game_to_screen(bx, by, &sx, &sy);
                tetris_set_pixel(sx, sy, c[0], c[1], c[2]);
            }
        }
    }

    // --- Draw next piece preview at top-left ---
    {
        const uint8_t *c = g_tetris_colors[g_tetris.next_type];
        int32_t preview_x = 1;
        int32_t preview_y = 1;
        for (int i = 0; i < 4; i++) {
            int8_t px = g_tetris_pieces[g_tetris.next_type][0][i][0];
            int8_t py = g_tetris_pieces[g_tetris.next_type][0][i][1];
            tetris_set_pixel(preview_x + py, preview_y + px, c[0], c[1], c[2]);
        }
    }

    // --- Draw score at bottom-left (rotated 90° CCW, anchored at bottom) ---
    tetris_draw_number_ccw(0, 29, g_tetris.score, 200, 200, 200);

    // --- Game over flash ---
    if (g_tetris.phase == 2) {
        static uint32_t flash = 0;
        flash++;
        if ((flash / 15) % 2 == 0) {
            for (int i = 0; i < TETRIS_COLS; i++) {
                int32_t sx1, sy1, sx2, sy2;
                int gy1 = i * TETRIS_ROWS / TETRIS_COLS;
                int gy2 = TETRIS_ROWS - 1 - gy1;
                tetris_game_to_screen(i, gy1, &sx1, &sy1);
                tetris_game_to_screen(i, gy2, &sx2, &sy2);
                tetris_set_pixel(sx1, sy1, 255, 0, 0);
                tetris_set_pixel(sx2, sy2, 255, 0, 0);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Tetris game effect - main function called from animation loop
 */
static void __tetris_effect(void)
{
    if (g_pixels_handle == NULL) return;

    if (!g_tetris.initialized) {
        memset(&g_tetris, 0, sizeof(g_tetris));
        g_tetris_input = 0;
        g_tetris.drop_speed = TETRIS_DROP_DEFAULT;
        g_tetris.phase = 0; // speed select
        g_tetris.next_type = rand() % 7;
        g_tetris.initialized = true;
        PR_NOTICE("Tetris: speed select screen");
    }

    uint8_t input = g_tetris_input;

    if (g_tetris.phase == 0) {
        // Speed selection phase
        g_tetris_input = 0;
        if (input == 1) { // A = slower (increase frames)
            if (g_tetris.drop_speed < TETRIS_DROP_MAX)
                g_tetris.drop_speed += 5;
        } else if (input == 2) { // B = faster (decrease frames)
            if (g_tetris.drop_speed > TETRIS_DROP_MIN)
                g_tetris.drop_speed -= 5;
        } else if (input == 3) { // OK = start game
            g_tetris.phase = 1;
            tetris_spawn_piece();
            PR_NOTICE("Tetris: game started! speed=%d frames (%dms)", g_tetris.drop_speed, g_tetris.drop_speed * 20);
        }
        tetris_render_speed_select();
    } else if (g_tetris.phase == 1) {
        // Playing
        tetris_update();
        tetris_render_game();
    } else {
        // Game over - play melody and restart after 2 seconds
        static uint32_t gameover_timer = 0;
        if (gameover_timer == 0) {
            board_buzzer_play_sequence_async(&g_seq_game_over);
        }
        gameover_timer++;
        tetris_render_game();
        if (gameover_timer > 50) {
            uint8_t saved_speed = g_tetris.drop_speed;
            memset(&g_tetris, 0, sizeof(g_tetris));
            g_tetris.drop_speed = saved_speed;
            g_tetris.phase = 1;
            g_tetris.next_type = rand() % 7;
            g_tetris.initialized = true;
            tetris_spawn_piece();
            gameover_timer = 0;
        }
    }
}

/***********************************************************
 * Snake Game Implementation (IMU controlled)
 ***********************************************************/

/**
 * @brief Place food at a random position not occupied by the snake
 */
static void snake_place_food(void)
{
    // Build a simple occupied check
    for (int attempts = 0; attempts < 200; attempts++) {
        uint8_t fx = rand() % SNAKE_GRID_W;
        uint8_t fy = rand() % SNAKE_GRID_H;
        bool occupied = false;
        for (uint16_t i = 0; i < g_snake.len; i++) {
            uint16_t idx = (g_snake.head + SNAKE_MAX_LEN - i) % SNAKE_MAX_LEN;
            if (g_snake.x[idx] == fx && g_snake.y[idx] == fy) {
                occupied = true;
                break;
            }
        }
        if (!occupied) {
            g_snake.food_x = fx;
            g_snake.food_y = fy;
            return;
        }
    }
    // Fallback: just place it somewhere
    g_snake.food_x = rand() % SNAKE_GRID_W;
    g_snake.food_y = rand() % SNAKE_GRID_H;
}

/**
 * @brief Read IMU and determine snake direction
 */
static void snake_read_imu_direction(void)
{
    if (!g_imu_ready || !g_imu_handle) return;

    float ax = 0, ay = 0, az = 0;
    if (board_bmi270_read_accel(g_imu_handle, &ax, &ay, &az) != OPRT_OK) return;

    // Use tilt thresholds to determine direction
    // ax/ay in m/s² units. Threshold ~3 m/s² (~0.3g) to avoid jitter
    float threshold = 3.0f;

    // Determine strongest tilt axis
    float abs_ax = (ax > 0) ? ax : -ax;
    float abs_ay = (ay > 0) ? ay : -ay;

    snake_dir_t new_dir = g_snake.dir;
    if (abs_ax > abs_ay && abs_ax > threshold) {
        // X axis dominant tilt
        new_dir = (ax > 0) ? SNAKE_DOWN : SNAKE_UP;
    } else if (abs_ay > threshold) {
        // Y axis dominant tilt
        new_dir = (ay > 0) ? SNAKE_LEFT : SNAKE_RIGHT;
    }

    // Prevent reversing into self (opposite direction not allowed)
    bool opposite = (g_snake.dir == SNAKE_UP    && new_dir == SNAKE_DOWN)  ||
                    (g_snake.dir == SNAKE_DOWN  && new_dir == SNAKE_UP)    ||
                    (g_snake.dir == SNAKE_LEFT  && new_dir == SNAKE_RIGHT) ||
                    (g_snake.dir == SNAKE_RIGHT && new_dir == SNAKE_LEFT);
    if (!opposite) {
        g_snake.dir = new_dir;
    }
}

/**
 * @brief Update snake game state
 */
static void snake_update(void)
{
    if (g_snake.game_over) return;

    g_snake.move_timer++;
    if (g_snake.move_timer < SNAKE_MOVE_FRAMES) return;
    g_snake.move_timer = 0;

    // Read direction from IMU
    snake_read_imu_direction();

    // Calculate new head position
    uint16_t head_idx = g_snake.head;
    int16_t new_x = g_snake.x[head_idx];
    int16_t new_y = g_snake.y[head_idx];

    switch (g_snake.dir) {
        case SNAKE_UP:    new_y--; break;
        case SNAKE_DOWN:  new_y++; break;
        case SNAKE_LEFT:  new_x--; break;
        case SNAKE_RIGHT: new_x++; break;
    }

    // Wall wrap-around
    if (new_x < 0) new_x = SNAKE_GRID_W - 1;
    else if (new_x >= SNAKE_GRID_W) new_x = 0;
    if (new_y < 0) new_y = SNAKE_GRID_H - 1;
    else if (new_y >= SNAKE_GRID_H) new_y = 0;

    // Self collision check
    for (uint16_t i = 0; i < g_snake.len; i++) {
        uint16_t idx = (g_snake.head + SNAKE_MAX_LEN - i) % SNAKE_MAX_LEN;
        if (g_snake.x[idx] == (uint8_t)new_x && g_snake.y[idx] == (uint8_t)new_y) {
            g_snake.game_over = true;
            return;
        }
    }

    // Move head forward
    uint16_t new_head = (g_snake.head + 1) % SNAKE_MAX_LEN;
    g_snake.x[new_head] = (uint8_t)new_x;
    g_snake.y[new_head] = (uint8_t)new_y;
    g_snake.head = new_head;

    // Check food
    if ((uint8_t)new_x == g_snake.food_x && (uint8_t)new_y == g_snake.food_y) {
        // Grow: increase length (don't remove tail)
        if (g_snake.len < SNAKE_MAX_LEN - 1) {
            g_snake.len++;
        }
        g_snake.score += 10;
        snake_place_food();
        board_buzzer_play_sequence_async(&g_seq_eat);
    }
    // If no food eaten, tail stays same length (head moved forward, oldest segment dropped)
}

/**
 * @brief Render snake game
 */
static void snake_render(void)
{
    if (g_pixels_handle == NULL) return;

    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    // Draw score first (lowest priority, can be overwritten by snake/food)
    tetris_draw_number_ccw(0, 29, g_snake.score, 100, 100, 100);

    // Draw snake body (green, head brighter) - on top of score
    for (uint16_t i = 0; i < g_snake.len; i++) {
        uint16_t idx = (g_snake.head + SNAKE_MAX_LEN - i) % SNAKE_MAX_LEN;
        uint8_t r = 0, g = (i == 0) ? 255 : 150, b = 0; // Head is bright green
        tetris_set_pixel(g_snake.x[idx], g_snake.y[idx], r, g, b);
    }

    // Draw food (red, blinking) - on top of score
    static uint32_t blink = 0;
    blink++;
    if ((blink / 8) % 2 == 0) {
        tetris_set_pixel(g_snake.food_x, g_snake.food_y, 255, 0, 0);
    } else {
        tetris_set_pixel(g_snake.food_x, g_snake.food_y, 180, 0, 0);
    }

    // Game over flash
    if (g_snake.game_over) {
        if ((blink / 15) % 2 == 0) {
            // Flash border red
            for (int i = 0; i < 32; i++) {
                tetris_set_pixel(i, 0, 255, 0, 0);
                tetris_set_pixel(i, 31, 255, 0, 0);
                tetris_set_pixel(0, i, 255, 0, 0);
                tetris_set_pixel(31, i, 255, 0, 0);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Snake game effect - main function
 */
static void __snake_effect(void)
{
    if (g_pixels_handle == NULL) return;

    if (!g_snake.initialized) {
        memset(&g_snake, 0, sizeof(g_snake));
        // Start snake in center, length 3, going right
        g_snake.head = 2;
        g_snake.len = 3;
        g_snake.x[0] = 14; g_snake.y[0] = 16; // tail
        g_snake.x[1] = 15; g_snake.y[1] = 16;
        g_snake.x[2] = 16; g_snake.y[2] = 16; // head
        g_snake.dir = SNAKE_RIGHT;
        g_snake.move_timer = 0;
        snake_place_food();
        g_snake.initialized = true;
        PR_NOTICE("Snake game started! IMU control enabled (g_imu_ready=%d)", g_imu_ready);
    }

    // Game over - play melody and restart after 2 seconds
    if (g_snake.game_over) {
        static uint32_t gameover_timer = 0;
        if (gameover_timer == 0) {
            board_buzzer_play_sequence_async(&g_seq_game_over);
        }
        gameover_timer++;
        snake_render();
        if (gameover_timer > 50) {
            g_snake.initialized = false;
            gameover_timer = 0;
        }
        return;
    }

    snake_update();
    snake_render();
}

// ======================== Ninja Runner Game ========================

/**
 * @brief Set pixel using game coordinates with rotation° rotation
 * Game (gx, gy) -> Screen (gy, 31 - gx)
 */
static void ninja_set_pixel(int32_t gx, int32_t gy, uint8_t r, uint8_t g, uint8_t b)
{
    tetris_set_pixel(gy, 31 - gx, r, g, b);
}

static void ninja_draw_digit(int32_t gx, int32_t gy, uint8_t digit, uint8_t r, uint8_t g, uint8_t b)
{
    if (digit > 9) return;
    for (int row = 0; row < 5; row++) {
        uint8_t bits = g_tiny_digits[digit][row];
        for (int col = 0; col < 3; col++) {
            if (bits & (0x4 >> col)) {
                ninja_set_pixel(gx + col, gy + row, r, g, b);
            }
        }
    }
}

static void ninja_draw_number(int32_t gx, int32_t gy, uint32_t num, uint8_t r, uint8_t g, uint8_t b)
{
    char buf[6]; int len = 0;
    if (num == 0) { buf[0] = 0; len = 1; }
    else { uint32_t tmp = num; while (tmp > 0 && len < 5) { buf[len++] = tmp % 10; tmp /= 10; } }
    for (int i = 0; i < len; i++) {
        ninja_draw_digit(gx + (len - 1 - i) * 4, gy, buf[i], r, g, b);
    }
}

static void ninja_get_spawn_range(uint8_t diff, uint32_t *min_spawn, uint32_t *max_spawn)
{
    switch (diff) {
        case 1: *min_spawn = 40; *max_spawn = 60; break;
        case 2: *min_spawn = 28; *max_spawn = 45; break;
        case 3: *min_spawn = 18; *max_spawn = 32; break;
        case 4: *min_spawn = 10; *max_spawn = 22; break;
        case 5: *min_spawn = 5;  *max_spawn = 15; break;
        default: *min_spawn = 18; *max_spawn = 32; break;
    }
}

/**
 * @brief Get player center Y with jump offset applied
 */
static int8_t ninja_player_gy(void)
{
    int8_t base_gy = g_ninja.above_line ? NINJA_ABOVE_GY : NINJA_BELOW_GY;
    if (g_ninja.jumping && g_ninja.jump_timer > 0) {
        int half = NINJA_JUMP_FRAMES / 2;
        int t = NINJA_JUMP_FRAMES - g_ninja.jump_timer;
        int offset;
        if (t <= half) {
            offset = (NINJA_JUMP_HEIGHT * t) / half;
        } else {
            offset = (NINJA_JUMP_HEIGHT * (NINJA_JUMP_FRAMES - t)) / half;
        }
        // Jump AWAY from line: above→up (decrease gy), below→down (increase gy)
        if (g_ninja.above_line) {
            base_gy -= offset;
        } else {
            base_gy += offset;
        }
    }
    return base_gy;
}

/**
 * @brief Spawn a new obstacle at the right edge
 */
static void ninja_spawn_obstacle(void)
{
    for (int i = 0; i < NINJA_MAX_OBSTACLES; i++) {
        if (!g_ninja.obstacles[i].active) {
            g_ninja.obstacles[i].gx = 31;
            g_ninja.obstacles[i].type = rand() % 3; // 0=above, 1=below, 2=full
            g_ninja.obstacles[i].active = true;
            return;
        }
    }
}

/**
 * @brief Draw the ninja player character (3x5 pixel figure) in game coords
 */
static void ninja_draw_player(int8_t cx, int8_t cy)
{
    // Head
    ninja_set_pixel(cx, cy - 2, 255, 255, 255);
    // Body
    ninja_set_pixel(cx, cy - 1, 0, 200, 255);
    ninja_set_pixel(cx, cy, 0, 200, 255);
    // Arms
    ninja_set_pixel(cx - 1, cy - 1, 0, 200, 255);
    ninja_set_pixel(cx + 1, cy - 1, 0, 200, 255);
    // Legs (running animation)
    if ((g_ninja.frame_count / 4) % 2 == 0) {
        ninja_set_pixel(cx - 1, cy + 1, 0, 150, 200);
        ninja_set_pixel(cx + 1, cy + 2, 0, 150, 200);
    } else {
        ninja_set_pixel(cx + 1, cy + 1, 0, 150, 200);
        ninja_set_pixel(cx - 1, cy + 2, 0, 150, 200);
    }
}

/**
 * @brief Draw an obstacle in game coords
 */
static void ninja_draw_obstacle(ninja_obstacle_t *obs)
{
    if (!obs->active) return;
    int8_t gx = obs->gx;

    switch (obs->type) {
        case 0: // Above-line obstacle: 2w x 4h (gy = ABOVE_GY-1 .. LINE_GY-1)
            for (int dy = NINJA_ABOVE_GY - 1; dy <= NINJA_LINE_GY - 1; dy++) {
                ninja_set_pixel(gx, dy, 255, 60, 0);
                ninja_set_pixel(gx + 1, dy, 255, 60, 0);
            }
            break;
        case 1: // Below-line obstacle: 2w x 4h (gy = LINE_GY+1 .. BELOW_GY+1)
            for (int dy = NINJA_LINE_GY + 1; dy <= NINJA_BELOW_GY + 1; dy++) {
                ninja_set_pixel(gx, dy, 0, 100, 255);
                ninja_set_pixel(gx + 1, dy, 0, 100, 255);
            }
            break;
        case 2: // Full-height obstacle: 2w x 10h (both sides, must jump)
            for (int dy = NINJA_ABOVE_GY - 1; dy <= NINJA_BELOW_GY + 1; dy++) {
                ninja_set_pixel(gx, dy, 255, 0, 60);
                ninja_set_pixel(gx + 1, dy, 255, 0, 60);
            }
            break;
    }
}

/**
 * @brief Check collision between player and obstacles
 */
static bool ninja_check_collision(void)
{
    int8_t py = ninja_player_gy();
    int8_t p_left = NINJA_PLAYER_GX - 1;
    int8_t p_right = NINJA_PLAYER_GX + 1;
    int8_t p_top = py - 2;
    int8_t p_bottom = py + 2;

    for (int i = 0; i < NINJA_MAX_OBSTACLES; i++) {
        if (!g_ninja.obstacles[i].active) continue;

        int8_t ox = g_ninja.obstacles[i].gx;
        int8_t o_left = ox;
        int8_t o_right = ox + 1;
        int8_t o_top, o_bottom;

        switch (g_ninja.obstacles[i].type) {
            case 0: o_top = NINJA_ABOVE_GY - 1; o_bottom = NINJA_LINE_GY - 1; break;
            case 1: o_top = NINJA_LINE_GY + 1;  o_bottom = NINJA_BELOW_GY + 1; break;
            case 2: o_top = NINJA_ABOVE_GY - 1; o_bottom = NINJA_BELOW_GY + 1; break;
            default: continue;
        }

        if (p_left <= o_right && p_right >= o_left &&
            p_top <= o_bottom && p_bottom >= o_top) {
            return true;
        }
    }
    return false;
}

/**
 * @brief Ninja difficulty selection screen (centered, game coords)
 */
static void ninja_draw_difficulty_select(void)
{
    if (g_pixels_handle == NULL) return;

    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    // Ninja character preview (centered top)
    ninja_draw_player(16, 8);

    // Ground line preview (centered)
    for (int gx = 10; gx < 22; gx++) {
        ninja_set_pixel(gx, NINJA_LINE_GY, 40, 40, 40);
    }

    // Difficulty number centered
    ninja_draw_digit(15, 21, g_ninja.difficulty, 255, 255, 0);

    // Difficulty bar: 5 blocks showing level
    for (uint8_t i = 0; i < 5; i++) {
        uint8_t cr = (i < g_ninja.difficulty) ? 0 : 30;
        uint8_t cg = (i < g_ninja.difficulty) ? 200 : 30;
        uint8_t cb = (i < g_ninja.difficulty) ? 50 : 30;
        ninja_set_pixel(10 + i * 3, 28, cr, cg, cb);
        ninja_set_pixel(11 + i * 3, 28, cr, cg, cb);
        ninja_set_pixel(10 + i * 3, 29, cr, cg, cb);
        ninja_set_pixel(11 + i * 3, 29, cr, cg, cb);
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Render the ninja game
 */
static void ninja_render(void)
{
    if (g_pixels_handle == NULL) return;

    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    // Draw ground line (game coords: horizontal line at gy=LINE_GY)
    for (int gx = 0; gx < 32; gx++) {
        ninja_set_pixel(gx, NINJA_LINE_GY, 40, 40, 40);
    }

    // Draw obstacles
    for (int i = 0; i < NINJA_MAX_OBSTACLES; i++) {
        ninja_draw_obstacle(&g_ninja.obstacles[i]);
    }

    // Draw player
    int8_t py = ninja_player_gy();
    ninja_draw_player(NINJA_PLAYER_GX, py);

    // Score (top-left in game coords)
    ninja_draw_number(1, 1, g_ninja.score, 150, 150, 150);

    // Game over flash
    if (g_ninja.phase == 2) {
        static uint32_t blink = 0;
        blink++;
        if ((blink / 15) % 2 == 0) {
            for (int i = 0; i < 32; i++) {
                ninja_set_pixel(i, 0, 255, 0, 0);
                ninja_set_pixel(i, 31, 255, 0, 0);
                ninja_set_pixel(0, i, 255, 0, 0);
                ninja_set_pixel(31, i, 255, 0, 0);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Ninja runner game effect - main function called each frame
 */
static void __ninja_effect(void)
{
    if (g_pixels_handle == NULL) return;

    if (!g_ninja.initialized) {
        memset(&g_ninja, 0, sizeof(g_ninja));
        g_ninja.above_line = true;
        g_ninja.difficulty = 3;
        g_ninja.next_spawn = 30;
        g_ninja.phase = 0;
        g_ninja.initialized = true;
        g_ninja_input = 0;
        PR_NOTICE("Ninja runner: difficulty select");
    }

    g_ninja.frame_count++;

    // Phase 0: Difficulty selection
    if (g_ninja.phase == 0) {
        uint8_t inp = g_ninja_input;
        g_ninja_input = 0;

        if (inp == 1) {
            if (g_ninja.difficulty > 1) g_ninja.difficulty--;
        } else if (inp == 2) {
            if (g_ninja.difficulty < 5) g_ninja.difficulty++;
        } else if (inp == 4) {
            g_ninja.phase = 1;
            g_ninja.score = 0;
            g_ninja.frame_count = 0;
            g_ninja.spawn_timer = 0;
            g_ninja.move_timer = 0;
            g_ninja.above_line = true;
            g_ninja.jumping = false;
            g_ninja.jump_timer = 0;
            uint32_t smin, smax;
            ninja_get_spawn_range(g_ninja.difficulty, &smin, &smax);
            g_ninja.next_spawn = smin + (rand() % (smax - smin + 1));
            for (int i = 0; i < NINJA_MAX_OBSTACLES; i++) {
                g_ninja.obstacles[i].active = false;
            }
            PR_NOTICE("Ninja started! diff=%d", g_ninja.difficulty);
            return;
        }
        ninja_draw_difficulty_select();
        return;
    }

    // Phase 2: Game over - play melody and restart after 2 seconds
    if (g_ninja.phase == 2) {
        static uint32_t gameover_timer = 0;
        if (gameover_timer == 0) {
            board_buzzer_play_sequence_async(&g_seq_game_over);
        }
        gameover_timer++;
        ninja_render();
        if (gameover_timer > 50) {
            uint8_t saved_diff = g_ninja.difficulty;
            memset(&g_ninja, 0, sizeof(g_ninja));
            g_ninja.difficulty = saved_diff;
            g_ninja.above_line = true;
            g_ninja.phase = 1;
            g_ninja.initialized = true;
            g_ninja_input = 0;
            uint32_t smin, smax;
            ninja_get_spawn_range(g_ninja.difficulty, &smin, &smax);
            g_ninja.next_spawn = smin + (rand() % (smax - smin + 1));
            gameover_timer = 0;
        }
        return;
    }

    // Phase 1: Playing
    uint8_t inp = g_ninja_input;
    g_ninja_input = 0;

    if (inp == 1) { // A = below line
        if (!g_ninja.jumping) g_ninja.above_line = false;
    } else if (inp == 2) { // B = above line
        if (!g_ninja.jumping) g_ninja.above_line = true;
    } else if (inp == 3) { // OK = jump
        if (!g_ninja.jumping) {
            g_ninja.jumping = true;
            g_ninja.jump_timer = NINJA_JUMP_FRAMES;
        }
    }

    // Update jump
    if (g_ninja.jumping) {
        g_ninja.jump_timer--;
        if (g_ninja.jump_timer <= 0) {
            g_ninja.jumping = false;
            g_ninja.jump_timer = 0;
        }
    }

    // Move obstacles
    g_ninja.move_timer++;
    if (g_ninja.move_timer >= NINJA_MOVE_SPEED) {
        g_ninja.move_timer = 0;

        for (int i = 0; i < NINJA_MAX_OBSTACLES; i++) {
            if (g_ninja.obstacles[i].active) {
                g_ninja.obstacles[i].gx--;
                if (g_ninja.obstacles[i].gx < -2) {
                    g_ninja.obstacles[i].active = false;
                    g_ninja.score += 10;
                }
            }
        }
    }

    // Spawn obstacles (difficulty-based frequency)
    g_ninja.spawn_timer++;
    if (g_ninja.spawn_timer >= g_ninja.next_spawn) {
        g_ninja.spawn_timer = 0;
        uint32_t smin, smax;
        ninja_get_spawn_range(g_ninja.difficulty, &smin, &smax);
        g_ninja.next_spawn = smin + (rand() % (smax - smin + 1));
        ninja_spawn_obstacle();
    }

    // Check collision
    if (ninja_check_collision()) {
        g_ninja.phase = 2;
        PR_NOTICE("Ninja game over! Score=%d", g_ninja.score);
    }

    ninja_render();
}

/***********************************************************
 * Audio Spectrum Analyzer
 ***********************************************************/

/**
 * @brief Reset spectrum bars and ring buffer to idle (empty display)
 * @return none
 */
static void spectrum_reset_state(void)
{
    memset(g_spectrum_band_mag, 0, sizeof(g_spectrum_band_mag));
    memset(g_spectrum_band_peak, 0, sizeof(g_spectrum_band_peak));
    memset(g_spectrum_audio_buf, 0, sizeof(g_spectrum_audio_buf));
    if (g_audio_ringbuf != NULL && g_audio_rb_mutex != NULL) {
        tal_mutex_lock(g_audio_rb_mutex);
        tuya_ring_buff_reset(g_audio_ringbuf);
        tal_mutex_unlock(g_audio_rb_mutex);
    }
}

/**
 * @brief Push PCM samples into spectrum ring buffer
 * @param[in] data PCM frame bytes
 * @param[in] len byte length
 * @return none
 */
static void spectrum_feed_pcm(const uint8_t *data, uint32_t len)
{
    if (data == NULL || len == 0) {
        return;
    }
    if (g_audio_ringbuf == NULL || g_audio_rb_mutex == NULL) {
        return;
    }

    tal_mutex_lock(g_audio_rb_mutex);
    uint32_t used = tuya_ring_buff_used_size_get(g_audio_ringbuf);
    if (used > SPECTRUM_RINGBUF_SIZE / 2) {
        tuya_ring_buff_discard(g_audio_ringbuf, used - SPECTRUM_RINGBUF_SIZE / 4);
    }
    tuya_ring_buff_write(g_audio_ringbuf, data, len);
    tal_mutex_unlock(g_audio_rb_mutex);
}

/**
 * @brief Whether spectrum should use TTS (playback) gain path
 * @return true when AI chat mode and cloud TTS is playing
 */
static bool spectrum_is_tts_visual(void)
{
    return (g_animation_mode == AI_CHAT_SPECTRUM_MODE) &&
           (ai_audio_player_is_playing() != 0);
}

/**
 * @brief Feed attenuated TTS PCM (decoded speaker path runs much hotter than mic)
 * @param[in] data PCM frame bytes
 * @param[in] len byte length
 * @return none
 */
static void spectrum_feed_pcm_tts(const uint8_t *data, uint32_t len)
{
    int16_t scratch[160];
    uint32_t off = 0;

    if (data == NULL || len < 2) {
        return;
    }

    while (off < len) {
        uint32_t chunk_bytes = len - off;

        if (chunk_bytes > sizeof(scratch)) {
            chunk_bytes = sizeof(scratch);
        }
        chunk_bytes &= ~1U;
        if (chunk_bytes < 2) {
            break;
        }

        const int16_t *in = (const int16_t *)(data + off);
        uint32_t chunk_samples = chunk_bytes / 2;
        for (uint32_t i = 0; i < chunk_samples; i++) {
            scratch[i] = (int16_t)(in[i] >> SPECTRUM_TTS_PCM_SHIFT);
        }
        spectrum_feed_pcm((const uint8_t *)scratch, chunk_samples * 2);
        off += chunk_bytes;
    }
}

static void spectrum_audio_callback(TDL_AUDIO_FRAME_FORMAT_E type, TDL_AUDIO_STATUS_E status, uint8_t *data, uint32_t len)
{
    (void)status;
    if (type != TDL_AUDIO_FRAME_FORMAT_PCM) {
        return;
    }
    spectrum_feed_pcm(data, len);
}

/**
 * @brief Create spectrum ring buffer (shared by standalone and AI chat modes)
 * @return OPRT_OK on success
 */
static OPERATE_RET spectrum_ringbuf_init(void)
{
    OPERATE_RET rt;

    if (g_spectrum_ringbuf_ready) {
        return OPRT_OK;
    }

    rt = tuya_ring_buff_create(SPECTRUM_RINGBUF_SIZE, OVERFLOW_PSRAM_STOP_TYPE, &g_audio_ringbuf);
    if (rt != OPRT_OK) {
        PR_ERR("Spectrum: ring buffer create failed: %d", rt);
        return rt;
    }

    rt = tal_mutex_create_init(&g_audio_rb_mutex);
    if (rt != OPRT_OK) {
        PR_ERR("Spectrum: mutex create failed: %d", rt);
        return rt;
    }

    memset(g_spectrum_band_mag, 0, sizeof(g_spectrum_band_mag));
    memset(g_spectrum_band_peak, 0, sizeof(g_spectrum_band_peak));
    memset(g_spectrum_audio_buf, 0, sizeof(g_spectrum_audio_buf));

    g_spectrum_ringbuf_ready = true;
    return OPRT_OK;
}

/**
 * @brief Tap decoded TTS PCM from speaker consumer for stronger spectrum
 * @param[in] pcm PCM samples
 * @param[in] len byte length
 * @return none
 */
void pixel_spectrum_pcm_tap(const uint8_t *pcm, uint32_t len)
{
    if (g_animation_mode != AI_CHAT_SPECTRUM_MODE) {
        return;
    }
    if (pcm == NULL || len == 0) {
        return;
    }
    if (ai_audio_player_is_playing() == 0) {
        return;
    }
    if (spectrum_ringbuf_init() != OPRT_OK) {
        return;
    }
    spectrum_feed_pcm_tts(pcm, len);
}

static void spectrum_init(void)
{
    if (g_spectrum_initialized) {
        return;
    }

    if (spectrum_ringbuf_init() != OPRT_OK) {
        return;
    }

    tal_system_sleep(200);

    OPERATE_RET rt = tdl_audio_find(AUDIO_CODEC_NAME, &g_audio_handle);
    if (rt != OPRT_OK) {
        PR_ERR("Spectrum: audio device not found: %d", rt);
        return;
    }

    rt = tdl_audio_open(g_audio_handle, spectrum_audio_callback);
    if (rt != OPRT_OK) {
        PR_ERR("Spectrum: audio open failed: %d", rt);
        return;
    }

    g_spectrum_initialized = true;
    PR_NOTICE("Spectrum analyzer initialized (16 bands, 2px wide)");
}

static void spectrum_compute_fft(void)
{
    static float window[SPECTRUM_FFT_SIZE];
    static bool window_computed = false;
    if (!window_computed) {
        for (int n = 0; n < SPECTRUM_FFT_SIZE; n++) {
            window[n] = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * (float)n / (float)(SPECTRUM_FFT_SIZE - 1)));
        }
        window_computed = true;
    }

    int max_bin = SPECTRUM_FFT_SIZE / 2;
    for (int k = 0; k < max_bin; k++) {
        float real_sum = 0.0f, imag_sum = 0.0f;
        float k_angle = -2.0f * (float)M_PI * (float)k / (float)SPECTRUM_FFT_SIZE;
        for (int n = 0; n < SPECTRUM_FFT_SIZE; n++) {
            float w = (float)g_spectrum_audio_buf[n] * window[n];
            float angle = k_angle * (float)n;
            real_sum += w * cosf(angle);
            imag_sum += w * sinf(angle);
        }
        g_spectrum_fft_real[k] = real_sum;
        g_spectrum_fft_imag[k] = imag_sum;
    }
}

static void spectrum_calc_bands(void)
{
    float freq_res = (float)SPECTRUM_SAMPLE_RATE / (float)SPECTRUM_FFT_SIZE;

    for (int band = 0; band < SPECTRUM_NUM_BANDS; band++) {
        int bin_start = (int)(g_spectrum_band_start[band] / freq_res);
        int bin_end = (int)(g_spectrum_band_end[band] / freq_res);
        if (bin_start < 0) bin_start = 0;
        if (bin_end >= SPECTRUM_FFT_SIZE / 2) bin_end = SPECTRUM_FFT_SIZE / 2 - 1;
        if (bin_end < bin_start) bin_end = bin_start;

        float mag_sum = 0.0f;
        int count = 0;
        for (int bin = bin_start; bin <= bin_end; bin++) {
            float mag = sqrtf(g_spectrum_fft_real[bin] * g_spectrum_fft_real[bin] +
                              g_spectrum_fft_imag[bin] * g_spectrum_fft_imag[bin]);
            mag_sum += mag;
            count++;
        }

        float avg = (count > 0) ? (mag_sum / (float)count) : 0.0f;
        bool tts = spectrum_is_tts_visual();
        float scale = tts ? SPECTRUM_MAG_SCALE_TTS : SPECTRUM_MAG_SCALE_MIC;
        float norm = avg / scale;
        if (norm > 1.0f) {
            norm = 1.0f;
        }
        if (norm < 0.0f) {
            norm = 0.0f;
        }
        norm = log10f(1.0f + norm * 9.0f);
        if (tts) {
            norm *= SPECTRUM_TTS_NORM_GAIN;
            if (norm > 1.0f) {
                norm = 1.0f;
            }
        }
        g_spectrum_band_mag[band] = norm;
    }
}

static void spectrum_render(bool live)
{
    if (g_pixels_handle == NULL) return;

    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    if (live) {
        for (int i = 0; i < SPECTRUM_NUM_BANDS; i++) {
            g_spectrum_band_peak[i] -= 0.6f;
            if (g_spectrum_band_peak[i] < 0.0f) {
                g_spectrum_band_peak[i] = 0.0f;
            }
        }
    }

    for (int band = 0; band < SPECTRUM_NUM_BANDS; band++) {
        int bar_h = (int)(g_spectrum_band_mag[band] * 32);
        if (spectrum_is_tts_visual() && bar_h > SPECTRUM_TTS_MAX_BAR) {
            bar_h = SPECTRUM_TTS_MAX_BAR;
        }
        if (bar_h > 32) bar_h = 32;
        if (bar_h < 0) bar_h = 0;

        if (bar_h > (int)g_spectrum_band_peak[band]) {
            g_spectrum_band_peak[band] = (float)bar_h;
        }

        int col_start = band * SPECTRUM_BAND_WIDTH;
        float base_hue = (float)band / (float)SPECTRUM_NUM_BANDS * 360.0f;

        for (int col = col_start; col < col_start + SPECTRUM_BAND_WIDTH && col < 32; col++) {
            for (int row = 31; row >= 32 - bar_h && row >= 0; row--) {
                int row_in_bar = 31 - row;
                float gradient = (bar_h > 1) ? ((float)row_in_bar / (float)(bar_h - 1)) : 0.0f;
                float hue = base_hue + (-10.0f + gradient * 20.0f);
                uint32_t r, g, b;
                board_pixel_hsv_to_rgb(hue, 1.0f, 1.0f, &r, &g, &b);
                /* CCW 90°: (col, row) -> (row, 31 - col) */
                tetris_set_pixel(row, 31 - col, (uint8_t)r, (uint8_t)g, (uint8_t)b);
            }

            int peak_y = 31 - (int)g_spectrum_band_peak[band];
            if (peak_y >= 0 && peak_y < 32) {
                tetris_set_pixel(peak_y, 31 - col, 255, 255, 255);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Process ring buffer and update FFT when live
 * @param[in] live true to consume audio and update bars
 * @return none
 */
static void spectrum_update_from_ringbuf(bool live)
{
    if (!live) {
        spectrum_render(false);
        return;
    }

    uint8_t frame_buf[SPECTRUM_FRAME_BYTES];
    uint32_t available = 0;

    if (g_audio_ringbuf != NULL && g_audio_rb_mutex != NULL) {
        tal_mutex_lock(g_audio_rb_mutex);
        available = tuya_ring_buff_used_size_get(g_audio_ringbuf);
        tal_mutex_unlock(g_audio_rb_mutex);
    }

    if (available >= SPECTRUM_FRAME_BYTES) {
        uint32_t read_len = 0;
        tal_mutex_lock(g_audio_rb_mutex);
        read_len = tuya_ring_buff_read(g_audio_ringbuf, frame_buf, SPECTRUM_FRAME_BYTES);
        tal_mutex_unlock(g_audio_rb_mutex);

        if (read_len == SPECTRUM_FRAME_BYTES) {
            int16_t *pcm = (int16_t *)frame_buf;
            uint32_t num_samples = read_len / 2;
            if (num_samples >= SPECTRUM_FFT_SIZE) {
                memcpy(g_spectrum_audio_buf, pcm, SPECTRUM_FFT_SIZE * sizeof(int16_t));
            } else {
                memmove(g_spectrum_audio_buf, &g_spectrum_audio_buf[num_samples],
                        (SPECTRUM_FFT_SIZE - num_samples) * sizeof(int16_t));
                memcpy(&g_spectrum_audio_buf[SPECTRUM_FFT_SIZE - num_samples],
                       pcm, num_samples * sizeof(int16_t));
            }
            spectrum_compute_fft();
            spectrum_calc_bands();
        }
    }

    spectrum_render(true);
}

static void __spectrum_effect(void)
{
    if (!g_spectrum_initialized) {
        spectrum_init();
        if (!g_spectrum_initialized) {
            tal_system_sleep(100);
            return;
        }
    }

    spectrum_update_from_ringbuf(true);
}

/**
 * @brief AI chat mode: spectrum reacts only while OK held or TTS playing
 * @return none
 */
static void __ai_chat_spectrum_effect(void)
{
    static bool prev_live = false;
    bool live;

    if (spectrum_ringbuf_init() != OPRT_OK) {
        tal_system_sleep(100);
        return;
    }

    live = g_spectrum_ok_holding || (ai_audio_player_is_playing() != 0);
    if (!live && prev_live) {
        spectrum_reset_state();
    }
    prev_live = live;

    if (live) {
        spectrum_update_from_ringbuf(true);
    } else {
        spectrum_render(false);
    }
}

/***********************************************************
 * Sharingan Animation Effect (1 tomoe → 3 tomoe → Mangekyou)
 ***********************************************************/

// Draw a tomoe (comma mark) at polar position on the iris
// orbit_r: distance from center, angle: position angle, tail_dir: direction tail curves
static void sharingan_draw_tomoe(float cx, float cy, float orbit_r, float angle, float scale)
{
    // Tomoe head position
    float hx = cx + orbit_r * cosf(angle);
    float hy = cy + orbit_r * sinf(angle);

    // Draw filled circle for head (radius proportional to scale)
    float head_r = 2.0f * scale;
    for (int x = 0; x < 32; x++) {
        for (int y = 0; y < 32; y++) {
            float dx = (float)x - hx;
            float dy = (float)y - hy;
            if (sqrtf(dx * dx + dy * dy) <= head_r) {
                tetris_set_pixel(x, y, 0, 0, 0);
            }
        }
    }

    // Draw curved tail (arc from head toward center, curving clockwise)
    int steps = (int)(12.0f * scale);
    for (int s = 0; s < steps; s++) {
        float t = (float)s / (float)steps;
        // Tail sweeps along the orbit circle (clockwise)
        float tail_angle = angle + t * 0.8f;
        // Tail radius decreases from orbit_r toward center
        float tail_r = orbit_r - t * 2.5f * scale;
        if (tail_r < 0) tail_r = 0;
        float tx = cx + tail_r * cosf(tail_angle);
        float ty = cy + tail_r * sinf(tail_angle);
        // Tail gets thinner
        float tw = (1.5f - t * 1.0f) * scale;
        for (int x = (int)(tx - tw); x <= (int)(tx + tw); x++) {
            for (int y = (int)(ty - tw); y <= (int)(ty + tw); y++) {
                if (x >= 0 && x < 32 && y >= 0 && y < 32) {
                    float ddx = (float)x - tx;
                    float ddy = (float)y - ty;
                    if (sqrtf(ddx * ddx + ddy * ddy) <= tw) {
                        tetris_set_pixel(x, y, 0, 0, 0);
                    }
                }
            }
        }
    }
}

static void __sharingan_effect(void)
{
    if (g_pixels_handle == NULL) return;

    static uint32_t frame = 0;
    frame++;

    // Phase timing (at 50fps)
    // Phase 0: 1 tomoe (0-199, 4s)
    // Phase 1: 3 tomoe (200-449, 5s)
    // Phase 2: transition flash (450-469, 0.4s)
    // Phase 3: mangekyou (470-719, 5s)
    // Phase 4: transition flash (720-739, 0.4s)
    // Total cycle: 740 frames (~14.8s)
    uint32_t cycle_frame = frame % 740;
    int phase;
    uint32_t phase_frame;
    if (cycle_frame < 200) {
        phase = 0; phase_frame = cycle_frame;
    } else if (cycle_frame < 450) {
        phase = 1; phase_frame = cycle_frame - 200;
    } else if (cycle_frame < 470) {
        phase = 2; phase_frame = cycle_frame - 450;
    } else if (cycle_frame < 720) {
        phase = 3; phase_frame = cycle_frame - 470;
    } else {
        phase = 4; phase_frame = cycle_frame - 720;
    }

    float cx = 15.5f, cy = 15.5f;
    float outer_r = 14.5f;
    float inner_r = 13.0f;
    float pupil_r = 3.5f;

    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    // Transition flash phases
    if (phase == 2 || phase == 4) {
        // White/red flash that fades
        float brightness = 1.0f - (float)phase_frame / 20.0f;
        if (brightness < 0) brightness = 0;
        uint8_t rv = (uint8_t)(255 * brightness);
        uint8_t gv = (uint8_t)(80 * brightness);
        for (int x = 0; x < 32; x++) {
            for (int y = 0; y < 32; y++) {
                float dx = (float)x - cx;
                float dy = (float)y - cy;
                if (sqrtf(dx * dx + dy * dy) <= outer_r) {
                    tetris_set_pixel(x, y, rv, gv, 0);
                }
            }
        }
        tdl_pixel_dev_refresh(g_pixels_handle);
        return;
    }

    // Rotation speed: ~72 deg/sec
    float rot_rad = (float)frame * 0.025f;

    // Draw iris (red circle)
    for (int x = 0; x < 32; x++) {
        for (int y = 0; y < 32; y++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float r = sqrtf(dx * dx + dy * dy);

            if (r > outer_r) continue;

            // Outer ring border
            if (r > inner_r) {
                tetris_set_pixel(x, y, 80, 0, 0);
                continue;
            }

            // Pupil
            if (r < pupil_r) {
                tetris_set_pixel(x, y, 0, 0, 0);
                continue;
            }

            if (phase == 3) {
                // Mangekyou phase: three curved blades
                float theta = atan2f(dy, dx) - rot_rad;
                while (theta < 0) theta += 2.0f * (float)M_PI;
                while (theta >= 2.0f * (float)M_PI) theta -= 2.0f * (float)M_PI;

                float blade_norm = (r - pupil_r) / (inner_r - pupil_r);
                bool on_blade = false;
                for (int k = 0; k < 3; k++) {
                    float blade_base = (float)k * 2.0f * (float)M_PI / 3.0f;
                    float blade_center = blade_base + blade_norm * 0.7f;
                    while (blade_center >= 2.0f * (float)M_PI) blade_center -= 2.0f * (float)M_PI;

                    float adiff = theta - blade_center;
                    while (adiff > (float)M_PI) adiff -= 2.0f * (float)M_PI;
                    while (adiff < -(float)M_PI) adiff += 2.0f * (float)M_PI;
                    if (adiff < 0) adiff = -adiff;

                    float half_w = 0.12f + 0.15f * blade_norm;
                    if (adiff < half_w) { on_blade = true; break; }
                }

                if (on_blade) {
                    tetris_set_pixel(x, y, 0, 0, 0);
                } else {
                    uint8_t rv = (uint8_t)(200 - (uint8_t)(blade_norm * 60.0f));
                    tetris_set_pixel(x, y, rv, 0, 0);
                }
            } else {
                // Tomoe phases (0 or 1): just red iris (tomoe drawn separately on top)
                float norm_r = (r - pupil_r) / (inner_r - pupil_r);
                uint8_t rv = (uint8_t)(200 - (uint8_t)(norm_r * 50.0f));
                tetris_set_pixel(x, y, rv, 0, 0);
            }
        }
    }

    // Draw tomoe for phases 0 and 1
    if (phase == 0 || phase == 1) {
        int num_tomoe = (phase == 0) ? 1 : 3;
        float orbit = 8.0f;
        for (int t = 0; t < num_tomoe; t++) {
            float angle = rot_rad + (float)t * 2.0f * (float)M_PI / (float)num_tomoe;
            sharingan_draw_tomoe(cx, cy, orbit, angle, 1.0f);
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Set one pixel in agent mode with reduced brightness (GRB order)
 * @param[in] sx matrix x
 * @param[in] sy matrix y
 * @param[in] r red 0-255
 * @param[in] g green 0-255
 * @param[in] b blue 0-255
 * @return none
 */
static void __agent_set_pixel(int32_t sx, int32_t sy, uint8_t r, uint8_t g, uint8_t b)
{
    uint32_t led_idx;

    if (g_pixels_handle == NULL || sx < 0 || sx >= 32 || sy < 0 || sy >= 32) {
        return;
    }
    led_idx = board_pixel_matrix_coord_to_led_index((uint32_t)sx, (uint32_t)sy);
    if (led_idx >= LED_PIXELS_TOTAL_NUM) {
        return;
    }
    PIXEL_COLOR_T color = {0};
    color.red   = (uint32_t)((g * COLOR_RESOLUTION * AGENT_BRIGHTNESS) / 255);
    color.green = (uint32_t)((r * COLOR_RESOLUTION * AGENT_BRIGHTNESS) / 255);
    color.blue  = (uint32_t)((b * COLOR_RESOLUTION * AGENT_BRIGHTNESS) / 255);
    tdl_pixel_set_single_color(g_pixels_handle, led_idx, 1, &color);
}

/**
 * @brief Agent mode idle — small center teal pulse (low power, never stops)
 * @return none
 */
static void __agent_standby_effect(void)
{
    static float s_breath = 0.0f;
    float cx = 15.5f;
    float cy = 15.5f;
    float radius;
    int32_t x;
    int32_t y;

    if (g_pixels_handle == NULL) {
        return;
    }

    s_breath += 0.08f;
    radius = 5.0f + 2.5f * sinf(s_breath);

    PIXEL_COLOR_T off = {0};
    tdl_pixel_set_single_color(g_pixels_handle, 0, LED_PIXELS_TOTAL_NUM, &off);

    for (y = 0; y < 32; y++) {
        for (x = 0; x < 32; x++) {
            float dx = (float)x - cx;
            float dy = (float)y - cy;
            float dist = sqrtf(dx * dx + dy * dy);
            uint32_t led_idx;
            PIXEL_COLOR_T color;

            if (dist > radius) {
                continue;
            }
            color = board_pixel_hsv_to_pixel_color(170.0f, 0.85f, 1.0f - (dist / radius) * 0.4f,
                                                   AGENT_BRIGHTNESS, COLOR_RESOLUTION);
            led_idx = board_pixel_matrix_coord_to_led_index((uint32_t)x, (uint32_t)y);
            if (led_idx < LED_PIXELS_TOTAL_NUM) {
                tdl_pixel_set_single_color(g_pixels_handle, led_idx, 1, &color);
            }
        }
    }

    tdl_pixel_dev_refresh(g_pixels_handle);
}

/**
 * @brief Draw permission tool name on bottom row (3x5 font)
 * @return none
 */
static void __agent_monitor_draw_perm_label(void)
{
    CONST CHAR_T *tool = pixel_agent_bridge_perm_tool_name();
    CHAR_T label[12];
    INT_T len;
    INT_T start_x;
    INT_T i;
    INT_T cx;

    if (!pixel_agent_bridge_perm_pending() || tool == NULL || tool[0] == '\0') {
        return;
    }

    snprintf(label, sizeof(label), "%.6s", tool);
    len = (INT_T)strlen(label);
    if (len <= 0) {
        return;
    }

    start_x = (32 - len * 6) / 2;
    if (start_x < 0) {
        start_x = 0;
    }

    for (i = 0; i < len; i++) {
        CHAR_T ch = label[i];
        if (ch >= 'a' && ch <= 'z') {
            ch = (CHAR_T)(ch - 'a' + 'A');
        }
        cx = start_x + i * 6;
        for (int row = 0; row < 8; row++) {
            int py = 24 + row;
            if (py < 0 || py >= 32) {
                continue;
            }
            const LED_FONT_CHAR_T *font_char = get_font_char(ch);
            uint8_t row_data = font_char->data[row];
            for (int col = 0; col < 8 && col < (int)font_char->width; col++) {
                int px = cx + col;
                if (px < 0 || px >= 32) {
                    continue;
                }
                if (row_data & (0x80 >> col)) {
                    __agent_set_pixel(px, py, 255, 180, 80);
                }
            }
        }
    }
}

/**
 * @brief Work-queue job: BLE link ready chime (high double tone)
 * @param[in] arg unused
 * @return none
 */
STATIC VOID_T __ble_link_ready_job(VOID_T *arg)
{
    (void)arg;
    board_buzzer_play_note_duration(NOTE_A5, 45);
    tal_system_sleep(30);
    board_buzzer_play_note_duration(NOTE_C6, 70);
}

/**
 * @brief Work-queue job: MQTT cloud online chime (ascending C-E-G)
 * @param[in] arg unused
 * @return none
 */
STATIC VOID_T __mqtt_online_chime_job(VOID_T *arg)
{
    (void)arg;
    board_buzzer_play_note_duration(NOTE_C5, 90);
    tal_system_sleep(60);
    board_buzzer_play_note_duration(NOTE_E5, 90);
    tal_system_sleep(60);
    board_buzzer_play_note_duration(NOTE_G5, 130);
}

/**
 * @brief PC BLE bridge subscribed — connection chime only (no auto agent mode)
 * @return none
 */
VOID_T pixel_agent_ble_on_link_ready(VOID_T)
{
    static UINT32_T s_last_feedback_ms = 0;
    UINT32_T now = (UINT32_T)tal_system_get_tick_count();

    if ((now - s_last_feedback_ms) < 800) {
        return;
    }
    if (!pixel_agent_ble_take_link_chime()) {
        return;
    }
    s_last_feedback_ms = now;
    PR_NOTICE("agent ble: link ready");
    if (tal_workq_schedule(WORKQ_SYSTEM, __ble_link_ready_job, NULL) != OPRT_OK) {
        __ble_link_ready_job(NULL);
    }
}

/**
 * @brief Enter agent monitor mode (permission UI / manual entry only)
 * @return none
 */
VOID_T pixel_agent_bridge_request_monitor_mode(VOID_T)
{
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
    pixel_agent_clawd_set_ble_theme(pixel_agent_ble_should_use_blue_theme());
#endif
    if (g_animation_mode == AGENT_MONITOR_MODE) {
        return;
    }
    g_animation_mode = AGENT_MONITOR_MODE;
    g_animation_loop = true;
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
    if (!pixel_agent_ble_link_up()) {
        tuya_ble_adv_update();
    }
#endif
    if (!s_agent_ai_chat_ready) {
        if (__ai_chat_init_deferred() == OPRT_OK) {
            PR_NOTICE("agent: monitor mode (permission)");
        }
    } else {
        __ai_chat_apply_mode_volume();
        PR_NOTICE("agent: monitor mode (permission)");
    }
}

/**
 * @brief Agent monitor mode — driven by USB serial from pixel-agent-bridge
 * @return none
 */
static void __agent_monitor_effect(void)
{
    PIXEL_AGENT_VIS_E vis;
    static UINT32_T s_perm_blink = 0;

    if (g_pixels_handle == NULL) {
        return;
    }

    pixel_agent_bridge_poll();
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
    pixel_agent_ble_sync_display_theme();
#endif
    vis = pixel_agent_bridge_get_vis_state();

    if (pixel_agent_clawd_ready()) {
        pixel_agent_clawd_render(vis);
    } else {
        __agent_standby_effect();
    }

    if (pixel_agent_bridge_perm_pending()) {
        s_perm_blink++;
        if ((s_perm_blink % 10) < 5) {
            for (INT_T x = 0; x < 32; x++) {
                __agent_set_pixel(x, 0, 255, 100, 0);
                __agent_set_pixel(x, 31, 255, 100, 0);
            }
            for (INT_T y = 1; y < 31; y++) {
                __agent_set_pixel(0, y, 255, 100, 0);
                __agent_set_pixel(31, y, 255, 100, 0);
            }
        }
        __agent_monitor_draw_perm_label();
        tdl_pixel_dev_refresh(g_pixels_handle);
    }

    /* Voice recording indicator: blue pulsing corners when OK held */
    if (pixel_agent_bridge_is_recording()) {
        static UINT32_T s_record_blink = 0;
        s_record_blink++;
        if ((s_record_blink % 6) < 3) {
            __agent_set_pixel(0, 0, 0, 100, 255);
            __agent_set_pixel(31, 0, 0, 100, 255);
            __agent_set_pixel(0, 31, 0, 100, 255);
            __agent_set_pixel(31, 31, 0, 100, 255);
        }
        tdl_pixel_dev_refresh(g_pixels_handle);
    }
}

/**
 * @brief Sand physics effect - main function
 */
static void __sand_physics_effect(void)
{
    if (g_pixels_handle == NULL) {
        return;
    }

    // Initialize: spawn a 15x15 centered square of particles
    if (!g_sand_initialized) {
        // Clear grid
        for (int x = 0; x < MATRIX_WIDTH; x++)
            for (int y = 0; y < MATRIX_HEIGHT; y++)
                g_sand_grid[x][y] = -1;

        for (uint32_t i = 0; i < SAND_MAX_PARTICLES; i++)
            g_sand_particles[i].active = false;

        // 15x15 square centered in 32x32: (9,9) to (23,23)
        uint32_t idx = 0;
        for (int y = 9; y <= 23 && idx < SAND_MAX_PARTICLES; y++) {
            for (int x = 9; x <= 23 && idx < SAND_MAX_PARTICLES; x++) {
                g_sand_particles[idx].x = (int8_t)x;
                g_sand_particles[idx].y = (int8_t)y;
                uint8_t base_b = 160 + (rand() % 50);
                g_sand_particles[idx].r = 10 + (rand() % 20);
                g_sand_particles[idx].g = 80 + (rand() % 60);
                g_sand_particles[idx].b = base_b;
                g_sand_particles[idx].active = true;
                g_sand_grid[x][y] = (int16_t)idx;
                idx++;
            }
        }

        g_sand_initialized = true;
        PR_NOTICE("Sand physics: spawned %d particles (15x15 square, grid-based)", idx);
    }

    sand_update_physics();
    sand_render();
}

/**
 * @brief Pixel LED animation task thread
 */
static void pixel_led_animation_task(void *args)
{
    g_animation_running = true;
    PR_NOTICE("Pixel LED animation task started");

    while (g_animation_running) {
        switch (g_animation_mode) {
        case 0:
            __scrolling_text_effect();
            break;
        case 1:
            __breathing_color_effect();
            break;
        case 2:
            __ripple_effect();
            break;
        case 3:
            __2d_wave_effect();
            break;
        case 4:
            __snowflake_effect();
            break;
        case 5:
            __scan_animation_effect();
            break;
        case 6:
            __breathing_circle_effect();
            break;
        case 7:
            __running_light_effect();
            break;
        case 8:
            __color_wave_effect();
            break;
        case 9:
            __sharingan_effect();
            break;
        case SAND_PHYSICS_MODE:
            __sand_physics_effect();
            break;
        case TETRIS_MODE:
            __tetris_effect();
            break;
        case SNAKE_MODE:
            __snake_effect();
            break;
        case NINJA_MODE:
            __ninja_effect();
            break;
        case SPECTRUM_MODE:
            __spectrum_effect();
            break;
        case AI_CHAT_SPECTRUM_MODE:
            __ai_chat_spectrum_effect();
            break;
        case AGENT_MONITOR_MODE:
            __agent_monitor_effect();
            break;
        default:
            // Handle pixel art animations (mode >= EFFECT_ANIMATION_COUNT)
            if (g_animation_mode >= EFFECT_ANIMATION_COUNT && g_animation_mode != SAND_PHYSICS_MODE && g_animation_mode != TETRIS_MODE && g_animation_mode != SNAKE_MODE && g_animation_mode != NINJA_MODE && g_animation_mode != SPECTRUM_MODE && g_animation_mode != AI_CHAT_SPECTRUM_MODE && g_animation_mode != AGENT_MONITOR_MODE) {
                uint32_t pixel_art_idx = g_animation_mode - EFFECT_ANIMATION_COUNT;
                if (pixel_art_idx < g_registered_pixel_art_count) {
                    __pixel_art_effect(g_registered_pixel_arts[pixel_art_idx]);
                } else {
                    // Invalid pixel art index, reset to mode 0
                    g_animation_mode = 0;
                    continue;
                }
            } else {
                // Invalid animation mode, reset to mode 0
                g_animation_mode = 0;
                continue;
            }
            break;
        }

        tal_system_sleep(20); // Frame delay (~50 FPS)
    }

    PR_NOTICE("Pixel LED animation task stopped");
    g_pixels_thrd = NULL;
}

/***********************************************************
 * Network provisioning & Tuya IoT cloud
 ***********************************************************/

/**
 * @brief Tuya IoT event handler
 */
static void user_event_handler_on(tuya_iot_client_t *client, tuya_event_msg_t *event)
{
    PR_DEBUG("Tuya Event ID:%d(%s)", event->id, EVENT_ID2STR(event->id));

    switch (event->id) {
    case TUYA_EVENT_BIND_START:
        PR_NOTICE("Device Bind Start!");
        if (g_need_reset == 1) {
            PR_NOTICE("Device Reset!");
            tal_system_reset();
        }
        break;

    case TUYA_EVENT_DIRECT_MQTT_CONNECTED:
        PR_NOTICE("Direct MQTT connected (device activated, waiting for bind)");
        break;

    case TUYA_EVENT_MQTT_CONNECTED:
        PR_NOTICE("Device MQTT Connected! (online)");
        g_mqtt_connected = true;
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
        pixel_agent_ble_on_cloud_online();
#endif
        if (s_agent_ai_chat_ready) {
            __agent_ai_bootstrap();
        }
        if (tal_workq_schedule(WORKQ_SYSTEM, __mqtt_online_chime_job, NULL) != OPRT_OK) {
            __mqtt_online_chime_job(NULL);
        }
        break;

    case TUYA_EVENT_MQTT_DISCONNECT:
        PR_NOTICE("Device MQTT Disconnected! (offline)");
        g_mqtt_connected = false;
        break;

    case TUYA_EVENT_RESET:
        PR_NOTICE("Device Reset: %d", event->value.asInteger);
        g_need_reset = 1;
        break;

    case TUYA_EVENT_TIMESTAMP_SYNC:
        PR_NOTICE("Time sync: %d", event->value.asInteger);
        tal_time_set_posix(event->value.asInteger, 1);
        break;

    case TUYA_EVENT_DP_RECEIVE_OBJ: {
        dp_obj_recv_t *dpobj = event->value.dpobj;
        PR_DEBUG("DP OBJ recv cnt:%u", dpobj->dpscnt);
        // Echo back
        tuya_iot_dp_obj_report(client, dpobj->devid, dpobj->dps, dpobj->dpscnt, 0);
        break;
    }

    case TUYA_EVENT_DP_RECEIVE_RAW: {
        dp_raw_recv_t *dpraw = event->value.dpraw;
        PR_DEBUG("DP RAW recv dpid:%d len:%d", dpraw->dp.id, dpraw->dp.len);
        tuya_iot_dp_raw_report(client, dpraw->devid, &dpraw->dp, 3);
        break;
    }

    case TUYA_EVENT_UPGRADE_NOTIFY:
        PR_NOTICE("OTA upgrade notification received");
        break;

    default:
        break;
    }
}

/**
 * @brief Network status check callback
 */
static bool user_network_check(void)
{
    netmgr_status_e status = NETMGR_LINK_DOWN;
    netmgr_conn_get(NETCONN_AUTO, NETCONN_CMD_STATUS, &status);
    return status != NETMGR_LINK_DOWN;
}

/**
 * @brief Read reset counter from KV storage
 */
static int reset_count_read(uint8_t *count)
{
    uint8_t *read_buf = NULL;
    size_t read_len;

    OPERATE_RET rt = tal_kv_get(RESET_NETCNT_NAME, &read_buf, &read_len);
    if (rt != OPRT_OK || read_buf == NULL) {
        *count = 0;
        return OPRT_OK;
    }
    *count = read_buf[0];
    tal_kv_free(read_buf);
    PR_DEBUG("reset count is %d", *count);
    return OPRT_OK;
}

static int reset_count_write(uint8_t count)
{
    PR_DEBUG("reset count write %d", count);
    return tal_kv_set(RESET_NETCNT_NAME, &count, 1);
}

static void reset_netconfig_timer_cb(TIMER_ID timer_id, void *arg)
{
    reset_count_write(0);
    PR_DEBUG("reset cnt clear!");
}

static OPERATE_RET reset_netconfig_event_cb(void *data)
{
    reset_count_write(0);
    PR_DEBUG("reset cnt clear by reset event!");
    return OPRT_OK;
}

/**
 * @brief Check if reset threshold reached, trigger network reset
 */
static int reset_netconfig_check(void)
{
    uint8_t rst_cnt = 0;
    reset_count_read(&rst_cnt);
    if (rst_cnt < RESET_NETCNT_MAX) {
        return OPRT_OK;
    }

    PR_NOTICE("Reset counter reached %d, resetting network config!", rst_cnt);
    tal_event_subscribe(EVENT_RESET, "reset_netconfig", reset_netconfig_event_cb, SUBSCRIBE_TYPE_NORMAL);
    tuya_iot_reset(tuya_iot_client_get());

    return OPRT_OK;
}

/**
 * @brief Increment reset counter and start clear timer (called at boot)
 */
static int reset_netconfig_start(void)
{
    uint8_t rst_cnt = 0;
    reset_count_read(&rst_cnt);
    reset_count_write(++rst_cnt);

    PR_DEBUG("reset cnt=%d, start 5s clear timer", rst_cnt);
    TIMER_ID rst_timer;
    tal_sw_timer_create(reset_netconfig_timer_cb, NULL, &rst_timer);
    tal_sw_timer_start(rst_timer, 5000, TAL_TIMER_ONCE);

    return OPRT_OK;
}

/**
 * @brief Feed AI mic/TTS audio into spectrum ring buffer in chat mode
 * @param[in] event AI notification event
 * @return none
 */
static void __ai_chat_spectrum_feed(AI_NOTIFY_EVENT_T *event)
{
    if (g_animation_mode != AI_CHAT_SPECTRUM_MODE) {
        return;
    }
    if (event == NULL || event->type != AI_USER_EVT_MIC_DATA || event->data == NULL) {
        return;
    }
    if (ai_audio_player_is_playing() != 0) {
        return;
    }
    if (!g_spectrum_ok_holding) {
        return;
    }
    if (spectrum_ringbuf_init() != OPRT_OK) {
        return;
    }

    AI_NOTIFY_MIC_DATA_T *mic = (AI_NOTIFY_MIC_DATA_T *)event->data;
    spectrum_feed_pcm((const uint8_t *)mic->data, mic->data_len);
}

/**
 * @brief AI event callback — agent STT bridge + AI chat spectrum audio feed
 * @param[in] event AI notification event from ai_chat_main
 * @return none
 */
static void __ai_chat_event_cb(AI_NOTIFY_EVENT_T *event)
{
    if (event == NULL) {
        return;
    }

    __ai_chat_spectrum_feed(event);

    if (g_animation_mode == AGENT_MONITOR_MODE) {
        if (event->type == AI_USER_EVT_ASR_OK && event->data != NULL) {
            AI_NOTIFY_TEXT_T *text = (AI_NOTIFY_TEXT_T *)event->data;
            if (text->data != NULL && text->datalen > 0) {
                PR_NOTICE("STT result: %.*s", (int)text->datalen, text->data);
                pixel_agent_bridge_send_text(text->data);
            }
            __agent_board_led_off();
        }
        if (event->type == AI_USER_EVT_ASR_EMPTY) {
            PR_NOTICE("STT returned empty (no speech detected)");
            __agent_board_led_off();
        }
        if (event->type == AI_USER_EVT_ASR_ERROR) {
            PR_NOTICE("STT error (event=%d)", (int)event->type);
            __agent_board_led_off();
        }
        return;
    }

    if (g_animation_mode == AI_CHAT_SPECTRUM_MODE) {
        if (event->type == AI_USER_EVT_ASR_OK && event->data != NULL) {
            AI_NOTIFY_TEXT_T *text = (AI_NOTIFY_TEXT_T *)event->data;
            if (text->data != NULL && text->datalen > 0) {
                PR_NOTICE("AI chat ASR: %.*s", (int)text->datalen, text->data);
            }
            __agent_board_led_off();
        }
        if (event->type == AI_USER_EVT_ASR_EMPTY || event->type == AI_USER_EVT_ASR_ERROR) {
            __agent_board_led_off();
        }
        if (event->type == AI_USER_EVT_TTS_PRE || event->type == AI_USER_EVT_TTS_START) {
            memset(g_spectrum_band_peak, 0, sizeof(g_spectrum_band_peak));
        }
        if (event->type == AI_USER_EVT_PLAY_END || event->type == AI_USER_EVT_TTS_STOP ||
            event->type == AI_USER_EVT_TTS_ABORT) {
            spectrum_reset_state();
        }
    }
}

/**
 * @brief Main user function
 */
static void user_main(void)
{
    OPERATE_RET rt = OPRT_OK;

    //! cJSON memory hooks
    cJSON_InitHooks(&(cJSON_Hooks){.malloc_fn = tal_malloc, .free_fn = tal_free});

    tal_log_init(TAL_LOG_LEVEL_DEBUG, 1024, (TAL_LOG_OUTPUT_CB)tkl_log_output);

    PR_NOTICE("==========================================");
    PR_NOTICE("Tuya T5AI Pixel Demo (with Cloud)");
    PR_NOTICE("==========================================");
    PR_NOTICE("Project name:        %s", PROJECT_NAME);
    PR_NOTICE("App version:         %s", PROJECT_VERSION);
    PR_NOTICE("Compile time:        %s", __DATE__);
    PR_NOTICE("Platform board:      %s", PLATFORM_BOARD);
    PR_NOTICE("==========================================");

    // Initialize KV storage, timers, work queue
    tal_kv_init(&(tal_kv_cfg_t){
        .seed = "vmlkasdh93dlvlcy",
        .key  = "dflfuap134ddlduq",
    });
    tal_sw_timer_init();
    tal_workq_init();
    tal_time_service_init();
#if (PIXEL_AGENT_OWN_UART0 == 0)
    tal_cli_init();
#endif

    // Track boot count for network reset
    reset_netconfig_start();

    // Read device license — always use build-time UUID/key (per-board flash workflow)
    if (OPRT_OK != tuya_authorize_read(&g_license)) {
        PR_WARN("No license in KV, using build-time defaults.");
    } else if (strcmp(g_license.uuid, TUYA_OPENSDK_UUID) != 0) {
        PR_WARN("KV license differs from build — using build-time UUID for this image");
    }
    g_license.uuid    = TUYA_OPENSDK_UUID;
    g_license.authkey = TUYA_OPENSDK_AUTHKEY;
    PR_NOTICE("UUID: %s", g_license.uuid ? g_license.uuid : "(null)");

    // Initialize Tuya IoT client
    rt = tuya_iot_init(&g_iot_client, &(const tuya_iot_config_t){
        .software_ver  = PROJECT_VERSION,
        .productkey    = TUYA_PRODUCT_ID,
        .uuid          = g_license.uuid,
        .authkey       = g_license.authkey,
        .event_handler = user_event_handler_on,
        .network_check = user_network_check,
    });
    if (rt != OPRT_OK) {
        PR_ERR("tuya_iot_init failed: %d (device may lack license)", rt);
        g_iot_initialized = false;
    } else {
        g_iot_initialized = true;
        PR_NOTICE("Tuya IoT client initialized (product: %s)", TUYA_PRODUCT_ID);
    }

    // Initialize network manager (WiFi + BLE provisioning)
    if (g_iot_initialized) {
#if defined(ENABLE_BLUETOOTH) && (ENABLE_BLUETOOTH == 1)
        pixel_agent_ble_configure_security();
#endif
        netmgr_type_e net_type = 0;
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
        net_type |= NETCONN_WIFI;
#endif
        netmgr_init(net_type);
#if defined(ENABLE_WIFI) && (ENABLE_WIFI == 1)
        netmgr_conn_set(NETCONN_WIFI, NETCONN_CMD_NETCFG,
                        &(netcfg_args_t){.type = NETCFG_TUYA_BLE | NETCFG_TUYA_WIFI_AP});
        PR_NOTICE("WiFi + BLE/AP provisioning enabled");
#endif
    }

    // Initialize hardware
    rt = board_register_hardware();
    if (OPRT_OK != rt) {
        PR_ERR("board_register_hardware failed: %d", rt);
        return;
    }
    PR_NOTICE("Hardware initialized");

#if (PIXEL_AGENT_OWN_UART0 == 1)
    rt = pixel_agent_bridge_init();
    if (OPRT_OK != rt) {
        PR_WARN("Agent bridge UART init failed — use debug USB serial + pixel-agent-bridge");
    }
#endif
    rt = pixel_agent_ble_init();
    if (OPRT_OK != rt) {
        PR_WARN("Agent BLE init failed — serial-only mode");
    } else {
        pixel_agent_ble_apply_unique_adv_name();
    }

    // Initialize BMI260 sensor via BMI270 driver (chip_id 0x26 patched as compatible)
    PR_NOTICE("Initializing IMU sensor via BMI270 driver...");
    rt = board_bmi270_register();
    if (OPRT_OK == rt) {
        g_imu_handle = board_bmi270_get_handle();
        g_imu_ready = true;
        PR_NOTICE("IMU sensor initialized successfully (with config firmware)");
    } else {
        g_imu_ready = false;
        g_imu_handle = NULL;
        PR_ERR("IMU init FAILED (error=%d)", rt);
    }

    // Initialize buzzer
    rt = board_buzzer_init();
    if (OPRT_OK != rt) {
        PR_ERR("board_buzzer_init failed: %d", rt);
        return;
    }
    PR_NOTICE("Buzzer initialized");

    // Initialize buttons
    buzzer_demo_init_buttons();

    // Initialize pixel art animation registrations
    pixel_art_init_registrations();

    // Initialize pixel LED
    rt = pixel_led_init();
    if (OPRT_OK == rt) {
        PR_NOTICE("Pixel LED initialized successfully");

        // Start pixel LED animation thread
        THREAD_CFG_T thrd_param = {0};
        thrd_param.stackDepth = 1024 * 4;
        thrd_param.priority = THREAD_PRIO_2;
        thrd_param.thrdname = "pixel_anim";

        rt = tal_thread_create_and_start(&g_pixels_thrd, NULL, NULL, pixel_led_animation_task, NULL, &thrd_param);
        if (OPRT_OK == rt) {
            PR_NOTICE("Pixel LED animation thread started");
        } else {
            PR_ERR("Failed to start pixel LED animation thread: %d", rt);
        }
    } else {
        PR_ERR("Pixel LED initialization failed: %d", rt);
    }

    PR_NOTICE("==========================================");
    PR_NOTICE("Demo Ready! Starting Tuya IoT...");
    PR_NOTICE("==========================================");

    if (g_iot_initialized) {
        // AI chat init is deferred to Agent mode entry (to avoid conflict with spectrum mode)
        PR_NOTICE("AI chat deferred to agent mode entry");

        // Start Tuya IoT task (connects to cloud)
        tuya_iot_start(&g_iot_client);

        // Disable WiFi low-power mode for stable connection
        tkl_wifi_set_lp_mode(0, 0);

        // Check if network reset was triggered by repeated power cycles
        reset_netconfig_check();

        // Main loop: Tuya IoT yield handles cloud communication
        for (;;) {
            pixel_agent_bridge_poll();
            tuya_iot_yield(&g_iot_client);
        }
    } else {
        PR_WARN("IoT not initialized - running in offline mode (no cloud)");
        // Fallback: simple sleep loop, animations still run in separate thread
        for (;;) {
            pixel_agent_bridge_poll();
            tal_system_sleep(100);
        }
    }
}

/**
 * @brief main
 *
 * @param argc
 * @param argv
 * @return void
 */
#if OPERATING_SYSTEM == SYSTEM_LINUX
void main(int argc, char *argv[])
{
    user_main();
}
#else

/* Tuya thread handle */
static THREAD_HANDLE ty_app_thread = NULL;

/**
 * @brief  task thread
 *
 * @param[in] arg:Parameters when creating a task
 * @return none
 */
static void tuya_app_thread(void *arg)
{
    user_main();

    tal_thread_delete(ty_app_thread);
    ty_app_thread = NULL;
}

void tuya_app_main(void)
{
#if defined(PLATFORM_T5) && (PLATFORM_T5 == 1)
    extern VOID_T tkl_system_psram_malloc_force_set(BOOL_T enable);
    tkl_system_psram_malloc_force_set(TRUE);
#endif

    THREAD_CFG_T thrd_param = {0};
    thrd_param.stackDepth = 1024 * 6;
    thrd_param.priority = THREAD_PRIO_1;
    thrd_param.thrdname = "tuya_app_main";
    tal_thread_create_and_start(&ty_app_thread, NULL, NULL, tuya_app_thread, NULL, &thrd_param);
}
#endif