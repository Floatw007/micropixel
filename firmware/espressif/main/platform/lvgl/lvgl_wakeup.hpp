#ifndef MICROPIXEL_PLATFORM_LVGL_LVGL_WAKEUP_HPP
#define MICROPIXEL_PLATFORM_LVGL_LVGL_WAKEUP_HPP

#include <cstdint>

#include "esp_lv_adapter.h"
#include "lvgl.h"

namespace micropixel::platform::lvgl {

enum class DisplayActivityIntent : uint8_t {
    kForeground,
    kPassive,
};

// A board may use this hook to coordinate a display whose panel resources are
// deleted while idle. Returning false tells the caller that the display is
// detached; the coordinator is then responsible for a full refresh after it
// has rebuilt and rebound the panel.
using DisplayActivityHook = bool (*)(void* context, lv_display_t* display, DisplayActivityIntent intent);

[[nodiscard]] bool BindDisplayActivityHook(DisplayActivityHook hook, void* context);
[[nodiscard]] bool ReportDisplayActivity(lv_display_t* display,
                                         DisplayActivityIntent intent = DisplayActivityIntent::kForeground);

// LVGL's display refresh timer is intentionally slow while the scene is
// static. Mark it ready and wake the adapter together so an update is not left
// pending while the event-driven LVGL worker is asleep.
inline void RequestDisplayRefresh(lv_display_t* display) {
    if (display == nullptr || !ReportDisplayActivity(display)) {
        return;
    }
    lv_timer_ready(lv_display_get_refr_timer(display));
    (void)esp_lv_adapter_request_wake();
}

// Background-only changes (the clock and fallback status values) are rendered
// while the panel is active, but neither postpone a board-level idle suspend
// nor wake a panel that has already been detached.
inline void RequestPassiveDisplayRefresh(lv_display_t* display) {
    if (display == nullptr || !ReportDisplayActivity(display, DisplayActivityIntent::kPassive)) {
        return;
    }
    lv_timer_ready(lv_display_get_refr_timer(display));
    (void)esp_lv_adapter_request_wake();
}

// Static scenes use a deliberately slow display refresh timer. Native LVGL
// animations still advance on their own timer, so this small controller marks
// the display refresh timer ready once per normal LVGL frame while at least
// one animation is running. It stops itself as soon as the scene is static.
class AnimatedDisplayRefresh final {
   public:
    AnimatedDisplayRefresh() = default;
    AnimatedDisplayRefresh(const AnimatedDisplayRefresh&) = delete;
    AnimatedDisplayRefresh& operator=(const AnimatedDisplayRefresh&) = delete;

    void Start(lv_display_t* display) {
        if (display == nullptr) {
            return;
        }
        display_ = display;
        RequestDisplayRefresh(display_);
        if (timer_ == nullptr) {
            timer_ = lv_timer_create(TimerCallback, LV_DEF_REFR_PERIOD, this);
        } else {
            lv_timer_set_period(timer_, LV_DEF_REFR_PERIOD);
            lv_timer_reset(timer_);
            lv_timer_resume(timer_);
        }
        (void)esp_lv_adapter_request_wake();
    }

    void Stop() {
        if (timer_ != nullptr) {
            lv_timer_delete(timer_);
            timer_ = nullptr;
        }
        display_ = nullptr;
    }

   private:
    static void TimerCallback(lv_timer_t* timer) {
        auto* controller = static_cast<AnimatedDisplayRefresh*>(lv_timer_get_user_data(timer));
        if (controller == nullptr) {
            lv_timer_delete(timer);
            return;
        }
        RequestDisplayRefresh(controller->display_);
        if (lv_anim_count_running() == 0U) {
            controller->timer_ = nullptr;
            controller->display_ = nullptr;
            lv_timer_delete(timer);
        }
    }

    lv_timer_t* timer_{};
    lv_display_t* display_{};
};

}  // namespace micropixel::platform::lvgl

#endif
