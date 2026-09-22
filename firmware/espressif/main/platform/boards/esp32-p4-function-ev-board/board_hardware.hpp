#pragma once

#include <cstdint>

#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "esp_ldo_regulator.h"

namespace micropixel::platform::esp32_p4_function_ev_board {

class BoardHardware final {
   public:
    [[nodiscard]] esp_err_t Initialize();
    [[nodiscard]] esp_err_t SetBrightness(uint8_t percent);
    [[nodiscard]] esp_err_t SetDisplayEnabled(bool enabled);
    [[nodiscard]] esp_err_t SuspendDisplay();
    [[nodiscard]] esp_err_t ResumeDisplay();
    [[nodiscard]] esp_err_t RestoreBrightness();

    [[nodiscard]] i2c_master_bus_handle_t I2cBus() const { return i2c_bus_; }
    [[nodiscard]] esp_lcd_panel_handle_t Panel() const { return panel_; }
    [[nodiscard]] esp_lcd_panel_io_handle_t PanelIo() const { return panel_io_; }
    [[nodiscard]] esp_lcd_touch_handle_t Touch() const { return touch_; }

   private:
    [[nodiscard]] esp_err_t InitializeBacklight();
    [[nodiscard]] esp_err_t InitializeI2cAndTouch();
    [[nodiscard]] esp_err_t InitializeDisplay();
    [[nodiscard]] esp_err_t ApplyBrightness(uint8_t percent);

    i2c_master_bus_handle_t i2c_bus_{};
    esp_lcd_panel_io_handle_t touch_io_{};
    esp_lcd_touch_handle_t touch_{};
    esp_lcd_dsi_bus_handle_t dsi_bus_{};
    esp_lcd_panel_io_handle_t panel_io_{};
    esp_lcd_panel_handle_t panel_{};
    esp_ldo_channel_handle_t dsi_ldo_{};
    bool backlight_ready_{};
    bool panel_dma2d_enabled_{};
    uint8_t brightness_percent_{};
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board
