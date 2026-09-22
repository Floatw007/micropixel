#include "platform/boards/esp32-p4-function-ev-board/idle_frequency_telemetry.hpp"

#include <cstdint>
#include <cstdio>

#include "esp_log.h"
#include "esp_pm.h"
#include "work/task_policy.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {
namespace {

constexpr char kTag[] = "function_ev_power";
constexpr uint32_t kTaskStackBytes = 2048U;
constexpr uint32_t kSampleDelaysMs[] = {5000U, 10000U, 25000U};

}  // namespace

esp_err_t IdleFrequencyTelemetry::Start() {
    if (task_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const BaseType_t created = xTaskCreatePinnedToCore(TaskEntry, "idle_freq", kTaskStackBytes, this,
                                                       tskIDLE_PRIORITY + 1U, &task_, task_policy::kSystemCore);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void IdleFrequencyTelemetry::TaskEntry(void* context) { static_cast<IdleFrequencyTelemetry*>(context)->Run(); }

void IdleFrequencyTelemetry::Run() {
    esp_pm_config_t power_config{};
    const bool configured = esp_pm_get_configuration(&power_config) == ESP_OK;
    for (uint32_t index = 0U; index < sizeof(kSampleDelaysMs) / sizeof(kSampleDelaysMs[0]); ++index) {
        vTaskDelay(pdMS_TO_TICKS(kSampleDelaysMs[index]));
        if (configured) {
            ESP_LOGI(kTag, "idle DFS checkpoint %lu/3: configured=%d..%d MHz light-sleep=%s",
                     static_cast<unsigned long>(index + 1U), power_config.min_freq_mhz, power_config.max_freq_mhz,
                     power_config.light_sleep_enable ? "automatic" : "explicit-only");
        } else {
            ESP_LOGI(kTag, "idle DFS checkpoint %lu/3", static_cast<unsigned long>(index + 1U));
        }
        if (index + 1U == sizeof(kSampleDelaysMs) / sizeof(kSampleDelaysMs[0])) {
            // A runnable task owns the per-core RTOS CPU_FREQ_MAX lock, so an
            // instantaneous read from this task would always observe 360 MHz.
            // PM profiling instead reports residency accumulated while all
            // application tasks were blocked and is not biased by this probe.
            (void)esp_pm_dump_locks(stdout);
        }
    }
    ESP_LOGI(kTag, "idle DFS sampling complete; probe task is stopping");
    task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace micropixel::platform::esp32_p4_function_ev_board
