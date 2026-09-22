#include "platform/boards/esp32-p4-function-ev-board/board_hardware.hpp"

#include <algorithm>

#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_lcd_ek79007.h"
#include "esp_lcd_touch_gt911.h"
#include "platform/boards/esp32-p4-function-ev-board/board_config.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {
namespace {

constexpr char kTag[] = "p4_func_hw";
constexpr uint32_t kBacklightMaximumDuty = (1U << 10U) - 1U;

}  // namespace

esp_err_t BoardHardware::Initialize() {
    ESP_RETURN_ON_ERROR(InitializeBacklight(), kTag, "initialize backlight PWM failed");
    ESP_RETURN_ON_ERROR(InitializeI2cAndTouch(), kTag, "initialize GT911 failed");
    return InitializeDisplay();
}

esp_err_t BoardHardware::InitializeBacklight() {
    ledc_timer_config_t timer_config{};
    timer_config.speed_mode = board::kBacklightLedcMode;
    timer_config.duty_resolution = LEDC_TIMER_10_BIT;
    timer_config.timer_num = board::kBacklightLedcTimer;
    timer_config.freq_hz = 5000U;
    timer_config.clk_cfg = LEDC_AUTO_CLK;
    ESP_RETURN_ON_ERROR(ledc_timer_config(&timer_config), kTag, "configure LEDC timer failed");

    ledc_channel_config_t channel_config{};
    channel_config.gpio_num = board::kDisplayBacklight;
    channel_config.speed_mode = board::kBacklightLedcMode;
    channel_config.channel = board::kBacklightLedcChannel;
    channel_config.timer_sel = board::kBacklightLedcTimer;
    channel_config.duty = 0U;
    channel_config.hpoint = 0;
    ESP_RETURN_ON_ERROR(ledc_channel_config(&channel_config), kTag, "configure LEDC channel failed");
    backlight_ready_ = true;
    return ESP_OK;
}

esp_err_t BoardHardware::InitializeI2cAndTouch() {
    i2c_master_bus_config_t bus_config{};
    bus_config.i2c_port = board::kI2cPort;
    bus_config.sda_io_num = board::kI2cData;
    bus_config.scl_io_num = board::kI2cClock;
    bus_config.clk_source = I2C_CLK_SRC_DEFAULT;
    bus_config.glitch_ignore_cnt = 7U;
    bus_config.flags.enable_internal_pullup = true;
    ESP_RETURN_ON_ERROR(i2c_new_master_bus(&bus_config, &i2c_bus_), kTag, "create shared I2C bus failed");

    // Spell out every field instead of using ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG().
    // The component macro predates fields added in ESP-IDF 6.1, while MicroPixel
    // deliberately treats missing aggregate initializers as errors.
    esp_lcd_panel_io_i2c_config_t touch_io_config{};
    touch_io_config.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    touch_io_config.scl_speed_hz = 100000U;
    touch_io_config.control_phase_bytes = 1U;
    touch_io_config.dc_bit_offset = 0U;
    touch_io_config.lcd_cmd_bits = 16;
    touch_io_config.lcd_param_bits = 0;
    touch_io_config.on_color_trans_done = nullptr;
    touch_io_config.user_ctx = nullptr;
    touch_io_config.flags.dc_low_on_data = 0U;
    touch_io_config.flags.disable_control_phase = 1U;
    touch_io_config.transaction_timeout_ms = 0;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(i2c_bus_, &touch_io_config, &touch_io_), kTag,
                        "create GT911 panel IO failed");
    esp_lcd_touch_config_t touch_config{};
    touch_config.x_max = board::kDisplayWidth;
    touch_config.y_max = board::kDisplayHeight;
    touch_config.rst_gpio_num = board::kTouchReset;
    touch_config.int_gpio_num = board::kTouchInterrupt;
    touch_config.levels.reset = 0;
    touch_config.levels.interrupt = 0;
    touch_config.flags.swap_xy = false;
    touch_config.flags.mirror_x = true;
    touch_config.flags.mirror_y = true;
    return esp_lcd_touch_new_i2c_gt911(touch_io_, &touch_config, &touch_);
}

