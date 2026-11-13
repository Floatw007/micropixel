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
    // Host features can lease the microphone for a continuous real-time
    // session. Ordinary Guest reads must report WOULD_BLOCK while leased.
    [[nodiscard]] virtual int32_t BeginCapture(void* owner) {
        return owner != nullptr ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    [[nodiscard]] virtual int32_t ReadCapture(void* owner, int16_t* mono_samples, uint32_t frame_capacity,
                                              uint32_t& frames_read) {
        return owner != nullptr ? Read(mono_samples, frame_capacity, frames_read)
                                : static_cast<int32_t>(MICROPIXEL_STATUS_INVALID_ARGUMENT);
    }
    virtual void EndCapture(void* owner) { (void)owner; }
};

}  // namespace micropixel::device

#endif
