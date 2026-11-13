#ifndef MICROPIXEL_DEVICE_CONTRACTS_VOICE_HPP
#define MICROPIXEL_DEVICE_CONTRACTS_VOICE_HPP

#include <cstdint>
#include <span>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

struct VoiceTextResult final {
    uint32_t revision{};
    uint32_t total_length{};
    uint16_t chunk_length{};
    uint8_t flags{};
};

struct VoiceUpdate final {
    uint16_t event_id{};
    int32_t status{MICROPIXEL_STATUS_OK};
    micropixel_voice_event_payload_t payload{};
    uint64_t timestamp_us{};
};

using VoiceUpdateSink = void (*)(void* context, const VoiceUpdate& update);

class Voice {
   public:
    virtual ~Voice() = default;

    [[nodiscard]] virtual int32_t GetInfo(micropixel_voice_info_t& info) const = 0;
    [[nodiscard]] virtual int32_t StartSession() = 0;
    [[nodiscard]] virtual int32_t CancelSession() = 0;
    [[nodiscard]] virtual int32_t GetState(micropixel_voice_state_response_t& state) const = 0;
    [[nodiscard]] virtual int32_t ReadText(uint8_t kind, uint32_t revision, uint32_t offset,
                                           std::span<uint8_t> destination, VoiceTextResult& result) const = 0;
    [[nodiscard]] virtual int32_t GetPreferences(micropixel_voice_preferences_t& preferences) const = 0;
    [[nodiscard]] virtual int32_t SetPreferences(const micropixel_voice_set_preferences_request_t& preferences) = 0;
    [[nodiscard]] virtual int32_t GetDiagnostics(micropixel_voice_diagnostics_t& diagnostics) const = 0;
    [[nodiscard]] virtual int32_t RunDiagnostic(uint8_t kind) = 0;
    [[nodiscard]] virtual int32_t ClearContext() = 0;
    virtual void BindUpdateSink(VoiceUpdateSink sink, void* context) = 0;
    virtual void UnbindUpdateSink(void* context) = 0;
};

}  // namespace micropixel::device

#endif
