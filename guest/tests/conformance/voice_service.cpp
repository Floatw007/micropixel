#include "abi/micropixel_abi.h"
#include "sdk/micropixel.hpp"

int main() {
    micropixel_service_info_t service{};
    if (micropixel_service_open(
            MICROPIXEL_SERVICE_VOICE,
            MICROPIXEL_INTERFACE_VERSION(MICROPIXEL_VOICE_INTERFACE_MAJOR, MICROPIXEL_VOICE_INTERFACE_MINOR), &service,
            sizeof(service)) != MICROPIXEL_STATUS_OK ||
        service.interface_minor < MICROPIXEL_VOICE_INTERFACE_MINOR ||
        (service.flags & (MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS)) !=
            (MICROPIXEL_SERVICE_FLAG_CALL | MICROPIXEL_SERVICE_FLAG_EVENTS)) {
        return 150;
    }

    uint8_t invalid = 0U;
    uint32_t response_size = 0U;
    micropixel_voice_preferences_t preferences{};
    if (micropixel_service_call(service.service_handle, MICROPIXEL_VOICE_METHOD_GET_PREFERENCES, &invalid, 1U,
                                reinterpret_cast<uint8_t*>(&preferences), sizeof(preferences),
                                &response_size) != MICROPIXEL_STATUS_INVALID_ARGUMENT) {
        return 151;
    }

    micropixel::Application app;
    auto info = app.voice().info();
    auto current = app.voice().preferences();
    auto diagnostics = app.voice().diagnostics();
    if (!info.has_value() || !current.has_value() || !diagnostics.has_value() || info.value().sample_rate != 16000U ||
        info.value().channels != 1U || current.value().response_gain_per_mille > 1000U ||
        diagnostics.value().input_level_per_mille > 1000U) {
        return 152;
    }

    micropixel::VoicePreferenceUpdate unchanged{};
    unchanged.set_response_gain = true;
    unchanged.set_local_commands = true;
    unchanged.response_gain_per_mille = current.value().response_gain_per_mille;
    unchanged.local_commands_enabled = current.value().local_commands_enabled;
    if (!app.voice().SetPreferences(unchanged).has_value()) return 153;

    app.log().Info("voice_service: Voice 1.1 preferences and redacted diagnostics ready");
    return 0;
}
