#include "platform/boards/esp32-p4-function-ev-board/display_idle_controller.hpp"

#include <algorithm>
#include <cinttypes>

#include "esp_log.h"
#include "esp_lv_adapter.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "work/task_policy.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {
namespace {

constexpr char kTag[] = "function_ev_display";
constexpr uint32_t kTaskStackBytes = 4096U;
constexpr uint32_t kResumeRetryDelayMs = 1000U;
constexpr int64_t kIdleTimeoutUs = static_cast<int64_t>(CONFIG_MICROPIXEL_FUNCTION_EV_DISPLAY_IDLE_TIMEOUT_MS) * 1000;

}  // namespace

esp_err_t FunctionEvDisplayIdleController::Start(lv_display_t* display, input::EspLcdTouchInput& touch_input) {
    if (display == nullptr || task_ != nullptr) {
        return ESP_ERR_INVALID_ARG;
    }
    display_ = display;
    last_activity_us_.store(esp_timer_get_time(), std::memory_order_release);
    const BaseType_t created = xTaskCreatePinnedToCore(TaskEntry, "display_idle", kTaskStackBytes, this,
                                                       tskIDLE_PRIORITY + 1U, &task_, task_policy::kSystemCore);
    if (created != pdPASS) {
        task_ = nullptr;
        display_ = nullptr;
        return ESP_ERR_NO_MEM;
    }
    if (!lvgl::BindDisplayActivityHook(ActivityHook, this)) {
        vTaskDelete(task_);
        task_ = nullptr;
        display_ = nullptr;
        return ESP_ERR_INVALID_STATE;
    }
    touch_input.BindDispatchGate(TouchGate, this);
    ESP_LOGI(kTag, "controlled MIPI-DPI suspend armed after %d ms of foreground inactivity",
             CONFIG_MICROPIXEL_FUNCTION_EV_DISPLAY_IDLE_TIMEOUT_MS);
    return ESP_OK;
}

void FunctionEvDisplayIdleController::RecordForegroundActivity() {
    last_activity_us_.store(esp_timer_get_time(), std::memory_order_release);
    if (state_.load(std::memory_order_acquire) != State::kActive) {
        wake_requested_.store(true, std::memory_order_release);
    }
    if (task_ != nullptr) {
        xTaskNotifyGive(task_);
    }
}

bool FunctionEvDisplayIdleController::EnsureAwake(TickType_t timeout_ticks) {
    RecordForegroundActivity();
    const TickType_t started = xTaskGetTickCount();
    while (state_.load(std::memory_order_acquire) != State::kActive) {
        if (xTaskGetTickCount() - started >= timeout_ticks) {
            return false;
        }
        vTaskDelay(1U);
    }
    return true;
}

bool FunctionEvDisplayIdleController::ActivityHook(void* context, lv_display_t* display,
                                                   lvgl::DisplayActivityIntent intent) {
    auto* controller = static_cast<FunctionEvDisplayIdleController*>(context);
    if (controller == nullptr || display != controller->display_) {
        return true;
    }
    if (intent == lvgl::DisplayActivityIntent::kPassive) {
        return controller->state_.load(std::memory_order_acquire) == State::kActive;
    }
    controller->RecordForegroundActivity();
    return controller->state_.load(std::memory_order_acquire) == State::kActive;
}

bool FunctionEvDisplayIdleController::TouchGate(void* context, const device::TouchSample&) {
    auto* controller = static_cast<FunctionEvDisplayIdleController*>(context);
    return controller != nullptr && controller->EnsureAwake();
}

bool FunctionEvDisplayIdleController::IdleDeadlineReached() const {
    return esp_timer_get_time() - last_activity_us_.load(std::memory_order_acquire) >= kIdleTimeoutUs;
}

