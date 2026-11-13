#include "runtime/wamr/diagnostics.h"

#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_heap_caps.h"
#include "esp_heap_trace.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define WAMR_TASK_STACK_SIZE (16U * 1024U)

static const char* TAG = "micropixel_diag";

#if defined(CONFIG_MICROPIXEL_HEAP_CHECKPOINTS) && CONFIG_MICROPIXEL_HEAP_CHECKPOINTS && \
    defined(CONFIG_HEAP_TRACING_STANDALONE) && CONFIG_HEAP_TRACING_STANDALONE
#define MICROPIXEL_HEAP_TRACE_RECORD_COUNT 2048U
static heap_trace_record_t* s_heap_trace_records;

void micropixel_start_heap_trace(void) {
    const size_t bytes = MICROPIXEL_HEAP_TRACE_RECORD_COUNT * sizeof(heap_trace_record_t);
    s_heap_trace_records = heap_caps_calloc(1U, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (s_heap_trace_records == NULL) {
        esp_rom_printf("HEAP TRACE unavailable: unable to allocate %zu-byte PSRAM buffer\n", bytes);
        return;
    }

    esp_err_t status = heap_trace_init_standalone(s_heap_trace_records, MICROPIXEL_HEAP_TRACE_RECORD_COUNT);
    if (status != ESP_OK) {
        esp_rom_printf("HEAP TRACE unavailable: init failed err=%d\n", (int)status);
        return;
    }
    status = heap_trace_start(HEAP_TRACE_LEAKS);
    if (status != ESP_OK) {
        esp_rom_printf("HEAP TRACE unavailable: start failed err=%d\n", (int)status);
        return;
    }
    esp_rom_printf("HEAP TRACE active: PSRAM live allocations, records=%u buffer=%p size=%zu\n",
                   MICROPIXEL_HEAP_TRACE_RECORD_COUNT, s_heap_trace_records, bytes);
}
#endif

static void log_native_stack_profile(const char* label, size_t configured, size_t minimum_free) {
    if (minimum_free == SIZE_MAX) {
        ESP_LOGW(TAG, "%s native stack: no sample", label);
        return;
    }

    size_t maximum_used = configured > minimum_free ? configured - minimum_free : 0;
    ESP_LOGI(TAG, "%s native stack: configured=%zu B, max-used=%zu B, minimum-free=%zu B, utilization=%zu%%", label,
             configured, maximum_used, minimum_free, configured > 0 ? maximum_used * 100U / configured : 0);
}

#if defined(CONFIG_MICROPIXEL_HEAP_CHECKPOINTS) && CONFIG_MICROPIXEL_HEAP_CHECKPOINTS
void micropixel_check_heap(const char* stage) {
    // The allocator's integrity walker can itself fault on damaged metadata.
    // Emit the stage before entering it; bypass the heap-backed log capture ring.
    esp_rom_printf("HEAP CHECK begin: %s\n", stage);
    if (!heap_caps_check_integrity_all(false)) {
        esp_rom_printf("\nMICROPIXEL HEAP CORRUPTION: stage=%s\n", stage);
        (void)heap_caps_check_integrity_all(true);
        abort();
    }
    esp_rom_printf("HEAP CHECK ok: %s\n", stage);
}
#endif

#if defined(CONFIG_MICROPIXEL_HEAP_SERVICE_CHECKPOINTS) && CONFIG_MICROPIXEL_HEAP_SERVICE_CHECKPOINTS
void micropixel_check_heap_service(const char* stage) { micropixel_check_heap(stage); }
#endif

void micropixel_log_heap_state(const char* label) {
    micropixel_check_heap(label);
    const uint32_t internal_caps = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT;
    const uint32_t psram_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;

    ESP_LOGI(TAG,
             "%s heap bytes: SRAM(total/free/min/largest)=%zu/%zu/%zu/%zu "
             "PSRAM(total/free/min/largest)=%zu/%zu/%zu/%zu",
             label, heap_caps_get_total_size(internal_caps), heap_caps_get_free_size(internal_caps),
             heap_caps_get_minimum_free_size(internal_caps), heap_caps_get_largest_free_block(internal_caps),
             heap_caps_get_total_size(psram_caps), heap_caps_get_free_size(psram_caps),
             heap_caps_get_minimum_free_size(psram_caps), heap_caps_get_largest_free_block(psram_caps));
}

void micropixel_log_stack_profiles(void) {
    size_t host_minimum_free = (size_t)uxTaskGetStackHighWaterMark2(NULL);
    log_native_stack_profile("WAMR Host pthread", WAMR_TASK_STACK_SIZE, host_minimum_free);
}
