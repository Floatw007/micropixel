#include "platform/platform.hpp"

#include <optional>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_pm.h"
#include "host/ui/lvgl/square_common/square_system_ui.hpp"
#include "platform/adapters/graphics_adapter.hpp"
#include "platform/boards/esp32-p4-function-ev-board/board_config.hpp"
#include "platform/boards/esp32-p4-function-ev-board/i2s_audio.hpp"
#include "platform/boards/esp32-p4-function-ev-board/idle_frequency_telemetry.hpp"
#include "platform/boards/esp32-p4-function-ev-board/platform_state.hpp"
#include "platform/boards/esp32-p4-function-ev-board/presentation.hpp"
#include "platform/controllers/brightness_curve.hpp"
#include "platform/lvgl/guest_graphics_operations.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"
#include "platform/memory/ext_ram_bss.hpp"
#include "platform/memory/internal_ram.hpp"
#include "platform/storage/sd_card_block_storage.hpp"
#include "platform/transports/development_display_control.hpp"
#include "platform/wifi/esp_hosted_radio.hpp"
#include "platform/wifi/wifi_manager.hpp"
#include "work/task_policy.hpp"

namespace micropixel::platform {
namespace {

namespace board_detail = esp32_p4_function_ev_board::detail;
namespace board = esp32_p4_function_ev_board::board;

esp_err_t EnableAutomaticLightSleep() {
    esp_pm_config_t power_config{};
    ESP_RETURN_ON_ERROR(esp_pm_get_configuration(&power_config), board_detail::kTag,
                        "read power-management configuration failed");
    power_config.light_sleep_enable = true;
    ESP_RETURN_ON_ERROR(esp_pm_configure(&power_config), board_detail::kTag, "enable automatic light sleep failed");
    ESP_LOGI(board_detail::kTag, "automatic light sleep configured: CPU=%d..%d MHz", power_config.min_freq_mhz,
             power_config.max_freq_mhz);
    return ESP_OK;
}

esp_err_t ProtectUsbLocalControlFromLightSleep(esp_pm_lock_handle_t& lock) {
    if (lock != nullptr) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "micropixel_usb", &lock), board_detail::kTag,
                        "create USB local-control light-sleep guard failed");
    const esp_err_t status = esp_pm_lock_acquire(lock);
    if (status != ESP_OK) {
        (void)esp_pm_lock_delete(lock);
        lock = nullptr;
        ESP_LOGE(board_detail::kTag, "acquire USB local-control light-sleep guard failed: %s", esp_err_to_name(status));
        return status;
    }
    ESP_LOGI(board_detail::kTag,
             "USB local control protected by persistent NO_LIGHT_SLEEP lock; display suspend and DFS remain enabled");
    return ESP_OK;
}

esp_err_t InitializeLvgl(board_detail::BoardState& state) {
    esp_lv_adapter_config_t adapter_config{};
    adapter_config.task_stack_size = ESP_LV_ADAPTER_DEFAULT_STACK_SIZE;
    adapter_config.stack_in_psram = false;
    adapter_config.task_priority = task_policy::kDisplayPriority;
    adapter_config.task_core_id = task_policy::kSystemCore;
    adapter_config.tick_mode = ESP_LV_ADAPTER_TICK_MODE_MONOTONIC;
    adapter_config.task_min_delay_ms = portTICK_PERIOD_MS;
    adapter_config.task_max_delay_ms = board_detail::kLvglMaximumWaitMs;
    adapter_config.auto_sleep.enable = true;
    adapter_config.auto_sleep.mode = ESP_LV_ADAPTER_AUTO_SLEEP_MODE_PAUSE;
    adapter_config.auto_sleep.idle_timeout_ms = board_detail::kLvglIdleTimeoutMs;
    ESP_RETURN_ON_ERROR(esp_lv_adapter_init(&adapter_config), board_detail::kTag, "initialize LVGL adapter failed");

    esp_lv_adapter_display_config_t display_config{};
    display_config.panel = state.display_pipeline.Panel();
    display_config.panel_io = state.display_pipeline.PanelIo();
    display_config.profile.interface = ESP_LV_ADAPTER_PANEL_IF_MIPI_DSI;
    display_config.profile.hor_res = board_detail::kWidth;
    display_config.profile.ver_res = board_detail::kHeight;
    display_config.profile.buffer_height = CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_HEIGHT;
#if CONFIG_MICROPIXEL_LVGL_PARTIAL_BUFFER_PSRAM
    display_config.profile.use_psram = true;
#else
    display_config.profile.use_psram = false;
#endif
    display_config.profile.enable_ppa_accel = true;
    display_config.profile.require_double_buffer = true;
    display_config.tear_avoid_mode = ESP_LV_ADAPTER_TEAR_AVOID_MODE_DOUBLE_DIRECT;
    state.display = esp_lv_adapter_register_display(&display_config);
    ESP_RETURN_ON_FALSE(state.display != nullptr, ESP_FAIL, board_detail::kTag, "register MIPI display failed");

    state.display_pipeline.BindLvgl(state.display);
    ESP_RETURN_ON_ERROR(state.guest_graphics.Initialize(state.display, state.display_pipeline.DirectFramebuffers(),
                                                        state.display_pipeline.DirectScanout()),
                        board_detail::kTag, "initialize Guest graphics failed");
    lv_timer_set_period(lv_display_get_refr_timer(state.display), board_detail::kRefreshPeriodMs);

    ESP_RETURN_ON_ERROR(esp_lv_adapter_lock(-1), board_detail::kTag, "lock LVGL failed");
    const esp_err_t ui_status = state.ui.InitializeLocked(state.display);
    esp_lv_adapter_unlock();
    ESP_RETURN_ON_ERROR(ui_status, board_detail::kTag, "initialize 1024x600 Host UI failed");
    ESP_RETURN_ON_ERROR(esp_lv_adapter_start(), board_detail::kTag, "start LVGL adapter failed");
    ESP_RETURN_ON_ERROR(state.touch_input.Start(state.display), board_detail::kTag, "start GT911 polling failed");
    lvgl::RequestDisplayRefresh(state.display);
    return ESP_OK;
}

