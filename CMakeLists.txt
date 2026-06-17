##
# @file CMakeLists.txt
# @brief 
#/

# APP_PATH
set(APP_PATH ${CMAKE_CURRENT_LIST_DIR})

# APP_NAME
get_filename_component(APP_NAME ${APP_PATH} NAME)

# APP_SRC
aux_source_directory(${APP_PATH}/src APP_SRC)

# Clawd theme GIFs (buddy_pixel / clawd-on-desk via gif_to_c.py; only states used by agent mode)
set(CLAWD_GIF_DIR ${TOP_SOURCE_DIR}/apps/buddy_pixel/src/claude_status_gif)
set(CLAWD_GIF_SRC
    ${CLAWD_GIF_DIR}/gif_clawd_idle.c
    ${CLAWD_GIF_DIR}/gif_clawd_thinking.c
    ${CLAWD_GIF_DIR}/gif_clawd_typing.c
    ${CLAWD_GIF_DIR}/gif_clawd_juggling.c
    ${CLAWD_GIF_DIR}/gif_clawd_notification.c
    ${CLAWD_GIF_DIR}/gif_clawd_error.c
    ${CLAWD_GIF_DIR}/gif_clawd_happy.c
)

# APP_INC
set(APP_INC
    ${APP_PATH}/include
    ${CLAWD_GIF_DIR}
)

# APP_OPTIONS
set(APP_OPTIONS "")  # -W

########################################
# Target Configure
########################################
add_library(${EXAMPLE_LIB})
message(STATUS "EXAMPLE_LIB:${APP_PATH}")

target_sources(${EXAMPLE_LIB}
    PRIVATE
        ${APP_SRC}
        ${CLAWD_GIF_SRC}
    )

target_include_directories(${EXAMPLE_LIB}
    PRIVATE
        ${APP_INC}
    )

target_compile_options(${EXAMPLE_LIB}
    PRIVATE
        ${APP_OPTIONS}
    )

########################################
# Add AI components
########################################
set(AI_AUDIO_USE_REAL_PLAYER 1)
add_subdirectory(${TOP_SOURCE_DIR}/apps/tuya.ai/ai_components ${CMAKE_BINARY_DIR}/ai_components)
# AI components include paths (flat + sub-component includes)
target_include_directories(${EXAMPLE_LIB} PRIVATE
    ${TOP_SOURCE_DIR}/apps/tuya.ai/ai_components
    ${TOP_SOURCE_DIR}/apps/tuya.ai/ai_components/ai_audio/include
    ${TOP_SOURCE_DIR}/apps/tuya.ai/ai_components/ai_main/include
    ${TOP_SOURCE_DIR}/apps/tuya.ai/ai_components/utility/include
    ${TOP_SOURCE_DIR}/apps/tuya.ai/ai_components/ai_protocol/include
    ${TOP_SOURCE_DIR}/src/audio_player/include
)
# Enable AI audio input for STT only (no cloud AI response/playback)
target_compile_definitions(${EXAMPLE_LIB} PRIVATE
    ENABLE_COMP_AI_PRESENT_MODE=1
    ENABLE_COMP_AI_MODE_HOLD=1
    ENABLE_COMP_AI_AUDIO=1
    TAL_BLE_SERVICE_VERSION=3
    TY_HS_BLE_SM_LEGACY=1
    TY_HS_BLE_SM_BONDING=0
    TY_HS_BLE_SM_MITM=0
    TY_HS_BLE_SM_OUR_KEY_DIST=0
    TY_HS_BLE_SM_THEIR_KEY_DIST=0
)

# NimBLE headers for tal_ble_agent_event_hook
get_filename_component(SDK_ROOT "${TOP_SOURCE_DIR}" ABSOLUTE)
set(NIMBLE_INC ${SDK_ROOT}/src/tal_bluetooth/nimble/include)
set(NIMBLE_HOST_INC ${SDK_ROOT}/src/tal_bluetooth/nimble/host)
target_include_directories(${EXAMPLE_LIB} PRIVATE
    ${NIMBLE_INC}
    ${NIMBLE_HOST_INC}
)
