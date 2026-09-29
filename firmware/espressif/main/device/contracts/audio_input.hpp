#ifndef MICROPIXEL_DEVICE_AUDIO_INPUT_HPP
#define MICROPIXEL_DEVICE_AUDIO_INPUT_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

class AudioInput {
   public:
    virtual ~AudioInput() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_audio_input_info_t& info) = 0;
    [[nodiscard]] virtual int32_t Read(int16_t* mono_samples, uint32_t frame_capacity, uint32_t& frames_read) = 0;
};

}  // namespace micropixel::device

#endif
