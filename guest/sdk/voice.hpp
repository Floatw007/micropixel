#ifndef MICROPIXEL_SDK_VOICE_HPP
#define MICROPIXEL_SDK_VOICE_HPP

#include <stdint.h>

#include "sdk/result.hpp"

namespace micropixel {

class Application;

enum class VoiceState : uint8_t {
    kIdle = 0,
    kWoken = 1,
    kListening = 2,
    kThinking = 3,
    kSpeaking = 4,
    kOffline = 5,
    kError = 6,
};

enum class VoiceTextKind : uint8_t {
    kTranscript = 1,
    kResponse = 2,
};

enum class VoiceLocalCommand : uint8_t {
    kSleep = 1,
    kWake = 2,
    kVolumeUp = 3,
    kVolumeDown = 4,
};

struct VoiceInfo final {
    bool configured{};
    bool route_online{};
    uint16_t sample_rate{};
    uint8_t channels{};
    uint8_t frame_duration_ms{};
    uint16_t max_transcript_bytes{};
    uint16_t max_response_bytes{};
    uint32_t profile_revision{};
    bool wake_word{};
    bool local_commands{};
    bool streaming_text{};
    bool streaming_audio{};
    bool touch_cancel{};
};

struct VoiceStatus final {
    VoiceState state{VoiceState::kIdle};
    uint32_t session_id{};
    uint16_t input_level_per_mille{};
    uint32_t transcript_revision{};
    uint32_t response_revision{};
};

struct VoiceTextChunk final {
    VoiceTextKind kind{VoiceTextKind::kTranscript};
    uint32_t revision{};
    uint32_t total_length{};
    uint16_t length{};
    bool final{};
    bool truncated{};
};

struct VoicePreferences final {
    uint16_t response_gain_per_mille{300U};
    bool local_commands_enabled{true};
    uint32_t revision{};
};

struct VoicePreferenceUpdate final {
    bool set_response_gain{};
    bool set_local_commands{};
    uint16_t response_gain_per_mille{300U};
    bool local_commands_enabled{true};
};

enum class VoiceDiagnosticKind : uint8_t {
    kMicrophone = 1,
    kSpeaker = 2,
};

struct VoiceDiagnostics final {
    bool configured{};
    bool route_online{};
    bool microphone_ready{};
    bool speaker_ready{};
    bool session_active{};
    uint16_t input_level_per_mille{};
    uint16_t queue_depth{};
    uint16_t queue_high_water{};
    uint32_t frames_sent{};
    uint32_t frames_dropped{};
    uint32_t first_transcript_ms{};
    uint32_t first_response_text_ms{};
    uint32_t first_audio_ms{};
    int32_t last_status{};
};

class Voice final {
   public:
    [[nodiscard]] Result<VoiceInfo> info() const;
    [[nodiscard]] Result<void> StartSession() const;
    [[nodiscard]] Result<void> CancelSession() const;
    [[nodiscard]] Result<VoiceStatus> state() const;
    [[nodiscard]] Result<VoicePreferences> preferences() const;
    [[nodiscard]] Result<void> SetPreferences(const VoicePreferenceUpdate& update) const;
    [[nodiscard]] Result<VoiceDiagnostics> diagnostics() const;
    [[nodiscard]] Result<void> RunDiagnostic(VoiceDiagnosticKind kind) const;
    [[nodiscard]] Result<void> ClearContext() const;
    [[nodiscard]] Result<VoiceTextChunk> ReadText(VoiceTextKind kind, uint32_t revision, uint32_t offset,
                                                  char* destination, uint32_t capacity) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Voice(CapabilityToken) noexcept {}
    friend class Application;
};

}  // namespace micropixel

#endif