class Esp32P4FunctionEvBoard final : public Board {
   public:
    Esp32P4FunctionEvBoard()
        : state_(TaskState(hardware_, input_state_)),
          graphics_context_{.engine = &state_.guest_graphics, .hooks = state_.ui.GraphicsHooks()},
          graphics_(lvgl::MakeGuestGraphicsOperations(graphics_context_)),
          presentation_(state_),
          system_ui_(state_.ui, presentation_) {
        state_.guest_graphics.SetPresentationHooks(state_.ui.GuestFrameHooks());
    }

    [[nodiscard]] esp_err_t Initialize(BoardContext& context) override {
        ESP_RETURN_ON_FALSE(memory::IsInternalObject(*this), ESP_ERR_INVALID_STATE, board_detail::kTag,
                            "Board control objects must reside in internal RAM");
        ESP_RETURN_ON_ERROR(EnableAutomaticLightSleep(), board_detail::kTag, "configure automatic light sleep failed");
        ESP_RETURN_ON_ERROR(ProtectUsbLocalControlFromLightSleep(usb_light_sleep_lock_), board_detail::kTag,
                            "protect USB local control from light sleep failed");
        ESP_LOGI(board_detail::kTag, "initializing ESP32-P4-Function-EV-Board display and touch");
        ESP_RETURN_ON_ERROR(hardware_.Initialize(), board_detail::kTag, "initialize display/touch hardware failed");
        if (const esp_err_t storage_status = sd_storage_.Initialize(storage::SdCardBlockStorage::Config{
                .slot = board::kSdMmcSlot,
                .width = board::kSdBusWidth,
                .clock = board::kSdClock,
                .command = board::kSdCommand,
                .data0 = board::kSdData0,
                .data1 = board::kSdData1,
                .data2 = board::kSdData2,
                .data3 = board::kSdData3,
                .power_ldo_channel = board::kSdLdoChannel,
                .max_frequency_khz = board::kSdMaximumFrequencyKhz,
            });
            storage_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "SD App storage is unavailable: %s", esp_err_to_name(storage_status));
        }
        ESP_RETURN_ON_ERROR(state_.i2c_executor.Initialize(), board_detail::kTag, "start I2C executor failed");
        const esp_err_t audio_config_status = audio_.Configure(hardware_.I2cBus(), state_.i2c_executor);
        if (audio_config_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "ES8311 audio unavailable for this boot: %s",
                     esp_err_to_name(audio_config_status));
        }
        ESP_RETURN_ON_ERROR(state_.touch_input.Initialize(hardware_.Touch(), state_.i2c_executor), board_detail::kTag,
                            "bind GT911 touch failed");
        ESP_RETURN_ON_ERROR(InitializeLvgl(state_), board_detail::kTag, "initialize display pipeline failed");
        ESP_RETURN_ON_ERROR(hardware_.SetBrightness(80U), board_detail::kTag, "set startup brightness failed");
        ESP_RETURN_ON_ERROR(state_.display_idle.Start(state_.display, state_.touch_input), board_detail::kTag,
                            "start controlled MIPI-DPI suspend failed");
        ESP_RETURN_ON_ERROR(state_.development_display.Start(state_.touch_input, state_.local_control,
                                                             board_detail::kWidth, board_detail::kHeight,
                                                             transports::DevelopmentCaptureHook::For(presentation_)),
                            board_detail::kTag, "start USB Serial/JTAG local control failed");
