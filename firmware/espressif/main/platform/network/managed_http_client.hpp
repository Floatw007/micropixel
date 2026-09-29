#ifndef MICROPIXEL_PLATFORM_NETWORK_MANAGED_HTTP_CLIENT_HPP
#define MICROPIXEL_PLATFORM_NETWORK_MANAGED_HTTP_CLIENT_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>

#include "device/contracts/managed_network.hpp"
#include "esp_http_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/network/network.hpp"
#include "runtime/bundle/bundle_format.h"

namespace micropixel::platform::network {

struct TerminalProfileStatus final {
    bool configured{};
    uint32_t revision{};
    uint64_t store_id{};
    std::array<char, 257U> origin{};
    std::array<char, 65U> allowed_app_id{};
    std::array<char, 17U> certificate_fingerprint{};
    std::array<char, 9U> token_suffix{};
};

class ManagedHttpClient final : public device::ManagedNetwork {
   public:
    explicit ManagedHttpClient(host::network::Network& network);
    ~ManagedHttpClient() override;
    ManagedHttpClient(const ManagedHttpClient&) = delete;
    ManagedHttpClient& operator=(const ManagedHttpClient&) = delete;

    [[nodiscard]] bool valid() const { return mutex_ != nullptr && queue_ != nullptr && workers_started_ == 2U; }

    [[nodiscard]] int32_t OpenSession(std::string_view app_id, device::ManagedNetworkCompletionSink sink,
                                      void* context, uint32_t& session_out) override;
    void CloseSession(uint32_t session) override;
    [[nodiscard]] int32_t GetInfo(uint32_t session, device::ManagedNetworkInfo& info_out) const override;
    [[nodiscard]] int32_t Start(uint32_t session, const device::ManagedNetworkRequest& request,
                                uint32_t& handle_out) override;
    [[nodiscard]] int32_t Read(uint32_t session, uint32_t handle, uint32_t offset,
                               std::span<uint8_t> destination, uint32_t& length_out,
                               uint32_t& total_length_out) const override;
    [[nodiscard]] int32_t Cancel(uint32_t session, uint32_t handle) override;
    [[nodiscard]] int32_t Close(uint32_t session, uint32_t handle) override;

    // `blob` is the versioned binary profile produced by tools/micropixel. It
    // is deliberately not accepted as command-line fields, keeping PEM and
    // bearer secrets out of process listings and serial logs.
    [[nodiscard]] int32_t Configure(std::span<const uint8_t> blob);
    [[nodiscard]] int32_t ClearProfile();
    [[nodiscard]] TerminalProfileStatus ProfileStatus() const;

   private:
    static constexpr uint32_t kProfileMagic = 0x31544e4dU;  // MNT1
    static constexpr uint16_t kProfileVersion = 1U;
    static constexpr uint32_t kCacheMagic = 0x31434e4dU;  // MNC1
    static constexpr size_t kSlotCount = 6U;
    static constexpr size_t kWorkerCount = 2U;
    static constexpr size_t kMemoryCacheCount = 4U;

    struct ProfileRecord final {
        uint32_t magic{};
        uint16_t version{};
        uint16_t size{};
        uint32_t revision{};
        uint64_t store_id{};
        uint16_t origin_length{};
        uint16_t ca_length{};
        uint16_t token_length{};
        uint16_t app_id_length{};
        std::array<char, 257U> origin{};
        std::array<char, 4097U> ca_pem{};
        std::array<char, 1025U> bearer_token{};
        std::array<char, 65U> allowed_app_id{};
        std::array<uint8_t, 32U> certificate_sha256{};
    };

    enum class SlotState : uint8_t { kFree, kQueued, kActive, kComplete };

