#pragma once

#include "host/ui/lvgl/square_common/profiles/landscape_1024x600.hpp"
#include "host/ui/lvgl/square_common/status_layer_transition.hpp"
#include "lvgl.h"
#include "platform/boards/esp32-p4-function-ev-board/board_config.hpp"
#include "platform/boards/esp32-p4-function-ev-board/board_hardware.hpp"
#include "platform/boards/esp32-p4-function-ev-board/display/display_pipeline.hpp"
#include "platform/boards/esp32-p4-function-ev-board/display_idle_controller.hpp"
#include "platform/buses/i2c_executor.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/fonts/font_registry.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/transports/usb_serial_jtag_local_control.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board::detail {

namespace ui_profile = host_ui::lvgl::square_common::profiles::landscape_1024x600;

inline constexpr char kTag[] = "p4_function_ev";
inline constexpr int kWidth = board::kDisplayWidth;
inline constexpr int kHeight = board::kDisplayHeight;
inline constexpr uint32_t kRefreshPeriodMs = 1000U;
inline constexpr uint32_t kLvglIdleTimeoutMs = 1000U;
inline constexpr uint32_t kLvglMaximumWaitMs = 120U * 1000U;
inline constexpr uint64_t kTouchActivePollingIntervalUs = 10000U;
inline constexpr uint64_t kTouchIdlePollingIntervalUs = 50000U;
static_assert(kWidth == ui_profile::Layout::kWidth);
static_assert(kHeight == ui_profile::Layout::kHeight);

struct InputState final {
    buses::I2cExecutor i2c_executor{};
    input::EspLcdTouchInput touch_input{
        kWidth,
        kHeight,
        micropixel::device::kMaxTouchPoints,
        {.active_interval_us = kTouchActivePollingIntervalUs, .idle_interval_us = kTouchIdlePollingIntervalUs}};
};

struct BoardState final {
    BoardState(BoardHardware& board_hardware, InputState& input)
        : hardware(board_hardware),
          display_pipeline(hardware, kWidth, kHeight),
          i2c_executor(input.i2c_executor),
          touch_input(input.touch_input) {}

    BoardHardware& hardware;
    FunctionEvDisplayPipeline display_pipeline;
    buses::I2cExecutor& i2c_executor;
    input::EspLcdTouchInput& touch_input;
    lv_display_t* display{};
    lvgl::FontRegistry fonts{};
    lvgl::GuestGraphicsEngine guest_graphics{kWidth, kHeight, fonts};
    FunctionEvDisplayIdleController display_idle{display_pipeline, guest_graphics};
    host_ui::lvgl::square_common::StaticStatusLayerTransition status_transition{};
    host_ui::lvgl::square_common::SquareSystemUiState ui{touch_input, guest_graphics, status_transition,
                                                         ui_profile::kSystemUiProfile};
    transports::UsbSerialJtagLocalControl local_control{};
    transports::DevelopmentDisplayControl development_display{};
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board::detail
