#pragma once

#include <atomic>
#include <cstdint>

#include "device/contracts/input.hpp"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "platform/boards/esp32-p4-function-ev-board/display/display_pipeline.hpp"
#include "platform/input/esp_lcd_touch_input.hpp"
#include "platform/lvgl/guest_graphics_engine.hpp"
#include "platform/lvgl/lvgl_wakeup.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {

// Owns the Function EV Board's board-level display idle state. Unlike the
// LVGL adapter's short pause, a suspended state deletes the DPI panel so the
// IDF driver releases its ESP_PM_CPU_FREQ_MAX lock.
class FunctionEvDisplayIdleController final {
   public:
    FunctionEvDisplayIdleController(FunctionEvDisplayPipeline& pipeline, lvgl::GuestGraphicsEngine& guest_graphics)
        : pipeline_(pipeline), guest_graphics_(guest_graphics) {}
    FunctionEvDisplayIdleController(const FunctionEvDisplayIdleController&) = delete;
    FunctionEvDisplayIdleController& operator=(const FunctionEvDisplayIdleController&) = delete;

    [[nodiscard]] esp_err_t Start(lv_display_t* display, input::EspLcdTouchInput& touch_input);
    [[nodiscard]] bool EnsureAwake(TickType_t timeout_ticks = pdMS_TO_TICKS(5000U));

   private:
    enum class State : uint8_t {
        kActive,
        kSuspending,
        kSuspended,
        kResuming,
    };

    void Run();
    void RecordForegroundActivity();
    [[nodiscard]] bool IdleDeadlineReached() const;
    [[nodiscard]] bool RecoverPreparedDisplay(bool rebuild_panel);
    void AttemptSuspend();
    void AttemptResume();
    static void TaskEntry(void* context);
    static bool ActivityHook(void* context, lv_display_t* display, lvgl::DisplayActivityIntent intent);
    static bool TouchGate(void* context, const device::TouchSample& sample);

    FunctionEvDisplayPipeline& pipeline_;
    lvgl::GuestGraphicsEngine& guest_graphics_;
    lv_display_t* display_{};
    TaskHandle_t task_{};
    std::atomic<State> state_{State::kActive};
    std::atomic<int64_t> last_activity_us_{};
    std::atomic<bool> wake_requested_{};
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board
