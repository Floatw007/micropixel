#include "platform/boards/esp32-p4-function-ev-board/i2s_audio.hpp"

#include "esp_codec_dev_defaults.h"
#include "platform/boards/esp32-p4-function-ev-board/board_config.hpp"
#include "platform/buses/i2c_executor.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {

I2sAudio::I2sAudio()
    : codec_(
          {
              .name = "Function EV ES8311/NS4150",
              .log_tag = "function_ev_audio",
              .i2c_port = board::kI2cPort,
              .i2s_port = board::kI2sPort,
              .master_clock = board::kI2sMasterClock,
              .bit_clock = board::kI2sBitClock,
              .word_select = board::kI2sWordSelect,
              .data_out = board::kI2sDataOut,
              .data_in = board::kI2sDataIn,
              .amplifier_enable = board::kAmplifierEnable,
              .codec_i2c_address = static_cast<uint8_t>(ES8311_CODEC_DEFAULT_ADDR >> 1U),
              .sample_rate = board::kAudioSampleRate,
              .i2c_clock_hz = 100000U,
              .amplifier_preroll_ms = 24U,
              .dma_descriptor_count = 6U,
              .dma_frame_count = 240U,
              .output_channels = 1U,
              .input_gain_db = 24.0F,
              .amplifier_active_low = false,
              .probe_before_attach = true,
              .probe_input_on_initialize = true,
          },
          {.amplifier_voltage = 5.0F, .codec_dac_voltage = 3.3F}) {}

esp_err_t I2sAudio::Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor) {
    return codec_.Configure(bus, executor);
}

}  // namespace micropixel::platform::esp32_p4_function_ev_board
