#include "platform/lvgl/lvgl_wakeup.hpp"

#include <atomic>

namespace micropixel::platform::lvgl {
namespace {

struct ActivityRegistration final {
    DisplayActivityHook hook{};
    void* context{};
};

ActivityRegistration s_registration{};
std::atomic<const ActivityRegistration*> s_active_registration{};

}  // namespace

bool BindDisplayActivityHook(DisplayActivityHook hook, void* context) {
    if (hook == nullptr || context == nullptr) {
        return false;
    }
    const ActivityRegistration* expected = nullptr;
    s_registration = {.hook = hook, .context = context};
    if (!s_active_registration.compare_exchange_strong(expected, &s_registration, std::memory_order_release,
                                                       std::memory_order_relaxed)) {
        return false;
    }
    return true;
}

bool ReportDisplayActivity(lv_display_t* display, DisplayActivityIntent intent) {
    if (display == nullptr) {
        return false;
    }
    const ActivityRegistration* registration = s_active_registration.load(std::memory_order_acquire);
    if (registration == nullptr) {
        return true;
    }
    return registration->hook(registration->context, display, intent);
}

}  // namespace micropixel::platform::lvgl