#if CONFIG_MICROPIXEL_FUNCTION_EV_IDLE_FREQUENCY_TELEMETRY
        if (const esp_err_t telemetry_status = idle_frequency_telemetry_.Start(); telemetry_status != ESP_OK) {
            ESP_LOGW(board_detail::kTag, "idle-frequency telemetry unavailable: %s", esp_err_to_name(telemetry_status));
        }
#endif

        BoardRegistration& registration = registration_.emplace(device::BoardInfo{
            .board = "ESP32-P4-Function-EV-Board",
            .host_chip = "ESP32-P4",
            .firmware_target = "esp32-p4-function-ev",
            .wifi_coprocessor = "ESP32-C6 (ESP-Hosted SDIO)",
            .touch_controller = "GT911 (10/50/100 ms adaptive polling)",
            .display =
                {
                    .driver = "EK79007",
                    .interface = "MIPI-DSI / 2 lanes",
                    .pixel_format = "RGB888",
                    .width_pixels = static_cast<uint32_t>(board_detail::kWidth),
                    .height_pixels = static_cast<uint32_t>(board_detail::kHeight),
                    .refresh_rate_hz = 60U,
                },
            .graphics_acceleration = "PPA + DMA2D",
        });
        registration.SetGraphics(graphics_);
        registration.SetInput(state_.ui.Input());
        if (audio_config_status == ESP_OK) {
            registration.SetAudioOutput(audio_, audio_.SampleRate());
            registration.SetAudioInput(audio_);
        }
        registration.SetWifi(wifi_);
        registration.SetLocalControl(state_.local_control);
        if (sd_storage_.present()) {
            registration.SetAppStorage(sd_storage_, 0U, true);
        }
        registration.SetSystemUi(system_ui_);
        ESP_LOGI(board_detail::kTag,
                 "ready: EK79007 1024x600 RGB888 + GT911 touch + ES8311/NS4150 audio=%s + ESP32-C6 Wi-Fi + USB "
                 "local control + SD=%s",
                 audio_config_status == ESP_OK ? "ready" : "unavailable", sd_storage_.present() ? "ready" : "absent");
        return context.Publish(registration) ? ESP_OK : ESP_ERR_INVALID_STATE;
    }

    void BindBackgroundExecutor(work::BackgroundExecutor& executor) override {
        state_.ui.BindBackgroundExecutor(executor);
        state_.guest_graphics.BindBackgroundExecutor(executor);
        wifi_.BindBackgroundExecutor(executor);
    }

   private:
    static board_detail::BoardState& TaskState(esp32_p4_function_ev_board::BoardHardware& hardware,
                                               board_detail::InputState& input) {
        static MICROPIXEL_EXT_RAM_BSS board_detail::BoardState state(hardware, input);
        return state;
    }

    board_detail::InputState input_state_{};
    esp32_p4_function_ev_board::BoardHardware hardware_{};
    board_detail::BoardState& state_;
    std::optional<BoardRegistration> registration_{};
    lvgl::GuestGraphicsOperationsContext graphics_context_{};
    adapters::GraphicsAdapter graphics_;
    wifi::EspHostedRadio wifi_radio_{"slave_fw", true};
    wifi::WifiManager wifi_{wifi_radio_};
    storage::SdCardBlockStorage sd_storage_{};
    esp32_p4_function_ev_board::I2sAudio audio_{};
    esp_pm_lock_handle_t usb_light_sleep_lock_{};
    esp32_p4_function_ev_board::IdleFrequencyTelemetry idle_frequency_telemetry_{};
    board_detail::FunctionEvPresentation presentation_;
    host_ui::lvgl::square_common::SquareSystemUi system_ui_;
};

}  // namespace

Board& ConfiguredBoard() {
    static Esp32P4FunctionEvBoard board;
    return board;
}

}  // namespace micropixel::platform
