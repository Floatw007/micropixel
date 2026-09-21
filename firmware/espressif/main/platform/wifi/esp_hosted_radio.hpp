#pragma once

#include "platform/wifi/wifi_radio.hpp"

namespace micropixel::platform::wifi {

class EspHostedRadio final : public WifiRadio {
   public:
    explicit EspHostedRadio(const char* staged_firmware_partition = nullptr)
        : staged_firmware_partition_(staged_firmware_partition) {}

    [[nodiscard]] esp_err_t Initialize() override;
    [[nodiscard]] esp_err_t OnStationStarted() override;
    [[nodiscard]] const char* Name() const override { return "ESP-Hosted"; }
    [[nodiscard]] bool HasSlowStartup() const override { return true; }

   private:
    const char* staged_firmware_partition_{};
};

}  // namespace micropixel::platform::wifi
