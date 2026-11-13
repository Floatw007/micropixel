#ifndef MICROPIXEL_RUNTIME_MEMORY_GUEST_PSRAM_HPP
#define MICROPIXEL_RUNTIME_MEMORY_GUEST_PSRAM_HPP

#include <cstddef>

namespace micropixel::runtime {

struct GuestPsramState final {
    size_t free_bytes{};
    size_t largest_free_block{};
};

[[nodiscard]] constexpr size_t GuestPsramAllocationCapacity(const GuestPsramState& state) {
    // ESP32-P4 can report a zero largest block for its external heap while
    // heap_caps_malloc(MALLOC_CAP_SPIRAM) still succeeds. In that inconsistent
    // state, retain the free-byte quota and let the allocator decide whether a
    // contiguous request fits. A non-zero largest-block value remains a useful
    // fragmentation limit.
    return state.largest_free_block == 0U && state.free_bytes != 0U ? state.free_bytes : state.largest_free_block;
}

[[nodiscard]] constexpr bool IsGuestPsramAllocationAdmissible(const GuestPsramState& state, size_t bytes,
                                                              size_t reserve_bytes) {
    return bytes != 0U && state.free_bytes >= reserve_bytes && bytes <= state.free_bytes - reserve_bytes &&
           bytes <= GuestPsramAllocationCapacity(state);
}

[[nodiscard]] constexpr bool IsGuestPsramGrowthAdmissible(size_t free_bytes, size_t additional_bytes,
                                                          size_t reserve_bytes) {
    return free_bytes >= reserve_bytes && additional_bytes <= free_bytes - reserve_bytes;
}

[[nodiscard]] size_t GuestPsramReserveBytes();
[[nodiscard]] GuestPsramState CurrentGuestPsramState();
[[nodiscard]] bool CanGrowGuestPsram(size_t additional_bytes);
[[nodiscard]] void* AllocateGuestPsram(size_t bytes);
[[nodiscard]] void* AllocateAlignedGuestPsram(size_t alignment, size_t bytes);

}  // namespace micropixel::runtime

#endif
