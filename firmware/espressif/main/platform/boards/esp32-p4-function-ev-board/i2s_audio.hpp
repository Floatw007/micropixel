#pragma once

#include "device/contracts/audio_input.hpp"
#include "driver/i2c_master.h"
#include "platform/audio/audio_output_peripheral.hpp"
#include "platform/audio/es8311_i2s_audio_sink.hpp"

namespace micropixel::platform::buses {
class I2cExecutor;
}

namespace micropixel::platform::esp32_p4_function_ev_board {

class I2sAudio final : public audio::AudioOutputPeripheral, public device::AudioInput {
   public:
    I2sAudio();

    [[nodiscard]] esp_err_t Configure(i2c_master_bus_handle_t bus, buses::I2cExecutor& executor);
    [[nodiscard]] esp_err_t Initialize() override { return codec_.Initialize(); }
    [[nodiscard]] esp_err_t Start(int32_t* frames, uint32_t count) override { return codec_.Start(frames, count); }
    [[nodiscard]] esp_err_t Write(const int32_t* frames, uint32_t count) override {
        return codec_.Write(frames, count);
    }
    [[nodiscard]] esp_err_t Stop() override { return codec_.Stop(); }
    void Shutdown() override { codec_.Shutdown(); }
    [[nodiscard]] const char* Name() const override { return codec_.Name(); }
    [[nodiscard]] uint32_t SampleRate() const override { return codec_.SampleRate(); }
    [[nodiscard]] int32_t GetInfo(micropixel_audio_input_info_t& info) override { return codec_.GetInfo(info); }
    [[nodiscard]] int32_t Read(int16_t* samples, uint32_t capacity, uint32_t& frames_read) override {
        return codec_.Read(samples, capacity, frames_read);
    }

   private:
    audio::Es8311I2sAudioSink codec_;
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board
