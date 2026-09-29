#ifndef MICROPIXEL_RUNTIME_SERVICES_NETWORK_SERVICE_HPP
#define MICROPIXEL_RUNTIME_SERVICES_NETWORK_SERVICE_HPP

#include <array>
#include <string_view>

#include "device/device_services.hpp"
#include "runtime/event_queue.hpp"
#include "runtime/bundle/bundle_format.h"
#include "runtime/services/service_result.hpp"

namespace micropixel::runtime {

class TimerService;

class NetworkService final {
   public:
    NetworkService(device::ManagedNetworkService& network, EventQueue& events, TimerService& clock,
                   std::string_view app_id);
    NetworkService(const NetworkService&) = delete;
    NetworkService& operator=(const NetworkService&) = delete;
    ~NetworkService();

    [[nodiscard]] bool valid() const { return session_ != 0U; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] ServiceResult<micropixel_network_info_t> GetInfo() const;
    [[nodiscard]] ServiceResult<micropixel_network_start_response_t> Start(const device::ManagedNetworkRequest& request);
    [[nodiscard]] ServiceResult<uint32_t> Read(uint32_t handle, uint32_t offset, std::span<uint8_t> destination,
                                               uint32_t& total_length_out) const;
    [[nodiscard]] ServiceResult<void> Cancel(uint32_t handle);
    [[nodiscard]] ServiceResult<void> Close(uint32_t handle);
    void Suspend();
    [[nodiscard]] bool Resume();
    void Shutdown();

   private:
    static void OnComplete(void* context, const device::ManagedNetworkCompletion& completion);
    void HandleComplete(const device::ManagedNetworkCompletion& completion);

    device::ManagedNetworkService& network_;
    EventQueue& events_;
    TimerService& clock_;
    uint32_t session_{};
    uint32_t sequence_{};
    std::array<char, MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH + 1U> app_id_{};
};

}  // namespace micropixel::runtime

#endif
