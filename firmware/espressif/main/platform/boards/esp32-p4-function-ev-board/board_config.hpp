#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/ledc.h"

namespace micropixel::platform::esp32_p4_function_ev_board::board {

inline constexpr int kDisplayWidth = 1024;
inline constexpr int kDisplayHeight = 600;
inline constexpr gpio_num_t kDisplayReset = GPIO_NUM_27;
inline constexpr gpio_num_t kDisplayBacklight = GPIO_NUM_26;
inline constexpr i2c_port_num_t kI2cPort = I2C_NUM_1;
inline constexpr gpio_num_t kI2cData = GPIO_NUM_7;
inline constexpr gpio_num_t kI2cClock = GPIO_NUM_8;
inline constexpr gpio_num_t kTouchReset = GPIO_NUM_NC;
inline constexpr gpio_num_t kTouchInterrupt = GPIO_NUM_NC;
inline constexpr int kDsiLdoChannel = 3;
inline constexpr int kDsiLdoMillivolts = 2500;
inline constexpr int kDsiLaneCount = 2;
inline constexpr int kDsiLaneBitRateMbps = 1000;
inline constexpr ledc_mode_t kBacklightLedcMode = LEDC_LOW_SPEED_MODE;
inline constexpr ledc_timer_t kBacklightLedcTimer = LEDC_TIMER_1;
inline constexpr ledc_channel_t kBacklightLedcChannel = LEDC_CHANNEL_1;

}  // namespace micropixel::platform::esp32_p4_function_ev_board::board
