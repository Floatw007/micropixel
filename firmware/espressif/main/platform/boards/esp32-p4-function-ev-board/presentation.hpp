#pragma once

#include "host/ui/lvgl/square_common/square_presentation.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board::detail {

struct BoardState;

class FunctionEvPresentation final : public host_ui::lvgl::square_common::SquarePresentation,
                                     public host_ui::lvgl::square_common::ScreenCapture,
                                     public host_ui::lvgl::square_common::BrightnessControl {
   public:
    explicit FunctionEvPresentation(BoardState& state) : state_(state) {}

    [[nodiscard]] host_ui::lvgl::square_common::ScreenCapture* Capture() override { return this; }
    [[nodiscard]] host_ui::lvgl::square_common::BrightnessControl* Brightness() override { return this; }
    [[nodiscard]] std::expected<host_ui::ScreenCapture, host_ui::SystemUiError> CaptureScreenJpeg() override;
    void ApplyBrightness(uint8_t percent) override;

   private:
    BoardState& state_;
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board::detail
