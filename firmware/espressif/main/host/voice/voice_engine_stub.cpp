#include <cstring>

#include "host/voice/voice_engine.hpp"

namespace micropixel::host::voice {

VoiceEngine::VoiceEngine(device::Audio& audio, device::AudioInput& input, network::Network& network)
    : audio_(audio), input_(input), network_(network) {
    mutex_ready_ = false;
}

VoiceEngine::~VoiceEngine() = default;

int32_t VoiceEngine::Configure(std::span<const uint8_t>) { return MICROPIXEL_STATUS_UNSUPPORTED; }

int32_t VoiceEngine::ClearProfile() { return MICROPIXEL_STATUS_UNSUPPORTED; }

VoiceProfileStatus VoiceEngine::ProfileStatus() const { return {}; }

int32_t VoiceEngine::GetInfo(micropixel_voice_info_t& info) const {
    info = {};
    info.size = sizeof(info);
    info.sample_rate = kSampleRate;
    info.channels = 1U;
    info.frame_duration_ms = 20U;
    info.max_transcript_bytes = kTranscriptCapacity;
    info.max_response_bytes = kResponseCapacity;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::StartSession() { return MICROPIXEL_STATUS_UNSUPPORTED; }

int32_t VoiceEngine::CancelSession() { return MICROPIXEL_STATUS_UNSUPPORTED; }

int32_t VoiceEngine::GetState(micropixel_voice_state_response_t& state) const {
    state = {};
    state.size = sizeof(state);
    state.state = MICROPIXEL_VOICE_STATE_OFFLINE;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::ReadText(uint8_t, uint32_t, uint32_t, std::span<uint8_t>, device::VoiceTextResult&) const {
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

int32_t VoiceEngine::GetPreferences(micropixel_voice_preferences_t& preferences) const {
    preferences = {};
    preferences.size = sizeof(preferences);
    preferences.response_gain_per_mille = 300U;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::SetPreferences(const micropixel_voice_set_preferences_request_t&) {
    return MICROPIXEL_STATUS_UNSUPPORTED;
}

int32_t VoiceEngine::GetDiagnostics(micropixel_voice_diagnostics_t& diagnostics) const {
    diagnostics = {};
    diagnostics.size = sizeof(diagnostics);
    diagnostics.last_status = MICROPIXEL_STATUS_UNSUPPORTED;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::RunDiagnostic(uint8_t) { return MICROPIXEL_STATUS_UNSUPPORTED; }

int32_t VoiceEngine::ClearContext() { return MICROPIXEL_STATUS_UNSUPPORTED; }

void VoiceEngine::BindUpdateSink(device::VoiceUpdateSink sink, void* context) {
    sink_ = sink;
    sink_context_ = context;
}

void VoiceEngine::UnbindUpdateSink(void* context) {
    if (sink_context_ == context) {
        sink_ = nullptr;
        sink_context_ = nullptr;
    }
}

}  // namespace micropixel::host::voice
