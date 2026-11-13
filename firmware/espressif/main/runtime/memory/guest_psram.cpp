#include "runtime/memory/guest_psram.hpp"

#include <atomic>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "sdkconfig.h"

namespace micropixel::runtime {
namespace {

constexpr uint32_t kPsramCapabilities = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr char kTag[] = "micropixel_guest_psram";
std::atomic_bool unreliable_largest_block_logged;

template <typename Allocate>
void* AllocateWhilePreservingReserve(size_t bytes, Allocate allocate) {
    if (!IsGuestPsramAllocationAdmissible(CurrentGuestPsramState(), bytes, GuestPsramReserveBytes())) {
        return nullptr;
    }
    void* memory = allocate();
    if (memory != nullptr && heap_caps_get_free_size(kPsramCapabilities) < GuestPsramReserveBytes()) {
        heap_caps_free(memory);
        return nullptr;
    }
    return memory;
}

}  // namespace

size_t GuestPsramReserveBytes() { return CONFIG_MICROPIXEL_GUEST_PSRAM_RESERVE_BYTES; }

GuestPsramState CurrentGuestPsramState() {
    const GuestPsramState state{.free_bytes = heap_caps_get_free_size(kPsramCapabilities),
                                .largest_free_block = heap_caps_get_largest_free_block(kPsramCapabilities)};
    if (state.free_bytes != 0U && state.largest_free_block == 0U &&
        !unreliable_largest_block_logged.exchange(true, std::memory_order_relaxed)) {
        ESP_LOGW(kTag,
                 "PSRAM largest-block query returned zero with %zu bytes free; using the free-byte quota and "
                 "validating requests through the allocator",
                 state.free_bytes);
    }
    return state;
}

bool CanGrowGuestPsram(size_t additional_bytes) {
    return IsGuestPsramGrowthAdmissible(heap_caps_get_free_size(kPsramCapabilities), additional_bytes,
                                        GuestPsramReserveBytes());
}

void* AllocateGuestPsram(size_t bytes) {
    return AllocateWhilePreservingReserve(bytes, [bytes]() { return heap_caps_malloc(bytes, kPsramCapabilities); });
}

void* AllocateAlignedGuestPsram(size_t alignment, size_t bytes) {
    return AllocateWhilePreservingReserve(
        bytes, [alignment, bytes]() { return heap_caps_aligned_alloc(alignment, bytes, kPsramCapabilities); });
}

}  // namespace micropixel::runtime
