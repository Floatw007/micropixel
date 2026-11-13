#include "work/background_executor.hpp"

#include <inttypes.h>
#include <stdint.h>

#include "esp_log.h"
#include "esp_rom_sys.h"
#include "runtime/wamr/diagnostics.h"
#include "work/task_policy.hpp"

namespace micropixel::work {
namespace {

constexpr char kTag[] = "micropixel_background";
constexpr uint32_t kTaskStackBytes = 8U * 1024U;

}  // namespace

BackgroundExecutor::BackgroundExecutor(const char* task_name)
    : queue_(xQueueCreateStatic(kQueueCapacity, sizeof(Job), queue_bytes_.data(), &queue_storage_)),
      stopped_(xSemaphoreCreateBinaryStatic(&stopped_storage_)) {
    if (queue_ == nullptr || stopped_ == nullptr ||
        xTaskCreatePinnedToCore(WorkerEntry, task_name, kTaskStackBytes, this, task_policy::kAssetWorkerPriority,
                                &worker_, task_policy::kSystemCore) != pdPASS) {
        worker_ = nullptr;
        ESP_LOGE(kTag, "unable to create shared background executor");
    }
}

BackgroundExecutor::~BackgroundExecutor() { Shutdown(); }

bool BackgroundExecutor::valid() const { return queue_ != nullptr && stopped_ != nullptr && worker_ != nullptr; }

bool BackgroundExecutor::Submit(Function function, void* context) {
    if (function == nullptr || !valid() || stopping_.load(std::memory_order_acquire)) {
        return false;
    }
    const Job job{function, context};
    return xQueueSend(queue_, &job, 0U) == pdTRUE;
}

void BackgroundExecutor::Shutdown() {
    if (stopping_.exchange(true, std::memory_order_acq_rel) || worker_ == nullptr) {
        return;
    }
    const Job stop{};
    (void)xQueueSend(queue_, &stop, portMAX_DELAY);
    (void)xSemaphoreTake(stopped_, portMAX_DELAY);
    worker_ = nullptr;
}

void BackgroundExecutor::WorkerEntry(void* context) {
    static_cast<BackgroundExecutor*>(context)->WorkerLoop();
    vTaskDelete(nullptr);
}

void BackgroundExecutor::WorkerLoop() {
#if defined(CONFIG_MICROPIXEL_HEAP_CHECKPOINTS) && CONFIG_MICROPIXEL_HEAP_CHECKPOINTS
    uint32_t job_sequence = 0U;
#endif
    for (;;) {
        Job job{};
        if (xQueueReceive(queue_, &job, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (job.function == nullptr) {
            break;
        }
#if defined(CONFIG_MICROPIXEL_HEAP_CHECKPOINTS) && CONFIG_MICROPIXEL_HEAP_CHECKPOINTS
        ++job_sequence;
        const uintptr_t callback_address = reinterpret_cast<uintptr_t>(job.function);
        esp_rom_printf("BACKGROUND JOB begin: sequence=%" PRIu32 " callback=0x%" PRIxPTR "\n", job_sequence,
                       callback_address);
#endif
        micropixel_check_heap("before shared background job");
        job.function(job.context);
        micropixel_check_heap("after shared background job");
    }
    (void)xSemaphoreGive(stopped_);
}

}  // namespace micropixel::work
