#include "runtime/services/network_service.hpp"

#include <cstdio>

#include "runtime/services/timer_service.hpp"

namespace micropixel::runtime {

static_assert(sizeof(micropixel_network_complete_event_payload_t) == 16U);

NetworkService::NetworkService(device::ManagedNetworkService& network, EventQueue& events, TimerService& clock,
                               std::string_view app_id)
    : network_(network), events_(events), clock_(clock) {
    (void)std::snprintf(app_id_.data(), app_id_.size(), "%.*s", static_cast<int>(app_id.size()), app_id.data());
    (void)Resume();
}

NetworkService::~NetworkService() { Shutdown(); }

ServiceResult<micropixel_network_info_t> NetworkService::GetInfo() const {
    if (!valid()) return FailService<micropixel_network_info_t>(MICROPIXEL_STATUS_INTERNAL);
    device::ManagedNetworkInfo info{};
    const int32_t status = network_.GetInfo(session_, info);
    if (status != MICROPIXEL_STATUS_OK) return FailService<micropixel_network_info_t>(status);
    micropixel_network_info_t wire{};
    wire.size = sizeof(wire);
    wire.configured = info.configured ? 1U : 0U;
    wire.route_online = info.route_online ? 1U : 0U;
    wire.time_synchronized = info.time_synchronized ? 1U : 0U;
    wire.max_active_requests = info.max_active_requests;
    wire.max_queued_requests = info.max_queued_requests;
    wire.max_response_bytes = MICROPIXEL_NETWORK_MAX_RESPONSE_BYTES;
    wire.max_request_body_bytes = MICROPIXEL_NETWORK_MAX_BODY_BYTES;
    wire.profile_revision = info.profile_revision;
    wire.capabilities = info.capabilities;
    return wire;
}

ServiceResult<micropixel_network_start_response_t> NetworkService::Start(
    const device::ManagedNetworkRequest& request) {
    if (!valid()) return FailService<micropixel_network_start_response_t>(MICROPIXEL_STATUS_INTERNAL);
    uint32_t handle = 0U;
    const int32_t status = network_.Start(session_, request, handle);
    if (status != MICROPIXEL_STATUS_OK) return FailService<micropixel_network_start_response_t>(status);
    micropixel_network_start_response_t response{};
    response.size = sizeof(response);
    response.request_handle = handle;
    return response;
}

ServiceResult<uint32_t> NetworkService::Read(uint32_t handle, uint32_t offset, std::span<uint8_t> destination,
                                             uint32_t& total_length_out) const {
    if (!valid()) return FailService<uint32_t>(MICROPIXEL_STATUS_INTERNAL);
    uint32_t length = 0U;
    const int32_t status = network_.Read(session_, handle, offset, destination, length, total_length_out);
    return status == MICROPIXEL_STATUS_OK ? ServiceResult<uint32_t>{length} : FailService<uint32_t>(status);
}

ServiceResult<void> NetworkService::Cancel(uint32_t handle) {
    if (!valid()) return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    const int32_t status = network_.Cancel(session_, handle);
    return status == MICROPIXEL_STATUS_OK ? ServiceResult<void>{} : FailService<void>(status);
}

ServiceResult<void> NetworkService::Close(uint32_t handle) {
    if (!valid()) return FailService<void>(MICROPIXEL_STATUS_INTERNAL);
    const int32_t status = network_.Close(session_, handle);
    return status == MICROPIXEL_STATUS_OK ? ServiceResult<void>{} : FailService<void>(status);
}

void NetworkService::Suspend() {
    if (session_ == 0U) return;
    network_.CloseSession(session_);
    session_ = 0U;
}

bool NetworkService::Resume() {
    return session_ != 0U ||
           network_.OpenSession(app_id_.data(), OnComplete, this, session_) == MICROPIXEL_STATUS_OK;
}

void NetworkService::Shutdown() { Suspend(); }

void NetworkService::OnComplete(void* context, const device::ManagedNetworkCompletion& completion) {
    if (context != nullptr) static_cast<NetworkService*>(context)->HandleComplete(completion);
}

void NetworkService::HandleComplete(const device::ManagedNetworkCompletion& completion) {
    micropixel_event_t event{};
    event.size = sizeof(event);
    event.event_id = MICROPIXEL_NETWORK_EVENT_REQUEST_COMPLETE;
    event.service_id = MICROPIXEL_SERVICE_NETWORK;
    event.source = completion.request_handle;
    event.timestamp_us = clock_.Now();
    event.sequence = ++sequence_;
    event.status = completion.transport_status;
    auto* payload = reinterpret_cast<micropixel_network_complete_event_payload_t*>(event.payload);
    payload->request_handle = completion.request_handle;
    payload->http_status = completion.http_status;
    payload->body_length = completion.response_length;
    payload->cache_age_seconds = completion.cache_age_seconds;
    payload->source = completion.source;
    (void)events_.PushRequired(event);
}

}  // namespace micropixel::runtime
