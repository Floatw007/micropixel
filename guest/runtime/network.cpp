#include "sdk/network.hpp"

#include "runtime/service_binding.hpp"

using micropixel::runtime::CallService;
using micropixel::runtime::CallVoid;
using micropixel::runtime::CopyBytes;
using micropixel::runtime::ErrorFromStatus;
using micropixel::runtime::OpenService;
using micropixel::runtime::ServiceCache;

namespace {

ServiceCache network_service;
alignas(4) uint8_t start_buffer[sizeof(micropixel_network_start_request_t) + MICROPIXEL_NETWORK_MAX_PATH_BYTES +
                                MICROPIXEL_NETWORK_MAX_BODY_BYTES + MICROPIXEL_NETWORK_MAX_IDEMPOTENCY_KEY_BYTES];
alignas(4) uint8_t read_buffer[sizeof(micropixel_network_read_response_t) + MICROPIXEL_NETWORK_MAX_READ_BYTES];

uint32_t TextLength(const char* text, uint32_t maximum) {
    if (text == nullptr) return 0U;
    uint32_t length = 0U;
    while (length <= maximum && text[length] != '\0') ++length;
    return length;
}

}  // namespace

namespace micropixel {

Result<NetworkInfo> Network::info() const {
    micropixel_network_info_t raw{};
    int32_t status = OpenService(network_service, MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_NETWORK_INTERFACE_MAJOR,
                                 MICROPIXEL_NETWORK_INTERFACE_MINOR);
    uint32_t response_size = 0U;
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(network_service, MICROPIXEL_NETWORK_METHOD_GET_INFO, nullptr, 0U, &raw, sizeof(raw),
                             response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(raw) || raw.size != sizeof(raw) || raw.configured > 1U || raw.route_online > 1U ||
        raw.time_synchronized > 1U || raw.reserved0 != 0U || raw.reserved1 != 0U ||
        raw.max_response_bytes > MICROPIXEL_NETWORK_MAX_RESPONSE_BYTES ||
        raw.max_request_body_bytes > MICROPIXEL_NETWORK_MAX_BODY_BYTES) {
        runtime::Panic("network.info.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return NetworkInfo{
        raw.configured != 0U,
        raw.route_online != 0U,
        raw.time_synchronized != 0U,
        raw.max_active_requests,
        raw.max_queued_requests,
        raw.max_response_bytes,
        raw.max_request_body_bytes,
        raw.profile_revision,
        (raw.capabilities & MICROPIXEL_NETWORK_CAP_HTTPS) != 0U,
        (raw.capabilities & MICROPIXEL_NETWORK_CAP_AUTHENTICATED_PROFILE) != 0U,
        (raw.capabilities & MICROPIXEL_NETWORK_CAP_MEMORY_CACHE) != 0U,
        (raw.capabilities & MICROPIXEL_NETWORK_CAP_PERSISTENT_CACHE) != 0U,
    };
}

Result<NetworkRequest> Network::Start(const NetworkRequestOptions& options) const {
    const uint32_t path_length = TextLength(options.path, MICROPIXEL_NETWORK_MAX_PATH_BYTES);
    const uint32_t key_length = TextLength(options.idempotency_key, MICROPIXEL_NETWORK_MAX_IDEMPOTENCY_KEY_BYTES);
    const uint8_t method = static_cast<uint8_t>(options.method);
    const uint8_t cache_mode = static_cast<uint8_t>(options.cache_mode);
    if (path_length == 0U || path_length > MICROPIXEL_NETWORK_MAX_PATH_BYTES || options.path[0] != '/' ||
        options.body_length > MICROPIXEL_NETWORK_MAX_BODY_BYTES ||
        (options.body_length != 0U && options.body == nullptr) ||
        key_length > MICROPIXEL_NETWORK_MAX_IDEMPOTENCY_KEY_BYTES || method < MICROPIXEL_NETWORK_HTTP_GET ||
        method > MICROPIXEL_NETWORK_HTTP_DELETE || cache_mode > MICROPIXEL_NETWORK_CACHE_PERSISTENT_FALLBACK ||
        ((method == MICROPIXEL_NETWORK_HTTP_GET || method == MICROPIXEL_NETWORK_HTTP_DELETE) &&
         options.body_length != 0U)) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    const uint32_t total = sizeof(micropixel_network_start_request_t) + path_length + options.body_length + key_length;
    auto* request = reinterpret_cast<micropixel_network_start_request_t*>(start_buffer);
    *request = {};
    request->size = static_cast<uint16_t>(total);
    request->method = method;
    request->cache_mode = cache_mode;
    request->path_length = static_cast<uint16_t>(path_length);
    request->idempotency_key_length = static_cast<uint16_t>(key_length);
    request->body_length = options.body_length;
    uint8_t* cursor = start_buffer + sizeof(*request);
    CopyBytes(cursor, options.path, path_length);
    cursor += path_length;
    if (options.body_length != 0U) {
        CopyBytes(cursor, options.body, options.body_length);
        cursor += options.body_length;
    }
    if (key_length != 0U) CopyBytes(cursor, options.idempotency_key, key_length);

    micropixel_network_start_response_t response{};
    uint32_t response_size = 0U;
    int32_t status = OpenService(network_service, MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_NETWORK_INTERFACE_MAJOR,
                                 MICROPIXEL_NETWORK_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(network_service, MICROPIXEL_NETWORK_METHOD_START, request, total, &response,
                             sizeof(response), response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    if (response_size != sizeof(response) || response.size != sizeof(response) || response.reserved0 != 0U ||
        response.request_handle == 0U) {
        runtime::Panic("network.start.response", MICROPIXEL_STATUS_INTERNAL);
    }
    return NetworkRequest{response.request_handle};
}

Result<uint32_t> NetworkRequest::Read(uint32_t offset, uint8_t* bytes, uint32_t capacity) const {
    if (!valid() || bytes == nullptr || capacity == 0U || capacity > MICROPIXEL_NETWORK_MAX_READ_BYTES) {
        return unexpected(Error{ErrorCode::kInvalidArgument});
    }
    micropixel_network_read_request_t request{};
    request.size = sizeof(request);
    request.request_handle = handle_;
    request.offset = offset;
    request.capacity = capacity;
    uint32_t response_size = 0U;
    int32_t status = OpenService(network_service, MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_NETWORK_INTERFACE_MAJOR,
                                 MICROPIXEL_NETWORK_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallService(network_service, MICROPIXEL_NETWORK_METHOD_READ, &request, sizeof(request), read_buffer,
                             sizeof(micropixel_network_read_response_t) + capacity, response_size);
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    micropixel_network_read_response_t header{};
    if (response_size < sizeof(header)) runtime::Panic("network.read.response", MICROPIXEL_STATUS_INTERNAL);
    CopyBytes(&header, read_buffer, sizeof(header));
    if (header.request_handle != handle_ || header.offset != offset || header.chunk_length > capacity ||
        header.size != sizeof(header) + header.chunk_length || response_size != header.size ||
        offset > header.total_length || header.chunk_length > header.total_length - offset) {
        runtime::Panic("network.read.response", MICROPIXEL_STATUS_INTERNAL);
    }
    CopyBytes(bytes, read_buffer + sizeof(header), header.chunk_length);
    return header.chunk_length;
}

Result<void> NetworkRequest::Cancel() {
    if (!valid()) return unexpected(Error{ErrorCode::kInvalidState});
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(network_service, MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_NETWORK_INTERFACE_MAJOR,
                                 MICROPIXEL_NETWORK_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(network_service, MICROPIXEL_NETWORK_METHOD_CANCEL, &request, sizeof(request));
    }
    return status == MICROPIXEL_STATUS_OK ? Result<void>{} : Result<void>{unexpected(ErrorFromStatus(status))};
}

Result<void> NetworkRequest::Close() {
    if (!valid()) return {};
    micropixel_handle_request_t request{static_cast<uint16_t>(sizeof(request)), 0U, handle_};
    int32_t status = OpenService(network_service, MICROPIXEL_SERVICE_NETWORK, MICROPIXEL_NETWORK_INTERFACE_MAJOR,
                                 MICROPIXEL_NETWORK_INTERFACE_MINOR);
    if (status == MICROPIXEL_STATUS_OK) {
        status = CallVoid(network_service, MICROPIXEL_NETWORK_METHOD_CLOSE, &request, sizeof(request));
    }
    if (status != MICROPIXEL_STATUS_OK) return unexpected(ErrorFromStatus(status));
    handle_ = 0U;
    return {};
}

NetworkRequest::NetworkRequest(NetworkRequest&& other) noexcept : handle_(other.handle_) { other.handle_ = 0U; }

NetworkRequest& NetworkRequest::operator=(NetworkRequest&& other) noexcept {
    if (this != &other) {
        Reset();
        handle_ = other.handle_;
        other.handle_ = 0U;
    }
    return *this;
}

NetworkRequest::~NetworkRequest() { Reset(); }

void NetworkRequest::Reset() { (void)Close(); }

}  // namespace micropixel