bool FunctionEvDisplayIdleController::RecoverPreparedDisplay(bool rebuild_panel) {
    if (rebuild_panel) {
        const esp_err_t panel_status = pipeline_.Resume();
        if (panel_status != ESP_OK) {
            ESP_LOGE(kTag, "MIPI-DPI panel recreation failed: %s", esp_err_to_name(panel_status));
            return false;
        }
        guest_graphics_.RebindFramebuffers(pipeline_.DirectFramebuffers());
    }
    const int64_t resume_started_us = esp_timer_get_time();
    const esp_err_t recover_status = esp_lv_adapter_sleep_recover(display_, pipeline_.Panel(), pipeline_.PanelIo());
    if (recover_status != ESP_OK) {
        ESP_LOGE(kTag, "LVGL panel rebind failed: %s", esp_err_to_name(recover_status));
        return false;
    }
    guest_graphics_.ResumeDirectSurface();
    last_activity_us_.store(esp_timer_get_time(), std::memory_order_release);
    wake_requested_.store(false, std::memory_order_release);
    const esp_err_t refresh_status = esp_lv_adapter_refresh_now(display_);
    if (refresh_status != ESP_OK) {
        ESP_LOGE(kTag, "first refresh after display resume failed: %s", esp_err_to_name(refresh_status));
    }
    const esp_err_t brightness_status = pipeline_.RestoreBrightness();
    if (brightness_status != ESP_OK) {
        ESP_LOGE(kTag, "backlight restore failed: %s", esp_err_to_name(brightness_status));
    }
    // Publish Active only after the first synchronous refresh has reached the
    // new panel and the backlight has been restored. Touch and screenshot
    // callers wait on this boundary, so neither can observe the cleared spare
    // framebuffer or deliver input to a still-dark display.
    state_.store(State::kActive, std::memory_order_release);
    ESP_LOGI(kTag, "MIPI-DPI display resumed in %" PRIi64 " ms", (esp_timer_get_time() - resume_started_us) / 1000);
    return true;
}

void FunctionEvDisplayIdleController::AttemptSuspend() {
    // Drain an exclusive Guest scanout before LVGL detaches its panel. If that
    // handoff itself publishes a frame, count it as activity and wait through
    // one more idle interval before deleting the newly synchronized panel.
    guest_graphics_.SuspendDirectSurface();
    if (!IdleDeadlineReached()) {
        guest_graphics_.ResumeDirectSurface();
        return;
    }

    wake_requested_.store(false, std::memory_order_release);
    state_.store(State::kSuspending, std::memory_order_release);
    if (!IdleDeadlineReached()) {
        state_.store(State::kActive, std::memory_order_release);
        guest_graphics_.ResumeDirectSurface();
        return;
    }

    const esp_err_t prepare_status = esp_lv_adapter_sleep_prepare();
    if (prepare_status != ESP_OK) {
        ESP_LOGW(kTag, "LVGL display detach skipped: %s", esp_err_to_name(prepare_status));
        state_.store(State::kActive, std::memory_order_release);
        last_activity_us_.store(esp_timer_get_time(), std::memory_order_release);
        guest_graphics_.ResumeDirectSurface();
        return;
    }
    if (wake_requested_.load(std::memory_order_acquire)) {
        if (!RecoverPreparedDisplay(false)) {
            state_.store(State::kSuspended, std::memory_order_release);
        }
        return;
    }

    const int64_t suspend_started_us = esp_timer_get_time();
    const esp_err_t suspend_status = pipeline_.Suspend();
    if (suspend_status != ESP_OK) {
        ESP_LOGE(kTag, "MIPI-DPI shutdown failed: %s", esp_err_to_name(suspend_status));
        if (!RecoverPreparedDisplay(true)) {
            state_.store(State::kSuspended, std::memory_order_release);
            wake_requested_.store(true, std::memory_order_release);
        }
        return;
    }
    state_.store(State::kSuspended, std::memory_order_release);
    ESP_LOGI(kTag, "MIPI-DPI panel detached in %" PRIi64 " ms; dsi_dpi frequency lock released",
             (esp_timer_get_time() - suspend_started_us) / 1000);
    if (wake_requested_.load(std::memory_order_acquire)) {
        AttemptResume();
    }
}

void FunctionEvDisplayIdleController::AttemptResume() {
    state_.store(State::kResuming, std::memory_order_release);
    if (!RecoverPreparedDisplay(true)) {
        (void)pipeline_.Suspend();
        state_.store(State::kSuspended, std::memory_order_release);
        vTaskDelay(pdMS_TO_TICKS(kResumeRetryDelayMs));
    }
}

void FunctionEvDisplayIdleController::Run() {
    for (;;) {
        const State state = state_.load(std::memory_order_acquire);
        if (state == State::kActive) {
            const int64_t remaining_us =
                kIdleTimeoutUs - (esp_timer_get_time() - last_activity_us_.load(std::memory_order_acquire));
            if (remaining_us > 0) {
                const uint64_t wait_ms = (static_cast<uint64_t>(remaining_us) + 999U) / 1000U;
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(std::max<uint64_t>(wait_ms, 1U)));
                continue;
            }
            AttemptSuspend();
            continue;
        }
        if (state == State::kSuspended) {
            if (!wake_requested_.load(std::memory_order_acquire)) {
                (void)ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
                continue;
            }
            AttemptResume();
            continue;
        }
        vTaskDelay(1U);
    }
}

void FunctionEvDisplayIdleController::TaskEntry(void* context) {
    static_cast<FunctionEvDisplayIdleController*>(context)->Run();
}

}  // namespace micropixel::platform::esp32_p4_function_ev_board
