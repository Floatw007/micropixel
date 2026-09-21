#include "platform/boards/esp32-p4-function-ev-board/display/display_pipeline.hpp"

#include "esp_lcd_mipi_dsi.h"
#include "esp_lv_adapter.h"
#include "platform/controllers/brightness_curve.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {

void FunctionEvDisplayPipeline::DpiFramebuffers::Bind(lv_display_t* display, esp_lcd_panel_handle_t panel) {
    display_ = display;
    panel_ = panel;
    buffers_ = {};
    (void)Resolve();
}

bool FunctionEvDisplayPipeline::DpiFramebuffers::Resolve() {
    if (display_ == nullptr || panel_ == nullptr) {
        buffers_ = {};
        return false;
    }
    void* first = nullptr;
    void* second = nullptr;
    if (esp_lcd_dpi_panel_get_frame_buffer(panel_, 2U, &first, &second) != ESP_OK) {
        buffers_ = {};
        return false;
    }
    buffers_[0] = static_cast<uint8_t*>(first);
    buffers_[1] = static_cast<uint8_t*>(second);
    return buffers_[0] != nullptr && buffers_[1] != nullptr;
}

bool FunctionEvDisplayPipeline::DpiFramebuffers::Ready() const {
    return display_ != nullptr && panel_ != nullptr && buffers_[0] != nullptr && buffers_[1] != nullptr;
}

uint8_t* FunctionEvDisplayPipeline::DpiFramebuffers::AcquireFree() {
    if (!Ready() && !Resolve()) {
        return nullptr;
    }
    return static_cast<uint8_t*>(esp_lv_adapter_dummy_draw_get_free_buf_preserve(display_));
}

uint8_t* FunctionEvDisplayPipeline::DpiFramebuffers::Displayed() {
    uint8_t* free = AcquireFree();
    if (free == buffers_[0]) {
        return buffers_[1];
    }
    return free == buffers_[1] ? buffers_[0] : nullptr;
}

bool FunctionEvDisplayPipeline::DpiFramebuffers::Contains(const uint8_t* buffer) const {
    return buffer != nullptr && (buffer == buffers_[0] || buffer == buffers_[1]);
}

esp_err_t FunctionEvDisplayPipeline::DpiFramebuffers::Submit(uint8_t* buffer) {
    return Ready() && Contains(buffer) ? esp_lv_adapter_dummy_draw_flush_buf(display_, buffer) : ESP_ERR_INVALID_ARG;
}

void FunctionEvDisplayPipeline::BindLvgl(lv_display_t* display) {
    framebuffers_.Bind(display, hardware_.Panel());
}

lvgl::DisplayCapabilities FunctionEvDisplayPipeline::Capabilities() const {
    return {.partial_flush = true,
            .tearing_effect_sync = true,
            .direct_framebuffers = true,
            .ppa = true,
            .dma2d = true,
            .hardware_jpeg = true};
}

esp_err_t FunctionEvDisplayPipeline::Suspend() {
    framebuffers_.Bind(nullptr, nullptr);
    return hardware_.SetDisplayEnabled(false);
}

esp_err_t FunctionEvDisplayPipeline::Resume() { return hardware_.SetDisplayEnabled(true); }

esp_err_t FunctionEvDisplayPipeline::SetBrightness(uint32_t per_ten_thousand) {
    const uint32_t bounded =
        per_ten_thousand <= controllers::kBrightnessControlScale ? per_ten_thousand
                                                                 : controllers::kBrightnessControlScale;
    const uint8_t percent = static_cast<uint8_t>((bounded * 100U + controllers::kBrightnessControlScale / 2U) /
                                                 controllers::kBrightnessControlScale);
    return hardware_.SetBrightness(percent);
}

}  // namespace micropixel::platform::esp32_p4_function_ev_board
