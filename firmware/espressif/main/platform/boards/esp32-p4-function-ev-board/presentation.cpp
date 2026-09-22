#include "platform/boards/esp32-p4-function-ev-board/presentation.hpp"

#include <algorithm>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "platform/boards/esp32-p4-function-ev-board/platform_state.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/lvgl/display/screen_capture.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board::detail {

std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> FunctionEvPresentation::CaptureScreenJpeg() {
    if (!state_.display_idle.EnsureAwake()) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    // Select the displayed member of the double-DPI framebuffer pair while
    // LVGL cannot swap buffers. CaptureScreenJpeg() takes the same recursive
    // adapter lock while copying the frame, then encodes after releasing it.
    if (esp_lv_adapter_lock(-1) != ESP_OK) {
        return std::unexpected(host_ui::SystemUiError::kUnavailable);
    }
    auto* framebuffers = state_.display_pipeline.DirectFramebuffers();
    const uint8_t* displayed = framebuffers != nullptr ? framebuffers->Displayed() : nullptr;
    static constexpr bool kReady = true;
    auto result = lvgl::CaptureScreenJpeg(state_.display, static_cast<uint32_t>(kWidth), static_cast<uint32_t>(kHeight),
                                          {.pixels = displayed,
                                           .stride = static_cast<uint32_t>(kWidth) * 3U,
                                           .format = lvgl::DisplayCapturePixelFormat::kRgb888,
                                           .ready = displayed != nullptr ? &kReady : nullptr});
    esp_lv_adapter_unlock();
    return result;
}

void FunctionEvPresentation::ApplyBrightness(uint8_t percent) {
    const uint8_t safe_percent = std::min<uint8_t>(percent, 100U);
    const uint32_t output = controllers::BrightnessOutputPerTenThousand(safe_percent);
    const esp_err_t status = state_.display_pipeline.SetBrightness(output);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "backlight brightness update failed: %s", esp_err_to_name(status));
    }
}

}  // namespace micropixel::platform::esp32_p4_function_ev_board::detail
