#ifndef MICROPIXEL_DEVICE_CONTRACTS_MANAGED_NETWORK_HPP
#define MICROPIXEL_DEVICE_CONTRACTS_MANAGED_NETWORK_HPP

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "abi/micropixel_abi.h"

namespace micropixel::device {

inline constexpr size_t kManagedNetworkAppIdMaxLength = 64U;

struct ManagedNetworkInfo final {
    bool configured{};
    bool route_online{};
    bool time_synchronized{};
    uint32_t profile_revision{};
    uint64_t capabilities{};
    uint8_t max_active_requests{};
    uint8_t max_queued_requests{};
};

struct ManagedNetworkRequest final {
    uint8_t method{};
    uint8_t cache_mode{};
    std::string_view path{};
    std::span<const uint8_t> body{};
    std::string_view idempotency_key{};
};

struct ManagedNetworkCompletion final {
    uint32_t request_handle{};
    int32_t transport_status{MICROPIXEL_STATUS_INTERNAL};
    uint16_t http_status{};
    uint32_t response_length{};
    uint32_t cache_age_seconds{};
    uint8_t source{MICROPIXEL_NETWORK_RESPONSE_LIVE};
};

using ManagedNetworkCompletionSink = void (*)(void* context, const ManagedNetworkCompletion& completion);

// Host-owned authenticated HTTP client. Implementations must copy request data
// before Start returns, perform transport work away from the caller, and scope
// every handle and completion callback to one Guest session.
class ManagedNetwork {
   public:
    virtual ~ManagedNetwork() = default;

    [[nodiscard]] virtual int32_t OpenSession(std::string_view app_id, ManagedNetworkCompletionSink sink, void* context,
                                              uint32_t& session_out) = 0;
    virtual void CloseSession(uint32_t session) = 0;
    [[nodiscard]] virtual int32_t GetInfo(uint32_t session, ManagedNetworkInfo& info_out) const = 0;
    [[nodiscard]] virtual int32_t Start(uint32_t session, const ManagedNetworkRequest& request,
                                        uint32_t& handle_out) = 0;
    [[nodiscard]] virtual int32_t Read(uint32_t session, uint32_t handle, uint32_t offset,
                                       std::span<uint8_t> destination, uint32_t& length_out,
                                       uint32_t& total_length_out) const = 0;
    [[nodiscard]] virtual int32_t Cancel(uint32_t session, uint32_t handle) = 0;
    [[nodiscard]] virtual int32_t Close(uint32_t session, uint32_t handle) = 0;
};

}  // namespace micropixel::device

#endif
