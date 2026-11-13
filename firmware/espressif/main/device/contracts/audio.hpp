#ifndef MICROPIXEL_DEVICE_AUDIO_HPP
#define MICROPIXEL_DEVICE_AUDIO_HPP

#include <cstdint>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

using PcmStreamHandle = uint32_t;

struct PcmReadResult final {
    uint32_t frames{};
    bool finished{};
};

using PcmReadCallback = PcmReadResult (*)(void* context, int16_t* mono_samples, uint32_t frame_capacity);

struct PcmSource final {
    PcmReadCallback read{};
    void* context{};
};

struct PcmCompletion final {
    uint32_t token{};
    int32_t status{MICROPIXEL_STATUS_OK};
};

using PcmCompletionSink = void (*)(void* context, const PcmCompletion& completion);

class Audio {
   public:
    virtual ~Audio() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_audio_info_t& info) = 0;
    [[nodiscard]] virtual int32_t PlayTone(const micropixel_audio_tone_t& tone) = 0;
    [[nodiscard]] virtual int32_t StartPcm(const PcmSource& source, uint32_t token, uint16_t volume_per_mille,
                                           PcmStreamHandle& handle_out) = 0;
    [[nodiscard]] virtual int32_t PausePcm(PcmStreamHandle handle) = 0;
    [[nodiscard]] virtual int32_t ResumePcm(PcmStreamHandle handle) = 0;
    [[nodiscard]] virtual int32_t SetPcmVolume(PcmStreamHandle handle, uint16_t volume_per_mille) = 0;
    [[nodiscard]] virtual int32_t StopPcm(PcmStreamHandle handle) = 0;
    virtual void BindPcmCompletionSink(PcmCompletionSink sink, void* context) = 0;
    virtual void UnbindPcmCompletionSink(void* context) = 0;
    [[nodiscard]] virtual int32_t StopAll() = 0;
    // Host-owned controls such as local speech commands may adjust the
    // physical master volume without exposing that control to Guests.
    [[nodiscard]] virtual int32_t AdjustMasterVolumePercent(int8_t delta, uint8_t& percent_out) {
        (void)delta;
        percent_out = 0U;
        return MICROPIXEL_STATUS_UNSUPPORTED;
    }
    // Host capture pipelines can temporarily stop the physical output without
    // discarding Guest voices. Success means the output peripheral has
    // acknowledged the pause and no longer owns the shared codec/I2S path.
    [[nodiscard]] virtual int32_t PauseOutputForCapture(void* owner) {
        return owner == nullptr ? MICROPIXEL_STATUS_INVALID_ARGUMENT : MICROPIXEL_STATUS_UNSUPPORTED;
    }
    virtual void ResumeOutputAfterCapture(void* owner) { (void)owner; }
    // These methods also define the owning AppSession's foreground state:
    // ResumeAll keeps output ready, while SuspendAll permits idle shutdown.
    [[nodiscard]] virtual int32_t SuspendAll() = 0;
    [[nodiscard]] virtual int32_t ResumeAll() = 0;
};

}  // namespace micropixel::device

#endif
