#ifndef MICROPIXEL_RUNTIME_ABI_NETWORK_POLICY_HPP
#define MICROPIXEL_RUNTIME_ABI_NETWORK_POLICY_HPP

#include <algorithm>
#include <string_view>

#include "abi/micropixel_abi.h"

namespace micropixel::runtime {

[[nodiscard]] inline bool ValidRelativeNetworkPath(std::string_view path) {
    if (path.empty() || path.size() > MICROPIXEL_NETWORK_MAX_PATH_BYTES || path.front() != '/' ||
        (path.size() > 1U && path[1] == '/')) return false;
    for (size_t index = 0U; index < path.size(); ++index) {
        const unsigned char character = static_cast<unsigned char>(path[index]);
        if (character < 0x21U || character > 0x7eU || character == '\\') return false;
        if (character == '.' && index + 1U < path.size() && path[index + 1U] == '.' &&
            (index == 0U || path[index - 1U] == '/') &&
            (index + 2U == path.size() || path[index + 2U] == '/' || path[index + 2U] == '?' ||
             path[index + 2U] == '#')) return false;
    }
    return true;
}

[[nodiscard]] inline bool ValidIdempotencyKey(std::string_view key) {
    if (key.size() > MICROPIXEL_NETWORK_MAX_IDEMPOTENCY_KEY_BYTES) return false;
    return std::all_of(key.begin(), key.end(), [](char value) {
        const unsigned char character = static_cast<unsigned char>(value);
        return character >= 0x21U && character <= 0x7eU;
    });
}

}  // namespace micropixel::runtime

#endif
