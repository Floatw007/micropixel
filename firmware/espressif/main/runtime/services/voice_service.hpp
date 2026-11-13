#ifndef MICROPIXEL_RUNTIME_SERVICES_VOICE_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_VOICE_SERVICE_HPP

#include <cstdint>
#include <span>

#include "device/device_services.hpp"

namespace micropixel::runtime {

class EventQueue;

class VoiceService final {
   public:
    VoiceService(device::VoiceService& voice, EventQueue& events, int64_t clock_origin_us);
    VoiceService(const VoiceService&) = delete;
    VoiceService& operator=(const VoiceService&) = delete;
    ~VoiceService();

    void Shutdown();
    [[nodiscard]] device::DeviceResult<micropixel_voice_info_t> GetInfo() const { return voice_.GetInfo(); }
    [[nodiscard]] device::DeviceResult<void> StartSession() const { return voice_.StartSession(); }
    [[nodiscard]] device::DeviceResult<void> CancelSession() const { return voice_.CancelSession(); }
    [[nodiscard]] device::DeviceResult<micropixel_voice_state_response_t> GetState() const { return voice_.GetState(); }
    [[nodiscard]] device::DeviceResult<device::VoiceTextResult> ReadText(uint8_t kind, uint32_t revision,
                                                                         uint32_t offset,
                                                                         std::span<uint8_t> destination) const {
        return voice_.ReadText(kind, revision, offset, destination);
    }
    [[nodiscard]] device::DeviceResult<micropixel_voice_preferences_t> GetPreferences() const {
        return voice_.GetPreferences();
    }
    [[nodiscard]] device::DeviceResult<void> SetPreferences(
        const micropixel_voice_set_preferences_request_t& request) const {
        return voice_.SetPreferences(request);
    }
    [[nodiscard]] device::DeviceResult<micropixel_voice_diagnostics_t> GetDiagnostics() const {
        return voice_.GetDiagnostics();
    }
    [[nodiscard]] device::DeviceResult<void> RunDiagnostic(uint8_t kind) const { return voice_.RunDiagnostic(kind); }
    [[nodiscard]] device::DeviceResult<void> ClearContext() const { return voice_.ClearContext(); }

   private:
    static void OnUpdate(void* context, const device::VoiceUpdate& update);
    void Push(const device::VoiceUpdate& update);

    device::VoiceService& voice_;
    EventQueue& events_;
    int64_t clock_origin_us_{};
    uint32_t sequence_{};
    bool bound_{};
};

}  // namespace micropixel::runtime

#endif
