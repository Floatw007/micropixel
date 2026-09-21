#pragma once

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::esp32_p4_function_ev_board {

// Finite bring-up probe: sample DFS after the boot workload settles, then
// remove its task so it cannot become a permanent idle wake source.
class IdleFrequencyTelemetry final {
   public:
    [[nodiscard]] esp_err_t Start();

   private:
    static void TaskEntry(void* context);
    void Run();

    TaskHandle_t task_{};
};

}  // namespace micropixel::platform::esp32_p4_function_ev_board
