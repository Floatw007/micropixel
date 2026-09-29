#include <stdint.h>

#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

int main() {
    micropixel::Application app;
    micropixel::Audio audio = app.audio();

    auto audio_info = audio.info();
    if (!audio_info || !audio_info->supports_input_pcm) {
        app.log().Error("audio_input: signed 16-bit microphone capability missing");
        return 70;
    }

    auto input_info = audio.input_info();
    if (!input_info || input_info->sample_rate == 0U || input_info->channels != 1U ||
        input_info->bits_per_sample != 16U || input_info->max_read_frames == 0U ||
        input_info->max_read_frames > MICROPIXEL_AUDIO_INPUT_MAX_READ_FRAMES) {
        app.log().Error("audio_input: invalid input format");
        return 71;
    }

    int16_t samples[MICROPIXEL_AUDIO_INPUT_MAX_READ_FRAMES]{};
    if (audio.ReadInput(nullptr, 1U) || audio.ReadInput(samples, 0U) ||
        audio.ReadInput(samples, MICROPIXEL_AUDIO_INPUT_MAX_READ_FRAMES + 1U)) {
        return 72;
    }

    int16_t minimum = INT16_MAX;
    int16_t maximum = INT16_MIN;
    uint32_t frames_read = 0U;
    for (uint32_t block = 0U; block < 4U; ++block) {
        auto read = audio.ReadInput(samples, input_info->max_read_frames);
        if (!read || *read == 0U || *read > input_info->max_read_frames) {
            app.log().Error("audio_input: sample read failed");
            return 73;
        }
        frames_read += *read;
        for (uint32_t index = 0U; index < *read; ++index) {
            if (samples[index] < minimum) {
                minimum = samples[index];
            }
            if (samples[index] > maximum) {
                maximum = samples[index];
            }
        }
    }
    if (frames_read == 0U || minimum == maximum) {
        app.log().Error("audio_input: captured data is static");
        return 74;
    }

    app.log().Info("audio_input: microphone format and changing PCM samples verified");
    return 0;
}