    struct RequestSlot final {
        SlotState state{SlotState::kFree};
        uint8_t method{};
        uint8_t cache_mode{};
        bool close_requested{};
        uint32_t generation{};
        uint32_t handle{};
        uint32_t session{};
        std::atomic<bool> cancelled{};
        uint16_t path_length{};
        uint16_t key_length{};
        uint32_t body_length{};
        std::array<char, MICROPIXEL_NETWORK_MAX_PATH_BYTES + 1U> path{};
        std::array<char, MICROPIXEL_NETWORK_MAX_IDEMPOTENCY_KEY_BYTES + 1U> idempotency_key{};
        std::array<uint8_t, MICROPIXEL_NETWORK_MAX_BODY_BYTES> body{};
        std::array<char, 514U> url{};
        std::array<char, 1033U> authorization{};
        uint8_t* response{};
        uint32_t response_length{};
        uint32_t receive_offset{};
        bool response_too_large{};
        uint16_t http_status{};
        uint8_t response_source{MICROPIXEL_NETWORK_RESPONSE_LIVE};
        uint32_t cache_age_seconds{};
        int32_t transport_status{MICROPIXEL_STATUS_INTERNAL};
    };

    struct MemoryCache final {
        std::array<char, MICROPIXEL_NETWORK_MAX_PATH_BYTES + 1U> path{};
        uint8_t* bytes{};
        uint32_t length{};
        uint16_t http_status{};
        int64_t stored_at_seconds{};
        uint32_t use_sequence{};
    };

    struct PersistedCacheHeader final {
        uint32_t magic{};
        uint16_t version{};
        uint16_t http_status{};
        int64_t stored_at_seconds{};
        uint32_t body_length{};
        uint32_t body_crc32{};
    };

    static void WorkerEntry(void* context);
    static esp_err_t HttpEvent(esp_http_client_event_t* event);
    void WorkerLoop();
    void Execute(size_t slot_index);
    [[nodiscard]] int32_t PerformHttp(RequestSlot& slot);
    [[nodiscard]] bool UseFallbackCache(RequestSlot& slot);
    void StoreSuccessfulCache(const RequestSlot& slot);
    [[nodiscard]] bool LoadMemoryCache(std::string_view path, RequestSlot& slot);
    [[nodiscard]] bool LoadPersistentCache(std::string_view path, RequestSlot& slot);
    void StoreMemoryCache(std::string_view path, std::span<const uint8_t> bytes, uint16_t http_status);
    void StorePersistentCache(std::string_view path, std::span<const uint8_t> bytes, uint16_t http_status);
    [[nodiscard]] bool LoadProfile();
    [[nodiscard]] bool StoreProfile(const ProfileRecord& profile);
    [[nodiscard]] bool SessionMatches(uint32_t session) const;
    [[nodiscard]] RequestSlot* Find(uint32_t session, uint32_t handle);
    [[nodiscard]] const RequestSlot* Find(uint32_t session, uint32_t handle) const;
    void ReleaseSlot(RequestSlot& slot);
    [[nodiscard]] bool AnyRequestsLocked() const;
    [[nodiscard]] bool TakeLock() const;
    void GiveLock() const;

    host::network::Network& network_;
    mutable SemaphoreHandle_t mutex_{};
    QueueHandle_t queue_{};
    StaticQueue_t queue_storage_{};
    std::array<uint8_t, kSlotCount * sizeof(uint8_t)> queue_bytes_{};
    std::array<RequestSlot, kSlotCount> slots_{};
    std::array<MemoryCache, kMemoryCacheCount> memory_cache_{};
    ProfileRecord profile_{};
    std::array<TaskHandle_t, kWorkerCount> workers_{};
    std::atomic<bool> stopping_{};
    std::atomic<uint32_t> workers_exited_{};
    uint32_t workers_started_{};
    uint32_t session_generation_{};
    uint32_t active_session_{};
    uint32_t cache_use_sequence_{};
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> session_app_id_{};
    device::ManagedNetworkCompletionSink completion_sink_{};
    void* completion_context_{};
};

}  // namespace micropixel::platform::network

#endif
