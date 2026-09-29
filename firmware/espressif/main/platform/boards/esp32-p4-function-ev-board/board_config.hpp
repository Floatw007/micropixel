#pragma once

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "driver/ledc.h"
#include "driver/sdmmc_host.h"

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
inline constexpr int kSdLdoChannel = 4;
inline constexpr int kSdMmcSlot = SDMMC_HOST_SLOT_0;
inline constexpr int kSdBusWidth = 4;
inline constexpr gpio_num_t kSdClock = GPIO_NUM_43;
inline constexpr gpio_num_t kSdCommand = GPIO_NUM_44;
inline constexpr gpio_num_t kSdData0 = GPIO_NUM_39;
inline constexpr gpio_num_t kSdData1 = GPIO_NUM_40;
inline constexpr gpio_num_t kSdData2 = GPIO_NUM_41;
inline constexpr gpio_num_t kSdData3 = GPIO_NUM_42;
inline constexpr uint32_t kSdMaximumFrequencyKhz = 20000U;
inline constexpr int kI2sPort = I2S_NUM_0;
inline constexpr gpio_num_t kI2sDataOut = GPIO_NUM_9;
inline constexpr gpio_num_t kI2sDataIn = GPIO_NUM_11;
inline constexpr gpio_num_t kI2sWordSelect = GPIO_NUM_10;
inline constexpr gpio_num_t kI2sBitClock = GPIO_NUM_12;
inline constexpr gpio_num_t kI2sMasterClock = GPIO_NUM_13;
inline constexpr gpio_num_t kAmplifierEnable = GPIO_NUM_53;
inline constexpr uint32_t kAudioSampleRate = 16000U;
inline constexpr int kDsiLaneCount = 2;
inline constexpr int kDsiLaneBitRateMbps = 1000;
inline constexpr ledc_mode_t kBacklightLedcMode = LEDC_LOW_SPEED_MODE;
inline constexpr ledc_timer_t kBacklightLedcTimer = LEDC_TIMER_1;
inline constexpr ledc_channel_t kBacklightLedcChannel = LEDC_CHANNEL_1;

}  // namespace micropixel::platform::esp32_p4_function_ev_board::board
