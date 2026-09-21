#include "platform/boards/esp32-p4-function-ev-board/idle_frequency_telemetry.hpp"

#include <cstdint>

#include "esp_log.h"
#include "esp_pm.h"
#include "esp_rom_sys.h"
#include "work/task_policy.hpp"

namespace micropixel::platform::esp32_p4_function_ev_board {
namespace {

constexpr char kTag[] = "function_ev_power";
constexpr uint32_t kTaskStackBytes = 2048U;
constexpr uint32_t kSampleDelaysMs[] = {5000U, 10000U, 15000U};

}  // namespace

esp_err_t IdleFrequencyTelemetry::Start() {
    if (task_ != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    const BaseType_t created = xTaskCreatePinnedToCore(TaskEntry, "idle_freq", kTaskStackBytes, this,
                                                       tskIDLE_PRIORITY + 1U, &task_, task_policy::kSystemCore);
    return created == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void IdleFrequencyTelemetry::TaskEntry(void* context) {
    static_cast<IdleFrequencyTelemetry*>(context)->Run();
}

void IdleFrequencyTelemetry::Run() {
    esp_pm_config_t power_config{};
    const bool configured = esp_pm_get_configuration(&power_config) == ESP_OK;
    for (uint32_t index = 0U; index < sizeof(kSampleDelaysMs) / sizeof(kSampleDelaysMs[0]); ++index) {
        vTaskDelay(pdMS_TO_TICKS(kSampleDelaysMs[index]));
        // Read before logging: the console write itself can acquire clocks and
        // must not influence the frequency being reported.
        const uint32_t frequency_mhz = esp_rom_get_cpu_ticks_per_us();
        if (configured) {
            ESP_LOGI(kTag, "idle DFS sample %lu/3: cpu=%lu MHz configured=%d..%d MHz light-sleep=%s",
                     static_cast<unsigned long>(index + 1U), static_cast<unsigned long>(frequency_mhz),
                     power_config.min_freq_mhz, power_config.max_freq_mhz,
                     power_config.light_sleep_enable ? "automatic" : "explicit-only");
        } else {
            ESP_LOGI(kTag, "idle DFS sample %lu/3: cpu=%lu MHz", static_cast<unsigned long>(index + 1U),
                     static_cast<unsigned long>(frequency_mhz));
        }
    }
    ESP_LOGI(kTag, "idle DFS sampling complete; probe task is stopping");
    task_ = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace micropixel::platform::esp32_p4_function_ev_board
