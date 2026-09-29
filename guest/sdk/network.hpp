#ifndef MICROPIXEL_SDK_NETWORK_HPP
#define MICROPIXEL_SDK_NETWORK_HPP

#include <stdint.h>

#include "sdk/event.hpp"
#include "sdk/result.hpp"

namespace micropixel {

enum class HttpMethod : uint8_t {
    kGet = 1,
    kPost = 2,
    kPut = 3,
    kPatch = 4,
    kDelete = 5,
};

enum class NetworkCacheMode : uint8_t {
    kNone = 0,
    kMemoryFallback = 1,
    kPersistentFallback = 2,
};

enum class NetworkResponseSource : uint8_t {
    kLive = 1,
    kMemoryCache = 2,
    kPersistentCache = 3,
};

struct NetworkInfo final {
    bool configured{};
    bool route_online{};
    bool time_synchronized{};
    uint8_t max_active_requests{};
    uint8_t max_queued_requests{};
    uint32_t max_response_bytes{};
    uint32_t max_request_body_bytes{};
    uint32_t profile_revision{};
    bool supports_https{};
    bool authenticated_profile{};
    bool memory_cache{};
    bool persistent_cache{};
};

struct NetworkRequestOptions final {
    HttpMethod method{HttpMethod::kGet};
    NetworkCacheMode cache_mode{NetworkCacheMode::kNone};
    const char* path{};
    const uint8_t* body{};
    uint32_t body_length{};
    const char* idempotency_key{};
};

class NetworkRequest final {
   public:
    NetworkRequest() = default;
    NetworkRequest(const NetworkRequest&) = delete;
    NetworkRequest& operator=(const NetworkRequest&) = delete;
    NetworkRequest(NetworkRequest&& other) noexcept;
    NetworkRequest& operator=(NetworkRequest&& other) noexcept;
    ~NetworkRequest();

    [[nodiscard]] constexpr bool valid() const { return handle_ != 0U; }
    [[nodiscard]] constexpr uint32_t handle() const { return handle_; }
    [[nodiscard]] Result<uint32_t> Read(uint32_t offset, uint8_t* bytes, uint32_t capacity) const;
    [[nodiscard]] Result<void> Cancel();
    [[nodiscard]] Result<void> Close();
    void Reset();

   private:
    explicit constexpr NetworkRequest(uint32_t handle) : handle_(handle) {}
    [[nodiscard]] constexpr bool Matches(const NetworkCompleteEvent& event) const;

    uint32_t handle_{};
    friend class Network;
    friend class Event;
};

class Network final {
   public:
    [[nodiscard]] Result<NetworkInfo> info() const;
    [[nodiscard]] Result<NetworkRequest> Start(const NetworkRequestOptions& options) const;

   private:
    struct CapabilityToken final {
       private:
        constexpr CapabilityToken() = default;
        friend class Application;
    };

    explicit constexpr Network(CapabilityToken) noexcept {}
    friend class Application;
};

inline constexpr bool NetworkRequest::Matches(const NetworkCompleteEvent& event) const {
    return handle_ != 0U && event.request_handle() == handle_;
}

inline const NetworkCompleteEvent* Event::NetworkFrom(const NetworkRequest& source) const {
    const NetworkCompleteEvent* candidate = network_complete();
    return candidate != nullptr && source.Matches(*candidate) ? candidate : nullptr;
}

}  // namespace micropixel

#endif
