#include "sdk/voice.hpp"

#include "abi/micropixel_abi.h"
#include "runtime/panic.hpp"
#include "runtime/service_binding.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::OpenService;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache voice_core_service;
ServiceCache voice_management_service;
alignas(4) uint8_t read_buffer[sizeof(micropixel_voice_read_text_response_t) + MICROPIXEL_VOICE_MAX_READ_BYTES];

bool ValidState(uint8_t state) { return state <= MICROPIXEL_VOICE_STATE_ERROR; }

}  // namespace

namespace micropixel {

Result<VoiceInfo> Voice::info() const {
    micropixel_voice_info_t raw{};
    int32_t status = OpenService(voice_core_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR, 0U);
    uint32_t response_size = 0U;
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(voice_core_service, MICROPIXEL_VOICE_METHOD_GET_INFO, nullptr, 0U, &raw, sizeof(raw),
                             response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(raw) || raw.size != sizeof(raw) || raw.configured > 1U || raw.route_online > 1U ||
        raw.channels != 1U || raw.sample_rate == 0U || raw.frame_duration_ms == 0U || raw.max_transcript_bytes == 0U ||
        raw.max_response_bytes == 0U) {
        runtime::Panic("voice.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return VoiceInfo{
        raw.configured != 0U,
        raw.route_online != 0U,
        raw.sample_rate,
        raw.channels,
        raw.frame_duration_ms,
        raw.max_transcript_bytes,
        raw.max_response_bytes,
        raw.profile_revision,
        (raw.capabilities & MICROPIXEL_VOICE_CAP_WAKE_WORD) != 0U,
        (raw.capabilities & MICROPIXEL_VOICE_CAP_LOCAL_COMMANDS) != 0U,
        (raw.capabilities & MICROPIXEL_VOICE_CAP_STREAMING_TEXT) != 0U,
        (raw.capabilities & MICROPIXEL_VOICE_CAP_STREAMING_AUDIO) != 0U,
        (raw.capabilities & MICROPIXEL_VOICE_CAP_TOUCH_CANCEL) != 0U,
    };
}

Result<void> Voice::StartSession() const {
    int32_t status = OpenService(voice_core_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR, 0U);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(voice_core_service, MICROPIXEL_VOICE_METHOD_START_SESSION, nullptr, 0U);
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> Voice::CancelSession() const {
    int32_t status = OpenService(voice_core_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR, 0U);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(voice_core_service, MICROPIXEL_VOICE_METHOD_CANCEL_SESSION, nullptr, 0U);
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<VoiceStatus> Voice::state() const {
    micropixel_voice_state_response_t raw{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(voice_core_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR, 0U);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(voice_core_service, MICROPIXEL_VOICE_METHOD_GET_STATE, nullptr, 0U, &raw, sizeof(raw),
                             response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(raw) || raw.size != sizeof(raw) || !ValidState(raw.state) || raw.reserved0 != 0U ||
        raw.reserved1 != 0U || raw.input_level_per_mille > 1000U) {
        runtime::Panic("voice.state.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return VoiceStatus{static_cast<VoiceState>(raw.state), raw.session_id, raw.input_level_per_mille,
                       raw.transcript_revision, raw.response_revision};
}

Result<VoicePreferences> Voice::preferences() const {
    micropixel_voice_preferences_t raw{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(voice_management_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR,
                                 MICROPIXEL_VOICE_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(voice_management_service, MICROPIXEL_VOICE_METHOD_GET_PREFERENCES, nullptr, 0U, &raw,
                             sizeof(raw), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(raw) || raw.size != sizeof(raw) || raw.response_gain_per_mille > 1000U ||
        raw.local_commands_enabled > 1U || raw.reserved0 != 0U || raw.reserved1 != 0U) {
        runtime::Panic("voice.preferences.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return VoicePreferences{raw.response_gain_per_mille, raw.local_commands_enabled != 0U, raw.revision};
}

Result<void> Voice::SetPreferences(const VoicePreferenceUpdate& update) const {
    if ((!update.set_response_gain && !update.set_local_commands) || update.response_gain_per_mille > 1000U) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_voice_set_preferences_request_t request{};
    request.size = sizeof(request);
    if (update.set_response_gain) request.mask |= MICROPIXEL_VOICE_PREFERENCE_RESPONSE_GAIN;
    if (update.set_local_commands) request.mask |= MICROPIXEL_VOICE_PREFERENCE_LOCAL_COMMANDS;
    request.response_gain_per_mille = update.response_gain_per_mille;
    request.local_commands_enabled = update.local_commands_enabled ? 1U : 0U;
    int32_t status = OpenService(voice_management_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR,
                                 MICROPIXEL_VOICE_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(voice_management_service, MICROPIXEL_VOICE_METHOD_SET_PREFERENCES, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<VoiceDiagnostics> Voice::diagnostics() const {
    micropixel_voice_diagnostics_t raw{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(voice_management_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR,
                                 MICROPIXEL_VOICE_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(voice_management_service, MICROPIXEL_VOICE_METHOD_GET_DIAGNOSTICS, nullptr, 0U, &raw,
                             sizeof(raw), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    constexpr uint16_t kKnownFlags = MICROPIXEL_VOICE_DIAGNOSTIC_CONFIGURED | MICROPIXEL_VOICE_DIAGNOSTIC_ROUTE_ONLINE |
                                     MICROPIXEL_VOICE_DIAGNOSTIC_MICROPHONE_READY |
                                     MICROPIXEL_VOICE_DIAGNOSTIC_SPEAKER_READY |
                                     MICROPIXEL_VOICE_DIAGNOSTIC_SESSION_ACTIVE;
    if (response_size != sizeof(raw) || raw.size != sizeof(raw) || (raw.flags & ~kKnownFlags) != 0U ||
        raw.input_level_per_mille > 1000U || raw.reserved0 != 0U) {
        runtime::Panic("voice.diagnostics.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return VoiceDiagnostics{
        (raw.flags & MICROPIXEL_VOICE_DIAGNOSTIC_CONFIGURED) != 0U,
        (raw.flags & MICROPIXEL_VOICE_DIAGNOSTIC_ROUTE_ONLINE) != 0U,
        (raw.flags & MICROPIXEL_VOICE_DIAGNOSTIC_MICROPHONE_READY) != 0U,
        (raw.flags & MICROPIXEL_VOICE_DIAGNOSTIC_SPEAKER_READY) != 0U,
        (raw.flags & MICROPIXEL_VOICE_DIAGNOSTIC_SESSION_ACTIVE) != 0U,
        raw.input_level_per_mille,
        raw.queue_depth,
        raw.queue_high_water,
        raw.frames_sent,
        raw.frames_dropped,
        raw.first_transcript_ms,
        raw.first_response_text_ms,
        raw.first_audio_ms,
        raw.last_status,
    };
}

Result<void> Voice::RunDiagnostic(VoiceDiagnosticKind kind) const {
    const uint8_t raw_kind = static_cast<uint8_t>(kind);
    if (raw_kind < MICROPIXEL_VOICE_DIAGNOSTIC_MICROPHONE || raw_kind > MICROPIXEL_VOICE_DIAGNOSTIC_SPEAKER) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_voice_run_diagnostic_request_t request{};
    request.size = sizeof(request);
    request.kind = raw_kind;
    int32_t status = OpenService(voice_management_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR,
                                 MICROPIXEL_VOICE_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(voice_management_service, MICROPIXEL_VOICE_METHOD_RUN_DIAGNOSTIC, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> Voice::ClearContext() const {
    int32_t status = OpenService(voice_management_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR,
                                 MICROPIXEL_VOICE_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(voice_management_service, MICROPIXEL_VOICE_METHOD_CLEAR_CONTEXT, nullptr, 0U);
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<VoiceTextChunk> Voice::ReadText(VoiceTextKind kind, uint32_t revision, uint32_t offset, char* destination,
                                       uint32_t capacity) const {
    const uint8_t raw_kind = static_cast<uint8_t>(kind);
    if (raw_kind < MICROPIXEL_VOICE_TEXT_TRANSCRIPT || raw_kind > MICROPIXEL_VOICE_TEXT_RESPONSE ||
        destination == nullptr || capacity == 0U || capacity > MICROPIXEL_VOICE_MAX_READ_BYTES) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_voice_read_text_request_t request{};
    request.size = sizeof(request);
    request.kind = raw_kind;
    request.revision = revision;
    request.offset = offset;
    request.capacity = capacity;
    uint32_t response_size = 0U;
    int32_t status = OpenService(voice_core_service, MICROPIXEL_SERVICE_VOICE, MICROPIXEL_VOICE_INTERFACE_MAJOR, 0U);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(voice_core_service, MICROPIXEL_VOICE_METHOD_READ_TEXT, &request, sizeof(request),
                             read_buffer, sizeof(micropixel_voice_read_text_response_t) + capacity, response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    micropixel_voice_read_text_response_t header{};
    if (response_size < sizeof(header)) runtime::Panic("voice.read.response", MICROPIXEL_STATUS_INTERNAL);
    CopyBytes(&header, read_buffer, sizeof(header));
    if (header.size != response_size || header.kind != raw_kind || header.revision != revision ||
        header.reserved0 != 0U || header.chunk_length > capacity ||
        response_size != sizeof(header) + header.chunk_length || offset > header.total_length ||
        header.chunk_length > header.total_length - offset ||
        (header.flags & ~(MICROPIXEL_VOICE_TEXT_FINAL | MICROPIXEL_VOICE_TEXT_TRUNCATED)) != 0U) {
        runtime::Panic("voice.read.response", MICROPIXEL_STATUS_INTERNAL);
    }
    CopyBytes(destination, read_buffer + sizeof(header), header.chunk_length);
    return VoiceTextChunk{kind,
                          revision,
                          header.total_length,
                          header.chunk_length,
                          (header.flags & MICROPIXEL_VOICE_TEXT_FINAL) != 0U,
                          (header.flags & MICROPIXEL_VOICE_TEXT_TRUNCATED) != 0U};
}

}  // namespace micropixel