esp_err_t BoardHardware::InitializeDisplay() {
    if (panel_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (dsi_ldo_ == nullptr) {
        esp_ldo_channel_config_t ldo_config{};
        ldo_config.chan_id = board::kDsiLdoChannel;
        ldo_config.voltage_mv = board::kDsiLdoMillivolts;
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &dsi_ldo_), kTag, "enable MIPI DSI PHY power failed");
    }

    bool created_bus = false;
    if (dsi_bus_ == nullptr) {
        esp_lcd_dsi_bus_config_t bus_config{};
        bus_config.bus_id = 0;
        bus_config.num_data_lanes = board::kDsiLaneCount;
        bus_config.phy_clk_src = MIPI_DSI_PHY_CLK_SRC_DEFAULT;
        bus_config.lane_bit_rate_mbps = board::kDsiLaneBitRateMbps;
        ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &dsi_bus_), kTag, "create MIPI DSI bus failed");
        created_bus = true;
    }

    bool created_panel_io = false;
    if (panel_io_ == nullptr) {
        esp_lcd_dbi_io_config_t dbi_config{};
        dbi_config.virtual_channel = 0;
        dbi_config.lcd_cmd_bits = 8;
        dbi_config.lcd_param_bits = 8;
        const esp_err_t io_status = esp_lcd_new_panel_io_dbi(dsi_bus_, &dbi_config, &panel_io_);
        if (io_status != ESP_OK) {
            if (created_bus) {
                (void)esp_lcd_del_dsi_bus(dsi_bus_);
                dsi_bus_ = nullptr;
            }
            return io_status;
        }
        created_panel_io = true;
    }

    // Keep this timing in sync with EK79007_1024_600_PANEL_60HZ_CONFIG_CF.
    // An explicit initialization also covers the ESP-IDF 6.1 output-format and
    // flags fields that the current panel component macro leaves unspecified.
    esp_lcd_dpi_panel_config_t dpi_config{};
    dpi_config.virtual_channel = 0U;
    dpi_config.dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT;
    dpi_config.dpi_clock_freq_mhz = 52.0F;
    dpi_config.in_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.out_color_format = LCD_COLOR_FMT_RGB888;
    dpi_config.num_fbs = 2U;
    dpi_config.video_timing.h_size = board::kDisplayWidth;
    dpi_config.video_timing.v_size = board::kDisplayHeight;
    dpi_config.video_timing.hsync_pulse_width = 10U;
    dpi_config.video_timing.hsync_back_porch = 160U;
    dpi_config.video_timing.hsync_front_porch = 160U;
    dpi_config.video_timing.vsync_pulse_width = 1U;
    dpi_config.video_timing.vsync_back_porch = 23U;
    dpi_config.video_timing.vsync_front_porch = 12U;
    dpi_config.flags.disable_lp = 0U;
    ek79007_vendor_config_t vendor_config{};
    vendor_config.mipi_config.dsi_bus = dsi_bus_;
    vendor_config.mipi_config.dpi_config = &dpi_config;
    esp_lcd_panel_dev_config_t panel_config{};
    panel_config.bits_per_pixel = 16;
    panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
    panel_config.reset_gpio_num = board::kDisplayReset;
    panel_config.vendor_config = &vendor_config;
    esp_err_t status = esp_lcd_new_panel_ek79007(panel_io_, &panel_config, &panel_);
    if (status != ESP_OK) {
        if (created_panel_io) {
            (void)esp_lcd_panel_io_del(panel_io_);
            panel_io_ = nullptr;
        }
        if (created_bus) {
            (void)esp_lcd_del_dsi_bus(dsi_bus_);
            dsi_bus_ = nullptr;
        }
        return status;
    }
    status = esp_lcd_dpi_panel_enable_dma2d(panel_);
    if (status == ESP_OK) {
        panel_dma2d_enabled_ = true;
        status = esp_lcd_panel_reset(panel_);
    }
    if (status == ESP_OK) {
        status = esp_lcd_panel_init(panel_);
    }
    if (status != ESP_OK) {
        (void)SuspendDisplay();
    }
    return status;
}

esp_err_t BoardHardware::SetBrightness(uint8_t percent) {
    brightness_percent_ = static_cast<uint8_t>(std::min<uint32_t>(percent, 100U));
    return ApplyBrightness(brightness_percent_);
}

esp_err_t BoardHardware::ApplyBrightness(uint8_t percent) {
    if (!backlight_ready_) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint32_t bounded = std::min<uint32_t>(percent, 100U);
    const uint32_t duty = (kBacklightMaximumDuty * bounded + 50U) / 100U;
    ESP_RETURN_ON_ERROR(ledc_set_duty(board::kBacklightLedcMode, board::kBacklightLedcChannel, duty), kTag,
                        "set backlight duty failed");
    return ledc_update_duty(board::kBacklightLedcMode, board::kBacklightLedcChannel);
}

esp_err_t BoardHardware::SetDisplayEnabled(bool enabled) {
    ESP_RETURN_ON_FALSE(panel_ != nullptr, ESP_ERR_INVALID_STATE, kTag, "display is not initialized");
    return enabled ? RestoreBrightness() : ApplyBrightness(0U);
}

esp_err_t BoardHardware::SuspendDisplay() {
    esp_err_t status = ApplyBrightness(0U);
    if (panel_ != nullptr) {
        if (panel_dma2d_enabled_) {
            const esp_err_t dma2d_status = esp_lcd_dpi_panel_disable_dma2d(panel_);
            if (dma2d_status == ESP_OK) {
                panel_dma2d_enabled_ = false;
            }
            if (status == ESP_OK) {
                status = dma2d_status;
            }
        }
        if (!panel_dma2d_enabled_) {
            const esp_err_t delete_status = esp_lcd_panel_del(panel_);
            if (delete_status == ESP_OK) {
                panel_ = nullptr;
            }
            if (status == ESP_OK) {
                status = delete_status;
            }
        }
    }
    if (panel_ == nullptr && panel_io_ != nullptr) {
        const esp_err_t delete_status = esp_lcd_panel_io_del(panel_io_);
        if (delete_status == ESP_OK) {
            panel_io_ = nullptr;
        }
        if (status == ESP_OK) {
            status = delete_status;
        }
    }
    if (panel_ == nullptr && panel_io_ == nullptr && dsi_bus_ != nullptr) {
        const esp_err_t delete_status = esp_lcd_del_dsi_bus(dsi_bus_);
        if (delete_status == ESP_OK) {
            dsi_bus_ = nullptr;
        }
        if (status == ESP_OK) {
            status = delete_status;
        }
    }
    return status;
}

esp_err_t BoardHardware::ResumeDisplay() {
    ESP_RETURN_ON_ERROR(ApplyBrightness(0U), kTag, "hold backlight off during display resume failed");
    if (panel_ == nullptr) {
        return InitializeDisplay();
    }
    if (!panel_dma2d_enabled_) {
        ESP_RETURN_ON_ERROR(esp_lcd_dpi_panel_enable_dma2d(panel_), kTag, "re-enable DPI DMA2D failed");
        panel_dma2d_enabled_ = true;
    }
    return ESP_OK;
}

esp_err_t BoardHardware::RestoreBrightness() { return ApplyBrightness(brightness_percent_); }

}  // namespace micropixel::platform::esp32_p4_function_ev_board
