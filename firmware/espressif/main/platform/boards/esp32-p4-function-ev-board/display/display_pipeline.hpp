#pragma once

#include <array>
#include <cstdint>

#include "lvgl.h"
#include "platform/boards/esp32-p4-function-ev-board/board_hardware.hpp"
#include "platform/lvgl/display/display_pipeline.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {

class FunctionEvDisplayPipeline final : public lvgl::DisplayPipeline {
   public:
    FunctionEvDisplayPipeline(BoardHardware& hardware, uint32_t width, uint32_t height)
        : hardware_(hardware), geometry_{width, height, 3U} {}

    void BindLvgl(lv_display_t* display);

    [[nodiscard]] lvgl::DisplayGeometry Geometry() const override { return geometry_; }
    [[nodiscard]] lvgl::DisplayCapabilities Capabilities() const override;
    [[nodiscard]] lvgl::DirectFramebufferAccess* DirectFramebuffers() override { return &framebuffers_; }
    [[nodiscard]] lvgl::DirectScanoutProfile DirectScanout() const override {
        return {.mode = lvgl::DirectScanoutProfile::Mode::kFramebufferRgb888,
                .rgb565_byte_swapped = false,
                .max_full_frame_fps = 60U};
    }
    [[nodiscard]] esp_err_t Suspend() override;
    [[nodiscard]] esp_err_t Resume() override;
    [[nodiscard]] esp_err_t SetBrightness(uint32_t per_ten_thousand) override;

    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return hardware_.Panel(); }
    [[nodiscard]] esp_lcd_panel_io_handle_t PanelIo() const { return hardware_.PanelIo(); }

   private:
    class DpiFramebuffers final : public lvgl::DirectFramebufferAccess {
       public:
        void Bind(lv_display_t* display, esp_lcd_panel_handle_t panel);
        [[nodiscard]] bool Ready() const override;
        [[nodiscard]] uint32_t Count() const override { return buffers_.size(); }
        [[nodiscard]] uint8_t* AcquireFree() override;
        [[nodiscard]] uint8_t* Displayed() override;
        [[nodiscard]] bool Contains(const uint8_t* buffer) const override;
        [[nodiscard]] esp_err_t Submit(uint8_t* buffer) override;

       private:
        [[nodiscard]] bool Resolve();

        lv_display_t* display_{};
        esp_lcd_panel_handle_t panel_{};
        std::array<uint8_t*, 2U> buffers_{};
    };

    BoardHardware& hardware_;
    lvgl::DisplayGeometry geometry_{};
    DpiFramebuffers framebuffers_{};
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board
