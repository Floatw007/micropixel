#include "platform/network/managed_http_client.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_crc.h"
#include "host/time/system_time.hpp"
#include "nvs.h"
#include "psa/crypto.h"

namespace micropixel::platform::network {
namespace {

constexpr char kTag[] = "managed_http";
constexpr char kNvsPartition[] = "runtime_nvs";
constexpr char kCacheNvsPartition[] = "sys_store";
constexpr char kProfileNamespace[] = "terminal";
constexpr char kProfileKey[] = "profile";
constexpr char kCacheNamespace[] = "terminal_cache";
constexpr uint32_t kRequestTimeoutMs = 3000U;
constexpr int64_t kPersistentWriteIntervalSeconds = 5LL * 60LL;
constexpr uint8_t kStopWorker = 0xffU;

struct UploadHeader final {
    uint32_t magic{};
    uint16_t version{};
    uint16_t reserved{};
    uint64_t store_id{};
    uint16_t origin_length{};
    uint16_t ca_length{};
    uint16_t token_length{};
    uint16_t app_id_length{};
};

bool StartsWith(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0U, prefix.size()) == prefix;
}

bool ValidOrigin(std::string_view origin) {
    if (!StartsWith(origin, "https://") || origin.size() <= 8U || origin.back() == '/') return false;
    // A configured origin is a scheme + authority, never a URL path. This
    // prevents path-prefix tricks when the Guest supplies a relative path.
    for (size_t index = 8U; index < origin.size(); ++index) {
        const unsigned char byte = static_cast<unsigned char>(origin[index]);
        if (byte <= 0x20U || byte >= 0x7fU || origin[index] == '/' || origin[index] == '?' ||
            origin[index] == '#' || origin[index] == '@') return false;
    }
    return true;
}

bool ValidAppId(std::string_view app_id) {
    if (app_id.empty()) return false;
    for (const char value : app_id) {
        if (!((value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z') ||
              (value >= '0' && value <= '9') || value == '.' || value == '_' || value == '-')) return false;
    }
    return true;
}

bool PrintableSecret(std::string_view secret) {
    if (secret.empty()) return false;
    for (const unsigned char value : secret) {
        if (value < 0x21U || value > 0x7eU) return false;
    }
    return true;
}

void SecureWipe(void* pointer, size_t size) {
    auto* bytes = static_cast<volatile uint8_t*>(pointer);
    for (size_t index = 0U; index < size; ++index) bytes[index] = 0U;
}

void Hash(std::span<const uint8_t> bytes, std::array<uint8_t, 32U>& digest) {
    size_t digest_size = 0U;
    if (psa_crypto_init() != PSA_SUCCESS ||
        psa_hash_compute(PSA_ALG_SHA_256, bytes.data(), bytes.size(), digest.data(), digest.size(), &digest_size) !=
            PSA_SUCCESS ||
        digest_size != digest.size()) {
        digest.fill(0U);
    }
}

std::array<char, 14U> CacheKey(std::string_view path) {
    std::array<uint8_t, 32U> digest{};
    Hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(path.data()), path.size()), digest);
    std::array<char, 14U> key{};
    key[0] = 'c';
    for (size_t index = 0U; index < 6U; ++index) {
        (void)std::snprintf(key.data() + 1U + index * 2U, 3U, "%02x", digest[index]);
    }
    return key;
}

esp_http_client_method_t HttpMethod(uint8_t method) {
    switch (method) {
        case MICROPIXEL_NETWORK_HTTP_POST:
            return HTTP_METHOD_POST;
        case MICROPIXEL_NETWORK_HTTP_PUT:
            return HTTP_METHOD_PUT;
        case MICROPIXEL_NETWORK_HTTP_PATCH:
            return HTTP_METHOD_PATCH;
        case MICROPIXEL_NETWORK_HTTP_DELETE:
            return HTTP_METHOD_DELETE;
        default:
            return HTTP_METHOD_GET;
    }
}

}  // namespace

ManagedHttpClient::ManagedHttpClient(host::network::Network& network) : network_(network) {
    mutex_ = xSemaphoreCreateMutex();
    queue_ = xQueueCreateStatic(kSlotCount, sizeof(uint8_t), queue_bytes_.data(), &queue_storage_);
    (void)LoadProfile();
    if (mutex_ == nullptr || queue_ == nullptr) return;
    for (size_t index = 0U; index < workers_.size(); ++index) {
        char name[20]{};
        (void)std::snprintf(name, sizeof(name), "managed_http_%u", static_cast<unsigned>(index));
        if (xTaskCreate(WorkerEntry, name, 6144U, this, 4U, &workers_[index]) == pdPASS) ++workers_started_;
    }
    if (workers_started_ != kWorkerCount) ESP_LOGE(kTag, "failed to start both HTTP workers");
}

ManagedHttpClient::~ManagedHttpClient() {
    stopping_.store(true);
    for (size_t index = 0U; index < workers_started_; ++index) (void)xQueueSend(queue_, &kStopWorker, 0U);
    for (uint32_t attempt = 0U; attempt < 200U && workers_exited_.load() < workers_started_; ++attempt) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    for (RequestSlot& slot : slots_) ReleaseSlot(slot);
    for (MemoryCache& cache : memory_cache_) {
        if (cache.bytes != nullptr) heap_caps_free(cache.bytes);
    }
    if (queue_ != nullptr) vQueueDelete(queue_);
    if (mutex_ != nullptr) vSemaphoreDelete(mutex_);
    SecureWipe(&profile_, sizeof(profile_));
}

bool ManagedHttpClient::TakeLock() const {
    return mutex_ != nullptr && xSemaphoreTake(mutex_, portMAX_DELAY) == pdTRUE;
}

void ManagedHttpClient::GiveLock() const { (void)xSemaphoreGive(mutex_); }

bool ManagedHttpClient::SessionMatches(uint32_t session) const {
    return session != 0U && session == active_session_;
}

int32_t ManagedHttpClient::OpenSession(std::string_view app_id, device::ManagedNetworkCompletionSink sink,
                                       void* context, uint32_t& session_out) {
    session_out = 0U;
    if (!valid() || app_id.empty() || app_id.size() > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH || sink == nullptr ||
        context == nullptr || !TakeLock()) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (active_session_ != 0U) {
        GiveLock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    ++session_generation_;
    if (session_generation_ == 0U) ++session_generation_;
    active_session_ = session_generation_;
    completion_sink_ = sink;
    completion_context_ = context;
    std::memcpy(session_app_id_.data(), app_id.data(), app_id.size());
    session_app_id_[app_id.size()] = '\0';
    session_out = active_session_;
    GiveLock();
    return MICROPIXEL_STATUS_OK;
}

void ManagedHttpClient::CloseSession(uint32_t session) {
    if (!TakeLock()) return;
    if (!SessionMatches(session)) {
        GiveLock();
        return;
    }
    completion_sink_ = nullptr;
    completion_context_ = nullptr;
    active_session_ = 0U;
    session_app_id_.fill('\0');
    for (RequestSlot& slot : slots_) {
        if (slot.session != session || slot.state == SlotState::kFree) continue;
        slot.close_requested = true;
        slot.cancelled.store(true);
        if (slot.state == SlotState::kComplete) ReleaseSlot(slot);
    }
    GiveLock();
}

int32_t ManagedHttpClient::GetInfo(uint32_t session, device::ManagedNetworkInfo& info_out) const {
    if (!TakeLock()) return MICROPIXEL_STATUS_INTERNAL;
    if (!SessionMatches(session)) {
        GiveLock();
        return MICROPIXEL_STATUS_CLOSED;
    }
    host::network::NetworkSnapshot snapshot{};
    network_.CopySnapshot(snapshot);
    const std::time_t now = std::time(nullptr);
    info_out = {
        .configured = profile_.magic == kProfileMagic,
        .route_online = snapshot.connected && snapshot.route.transport != device::NetworkTransport::kNone,
        .time_synchronized = firmware::system_time::IsTrustedUtcTime(now),
        .profile_revision = profile_.revision,
        .capabilities = MICROPIXEL_NETWORK_CAP_HTTPS | MICROPIXEL_NETWORK_CAP_AUTHENTICATED_PROFILE |
                        MICROPIXEL_NETWORK_CAP_MEMORY_CACHE | MICROPIXEL_NETWORK_CAP_PERSISTENT_CACHE,
        .max_active_requests = 2U,
        .max_queued_requests = 4U,
    };
    GiveLock();
    return MICROPIXEL_STATUS_OK;
}

bool ManagedHttpClient::AnyRequestsLocked() const {
    return std::any_of(slots_.begin(), slots_.end(), [](const RequestSlot& slot) {
        return slot.state != SlotState::kFree;
    });
}

int32_t ManagedHttpClient::Start(uint32_t session, const device::ManagedNetworkRequest& request,
                                 uint32_t& handle_out) {
    handle_out = 0U;
    if (!valid() || request.path.empty() || request.path.size() > MICROPIXEL_NETWORK_MAX_PATH_BYTES ||
        request.body.size() > MICROPIXEL_NETWORK_MAX_BODY_BYTES ||
        request.idempotency_key.size() > MICROPIXEL_NETWORK_MAX_IDEMPOTENCY_KEY_BYTES || !TakeLock()) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (!SessionMatches(session)) {
        GiveLock();
        return MICROPIXEL_STATUS_CLOSED;
    }
    if (profile_.magic != kProfileMagic) {
        GiveLock();
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (std::strcmp(session_app_id_.data(), profile_.allowed_app_id.data()) != 0) {
        GiveLock();
        return MICROPIXEL_STATUS_PERMISSION_DENIED;
    }
    uint32_t queued = 0U;
    for (const RequestSlot& slot : slots_) {
        if (slot.state == SlotState::kQueued) ++queued;
        if (request.method == MICROPIXEL_NETWORK_HTTP_GET && slot.state != SlotState::kFree &&
            std::string_view(slot.path.data(), slot.path_length) == request.path) {
            GiveLock();
            return MICROPIXEL_STATUS_RATE_LIMITED;
        }
    }
    if (queued >= 4U) {
        GiveLock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    size_t index = slots_.size();
    for (size_t candidate = 0U; candidate < slots_.size(); ++candidate) {
        if (slots_[candidate].state == SlotState::kFree) {
            index = candidate;
            break;
        }
    }
    if (index == slots_.size()) {
        GiveLock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    RequestSlot& slot = slots_[index];
    ++slot.generation;
    if (slot.generation == 0U) ++slot.generation;
    slot.handle = (slot.generation << 8U) | static_cast<uint32_t>(index + 1U);
    slot.session = session;
    slot.method = request.method;
    slot.cache_mode = request.cache_mode;
    slot.path_length = static_cast<uint16_t>(request.path.size());
    slot.key_length = static_cast<uint16_t>(request.idempotency_key.size());
    slot.body_length = request.body.size();
    std::memcpy(slot.path.data(), request.path.data(), request.path.size());
    slot.path[request.path.size()] = '\0';
    if (!request.body.empty()) std::memcpy(slot.body.data(), request.body.data(), request.body.size());
    if (!request.idempotency_key.empty()) {
        std::memcpy(slot.idempotency_key.data(), request.idempotency_key.data(), request.idempotency_key.size());
    }
    slot.idempotency_key[request.idempotency_key.size()] = '\0';
    slot.response = static_cast<uint8_t*>(
        heap_caps_malloc(MICROPIXEL_NETWORK_MAX_RESPONSE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (slot.response == nullptr) {
        ReleaseSlot(slot);
        GiveLock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    slot.cancelled.store(false);
    slot.close_requested = false;
    slot.state = SlotState::kQueued;
    const uint8_t queued_index = static_cast<uint8_t>(index);
    if (xQueueSend(queue_, &queued_index, 0U) != pdTRUE) {
        ReleaseSlot(slot);
        GiveLock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    handle_out = slot.handle;
    GiveLock();
    return MICROPIXEL_STATUS_OK;
}

ManagedHttpClient::RequestSlot* ManagedHttpClient::Find(uint32_t session, uint32_t handle) {
    const uint32_t encoded = handle & 0xffU;
    if (encoded == 0U || encoded > slots_.size()) return nullptr;
    RequestSlot& slot = slots_[encoded - 1U];
    return slot.handle == handle && slot.session == session && slot.state != SlotState::kFree ? &slot : nullptr;
}

const ManagedHttpClient::RequestSlot* ManagedHttpClient::Find(uint32_t session, uint32_t handle) const {
    return const_cast<ManagedHttpClient*>(this)->Find(session, handle);
}

int32_t ManagedHttpClient::Read(uint32_t session, uint32_t handle, uint32_t offset,
                                std::span<uint8_t> destination, uint32_t& length_out,
                                uint32_t& total_length_out) const {
    length_out = 0U;
    total_length_out = 0U;
    if (destination.empty() || !TakeLock()) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    const RequestSlot* slot = Find(session, handle);
    if (slot == nullptr) {
        GiveLock();
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (slot->state != SlotState::kComplete) {
        GiveLock();
        return MICROPIXEL_STATUS_WOULD_BLOCK;
    }
    if (slot->transport_status != MICROPIXEL_STATUS_OK) {
        const int32_t status = slot->transport_status;
        GiveLock();
        return status;
    }
    if (offset > slot->response_length) {
        GiveLock();
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    length_out = std::min<uint32_t>(destination.size(), slot->response_length - offset);
    total_length_out = slot->response_length;
    if (length_out != 0U) std::memcpy(destination.data(), slot->response + offset, length_out);
    GiveLock();
    return MICROPIXEL_STATUS_OK;
}

int32_t ManagedHttpClient::Cancel(uint32_t session, uint32_t handle) {
    if (!TakeLock()) return MICROPIXEL_STATUS_INTERNAL;
    RequestSlot* slot = Find(session, handle);
    if (slot == nullptr) {
        GiveLock();
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (slot->state == SlotState::kComplete) {
        GiveLock();
        return MICROPIXEL_STATUS_OK;
    }
    slot->cancelled.store(true);
    GiveLock();
    return MICROPIXEL_STATUS_OK;
}

int32_t ManagedHttpClient::Close(uint32_t session, uint32_t handle) {
    if (!TakeLock()) return MICROPIXEL_STATUS_INTERNAL;
    RequestSlot* slot = Find(session, handle);
    if (slot == nullptr) {
        GiveLock();
        return MICROPIXEL_STATUS_NOT_FOUND;
    }
    if (slot->state == SlotState::kComplete) {
        ReleaseSlot(*slot);
    } else {
        slot->close_requested = true;
        slot->cancelled.store(true);
    }
    GiveLock();
    return MICROPIXEL_STATUS_OK;
}

void ManagedHttpClient::ReleaseSlot(RequestSlot& slot) {
    if (slot.response != nullptr) heap_caps_free(slot.response);
    const uint32_t generation = slot.generation;
    slot.~RequestSlot();
    new (&slot) RequestSlot{};
    slot.generation = generation;
}

void ManagedHttpClient::WorkerEntry(void* context) {
    static_cast<ManagedHttpClient*>(context)->WorkerLoop();
}

void ManagedHttpClient::WorkerLoop() {
    while (true) {
        uint8_t index = kStopWorker;
        if (xQueueReceive(queue_, &index, portMAX_DELAY) != pdTRUE) continue;
        if (index == kStopWorker || stopping_.load()) break;
        if (index < slots_.size()) Execute(index);
    }
    ++workers_exited_;
    vTaskDelete(nullptr);
}

void ManagedHttpClient::Execute(size_t slot_index) {
    if (!TakeLock()) return;
    RequestSlot& slot = slots_[slot_index];
    if (slot.state != SlotState::kQueued) {
        GiveLock();
        return;
    }
    slot.state = SlotState::kActive;
    GiveLock();

    int32_t status = slot.cancelled.load() ? static_cast<int32_t>(MICROPIXEL_STATUS_CANCELLED) : PerformHttp(slot);
    // Server failures are transport-successful, but a GET may still serve its
    // last known good snapshot. Authentication and other 4xx responses always
    // reach the Guest unchanged.
    if (status == MICROPIXEL_STATUS_OK && slot.method == MICROPIXEL_NETWORK_HTTP_GET &&
        slot.http_status >= 500U) status = MICROPIXEL_STATUS_INTERNAL;
    if (status != MICROPIXEL_STATUS_OK && !slot.cancelled.load() && slot.method == MICROPIXEL_NETWORK_HTTP_GET &&
        slot.cache_mode != MICROPIXEL_NETWORK_CACHE_NONE && UseFallbackCache(slot)) {
        status = MICROPIXEL_STATUS_OK;
    }
    if (status == MICROPIXEL_STATUS_OK && slot.response_source == MICROPIXEL_NETWORK_RESPONSE_LIVE &&
        slot.method == MICROPIXEL_NETWORK_HTTP_GET && slot.http_status >= 200U && slot.http_status < 300U) {
        StoreSuccessfulCache(slot);
    }

    if (!TakeLock()) return;
    slot.transport_status = slot.cancelled.load() ? static_cast<int32_t>(MICROPIXEL_STATUS_CANCELLED) : status;
    slot.state = SlotState::kComplete;
    const bool deliver = !slot.close_requested && SessionMatches(slot.session) && completion_sink_ != nullptr;
    device::ManagedNetworkCompletionSink sink = deliver ? completion_sink_ : nullptr;
    void* context = deliver ? completion_context_ : nullptr;
    const device::ManagedNetworkCompletion completion{
        .request_handle = slot.handle,
        .transport_status = slot.transport_status,
        .http_status = slot.http_status,
        .response_length = slot.response_length,
        .cache_age_seconds = slot.cache_age_seconds,
        .source = slot.response_source,
    };
    if (slot.close_requested || !SessionMatches(slot.session)) ReleaseSlot(slot);
    if (sink != nullptr) sink(context, completion);
    GiveLock();
}

esp_err_t ManagedHttpClient::HttpEvent(esp_http_client_event_t* event) {
    auto* slot = static_cast<RequestSlot*>(event->user_data);
    if (slot == nullptr || slot->cancelled.load()) return ESP_FAIL;
    if (event->event_id == HTTP_EVENT_ON_DATA && event->data_len > 0) {
        const uint32_t length = static_cast<uint32_t>(event->data_len);
        if (length > MICROPIXEL_NETWORK_MAX_RESPONSE_BYTES - slot->receive_offset) {
            slot->response_too_large = true;
            return ESP_FAIL;
        }
        std::memcpy(slot->response + slot->receive_offset, event->data, length);
        slot->receive_offset += length;
    }
    return ESP_OK;
}

int32_t ManagedHttpClient::PerformHttp(RequestSlot& slot) {
    host::network::NetworkSnapshot snapshot{};
    network_.CopySnapshot(snapshot);
    if (!snapshot.connected || snapshot.route.transport == device::NetworkTransport::kNone) {
        return MICROPIXEL_STATUS_INTERNAL;
    }
    const std::time_t now = std::time(nullptr);
    if (!firmware::system_time::IsTrustedUtcTime(now)) return MICROPIXEL_STATUS_STALE_STATE;

    const int url_size = std::snprintf(slot.url.data(), slot.url.size(), "%s%s", profile_.origin.data(), slot.path.data());
    if (url_size <= 0 || static_cast<size_t>(url_size) >= slot.url.size()) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    esp_http_client_config_t config{};
    config.url = slot.url.data();
    config.cert_pem = profile_.ca_pem.data();
    config.timeout_ms = kRequestTimeoutMs;
    config.keep_alive_enable = true;
    config.event_handler = HttpEvent;
    config.user_data = &slot;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    (void)esp_http_client_set_method(client, HttpMethod(slot.method));
    (void)esp_http_client_set_header(client, "Accept", "application/json");
    (void)std::snprintf(slot.authorization.data(), slot.authorization.size(), "Bearer %s",
                        profile_.bearer_token.data());
    (void)esp_http_client_set_header(client, "Authorization", slot.authorization.data());
    if (slot.key_length != 0U) {
        (void)esp_http_client_set_header(client, "Idempotency-Key", slot.idempotency_key.data());
    }
    if (slot.body_length != 0U) {
        (void)esp_http_client_set_header(client, "Content-Type", "application/json");
        (void)esp_http_client_set_post_field(client, reinterpret_cast<const char*>(slot.body.data()),
                                             slot.body_length);
    }
    slot.receive_offset = 0U;
    slot.response_too_large = false;
    slot.response_source = MICROPIXEL_NETWORK_RESPONSE_LIVE;
    const esp_err_t error = esp_http_client_perform(client);
    if (error == ESP_OK) slot.http_status = static_cast<uint16_t>(esp_http_client_get_status_code(client));
    esp_http_client_cleanup(client);
    slot.authorization.fill('\0');
    if (slot.cancelled.load()) return MICROPIXEL_STATUS_CANCELLED;
    if (slot.response_too_large) return MICROPIXEL_STATUS_BUFFER_TOO_SMALL;
    if (error != ESP_OK) return error == ESP_ERR_TIMEOUT ? MICROPIXEL_STATUS_TIMEOUT : MICROPIXEL_STATUS_INTERNAL;
    slot.response_length = slot.receive_offset;
    return MICROPIXEL_STATUS_OK;
}

bool ManagedHttpClient::UseFallbackCache(RequestSlot& slot) {
    return LoadMemoryCache(std::string_view(slot.path.data(), slot.path_length), slot) ||
           (slot.cache_mode == MICROPIXEL_NETWORK_CACHE_PERSISTENT_FALLBACK &&
            LoadPersistentCache(std::string_view(slot.path.data(), slot.path_length), slot));
}

void ManagedHttpClient::StoreSuccessfulCache(const RequestSlot& slot) {
    const std::string_view path(slot.path.data(), slot.path_length);
    const std::span<const uint8_t> bytes(slot.response, slot.response_length);
    StoreMemoryCache(path, bytes, slot.http_status);
    if (slot.cache_mode == MICROPIXEL_NETWORK_CACHE_PERSISTENT_FALLBACK) {
        StorePersistentCache(path, bytes, slot.http_status);
    }
}

bool ManagedHttpClient::LoadMemoryCache(std::string_view path, RequestSlot& slot) {
    if (!TakeLock()) return false;
    MemoryCache* found = nullptr;
    for (MemoryCache& cache : memory_cache_) {
        if (cache.bytes != nullptr && path == cache.path.data()) {
            found = &cache;
            break;
        }
    }
    if (found == nullptr || found->length > MICROPIXEL_NETWORK_MAX_RESPONSE_BYTES) {
        GiveLock();
        return false;
    }
    std::memcpy(slot.response, found->bytes, found->length);
    slot.response_length = found->length;
    slot.http_status = found->http_status;
    slot.response_source = MICROPIXEL_NETWORK_RESPONSE_MEMORY_CACHE;
    const int64_t now = std::time(nullptr);
    slot.cache_age_seconds = now > found->stored_at_seconds ? static_cast<uint32_t>(now - found->stored_at_seconds) : 0U;
    found->use_sequence = ++cache_use_sequence_;
    GiveLock();
    return true;
}

void ManagedHttpClient::StoreMemoryCache(std::string_view path, std::span<const uint8_t> bytes,
                                         uint16_t http_status) {
    if (!TakeLock()) return;
    MemoryCache* destination = nullptr;
    for (MemoryCache& cache : memory_cache_) {
        if (cache.bytes != nullptr && path == cache.path.data()) {
            destination = &cache;
            break;
        }
        if (destination == nullptr && cache.bytes == nullptr) destination = &cache;
    }
    if (destination == nullptr) {
        destination = &*std::min_element(memory_cache_.begin(), memory_cache_.end(),
                                         [](const MemoryCache& left, const MemoryCache& right) {
                                             return left.use_sequence < right.use_sequence;
                                         });
    }
    uint8_t* replacement = nullptr;
    if (!bytes.empty()) {
        replacement = static_cast<uint8_t*>(heap_caps_malloc(bytes.size(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (replacement == nullptr) {
            GiveLock();
            return;
        }
        std::memcpy(replacement, bytes.data(), bytes.size());
    }
    if (destination->bytes != nullptr) heap_caps_free(destination->bytes);
    *destination = {};
    std::memcpy(destination->path.data(), path.data(), path.size());
    destination->path[path.size()] = '\0';
    destination->bytes = replacement;
    destination->length = bytes.size();
    destination->http_status = http_status;
    destination->stored_at_seconds = std::time(nullptr);
    destination->use_sequence = ++cache_use_sequence_;
    GiveLock();
}

bool ManagedHttpClient::LoadPersistentCache(std::string_view path, RequestSlot& slot) {
    const auto key = CacheKey(path);
    nvs_handle_t handle{};
    if (nvs_open_from_partition(kCacheNvsPartition, kCacheNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = 0U;
    esp_err_t error = nvs_get_blob(handle, key.data(), nullptr, &size);
    if (error != ESP_OK || size < sizeof(PersistedCacheHeader) ||
        size > sizeof(PersistedCacheHeader) + MICROPIXEL_NETWORK_MAX_RESPONSE_BYTES) {
        nvs_close(handle);
        return false;
    }
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) {
        nvs_close(handle);
        return false;
    }
    error = nvs_get_blob(handle, key.data(), buffer, &size);
    nvs_close(handle);
    PersistedCacheHeader header{};
    std::memcpy(&header, buffer, sizeof(header));
    const auto body = std::span<const uint8_t>(buffer + sizeof(header), size - sizeof(header));
    const bool valid = error == ESP_OK && header.magic == kCacheMagic && header.version == 1U &&
                       header.body_length == body.size() &&
                       header.body_crc32 == esp_rom_crc32_le(0U, body.data(), body.size());
    if (valid) {
        std::memcpy(slot.response, body.data(), body.size());
        slot.response_length = body.size();
        slot.http_status = header.http_status;
        slot.response_source = MICROPIXEL_NETWORK_RESPONSE_PERSISTENT_CACHE;
        const int64_t now = std::time(nullptr);
        slot.cache_age_seconds = now > header.stored_at_seconds ? static_cast<uint32_t>(now - header.stored_at_seconds)
                                                                : 0U;
    }
    heap_caps_free(buffer);
    if (valid) StoreMemoryCache(path, std::span<const uint8_t>(slot.response, slot.response_length), slot.http_status);
    return valid;
}

void ManagedHttpClient::StorePersistentCache(std::string_view path, std::span<const uint8_t> bytes,
                                             uint16_t http_status) {
    const auto key = CacheKey(path);
    const size_t total = sizeof(PersistedCacheHeader) + bytes.size();
    auto* buffer = static_cast<uint8_t*>(heap_caps_malloc(total, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (buffer == nullptr) return;
    PersistedCacheHeader previous{};
    bool has_previous = false;
    bool content_changed = true;
    bool old_enough = true;
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kCacheNvsPartition, kCacheNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) {
        size_t previous_size = 0U;
        if (nvs_get_blob(handle, key.data(), nullptr, &previous_size) == ESP_OK &&
            previous_size >= sizeof(PersistedCacheHeader)) {
            auto* old = static_cast<uint8_t*>(heap_caps_malloc(previous_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (old != nullptr && nvs_get_blob(handle, key.data(), old, &previous_size) == ESP_OK) {
                std::memcpy(&previous, old, sizeof(previous));
                const int64_t now = std::time(nullptr);
                const auto old_body = std::span<const uint8_t>(old + sizeof(previous), previous_size - sizeof(previous));
                has_previous = previous.magic == kCacheMagic && previous.version == 1U &&
                               previous.body_length == old_body.size() &&
                               previous.body_crc32 == esp_rom_crc32_le(0U, old_body.data(), old_body.size());
                content_changed = !has_previous || previous.http_status != http_status ||
                                  previous.body_length != bytes.size() ||
                                  previous.body_crc32 != esp_rom_crc32_le(0U, bytes.data(), bytes.size());
                old_enough = !has_previous || now - previous.stored_at_seconds >= kPersistentWriteIntervalSeconds;
            }
            if (old != nullptr) heap_caps_free(old);
        }
        if (!has_previous || (content_changed && old_enough)) {
            const PersistedCacheHeader header{
                .magic = kCacheMagic,
                .version = 1U,
                .http_status = http_status,
                .stored_at_seconds = std::time(nullptr),
                .body_length = static_cast<uint32_t>(bytes.size()),
                .body_crc32 = esp_rom_crc32_le(0U, bytes.data(), bytes.size()),
            };
            std::memcpy(buffer, &header, sizeof(header));
            if (!bytes.empty()) std::memcpy(buffer + sizeof(header), bytes.data(), bytes.size());
            error = nvs_set_blob(handle, key.data(), buffer, total);
            if (error == ESP_OK) error = nvs_commit(handle);
            if (error != ESP_OK) ESP_LOGW(kTag, "persistent response cache write failed: %s", esp_err_to_name(error));
        }
        nvs_close(handle);
    }
    heap_caps_free(buffer);
}

bool ManagedHttpClient::LoadProfile() {
    nvs_handle_t handle{};
    if (nvs_open_from_partition(kNvsPartition, kProfileNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(profile_);
    const esp_err_t error = nvs_get_blob(handle, kProfileKey, &profile_, &size);
    nvs_close(handle);
    const bool lengths_valid = profile_.origin_length > 0U && profile_.origin_length < profile_.origin.size() &&
                               profile_.ca_length > 0U && profile_.ca_length < profile_.ca_pem.size() &&
                               profile_.token_length > 0U && profile_.token_length < profile_.bearer_token.size() &&
                               profile_.app_id_length > 0U && profile_.app_id_length < profile_.allowed_app_id.size();
    const bool terminators_valid = lengths_valid && profile_.origin[profile_.origin_length] == '\0' &&
                                   profile_.ca_pem[profile_.ca_length] == '\0' &&
                                   profile_.bearer_token[profile_.token_length] == '\0' &&
                                   profile_.allowed_app_id[profile_.app_id_length] == '\0';
    if (error != ESP_OK || size != sizeof(profile_) || profile_.magic != kProfileMagic ||
        profile_.version != kProfileVersion || profile_.size != sizeof(profile_) || !terminators_valid ||
        !ValidOrigin({profile_.origin.data(), profile_.origin_length}) ||
        !ValidAppId({profile_.allowed_app_id.data(), profile_.app_id_length}) ||
        !PrintableSecret({profile_.bearer_token.data(), profile_.token_length})) {
        SecureWipe(&profile_, sizeof(profile_));
        return false;
    }
    return true;
}

bool ManagedHttpClient::StoreProfile(const ProfileRecord& profile) {
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kNvsPartition, kProfileNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) error = nvs_set_blob(handle, kProfileKey, &profile, sizeof(profile));
    if (error == ESP_OK) error = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    return error == ESP_OK;
}

int32_t ManagedHttpClient::Configure(std::span<const uint8_t> blob) {
    UploadHeader header{};
    if (blob.size() < sizeof(header)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    std::memcpy(&header, blob.data(), sizeof(header));
    const size_t payload_size = static_cast<size_t>(header.origin_length) + header.ca_length + header.token_length +
                                header.app_id_length;
    if (header.magic != kProfileMagic || header.version != kProfileVersion || header.reserved != 0U ||
        header.origin_length == 0U || header.origin_length > 256U || header.ca_length == 0U ||
        header.ca_length > 4096U || header.token_length == 0U || header.token_length > 1024U ||
        header.app_id_length == 0U || header.app_id_length > MICROPIXEL_BUNDLE_APP_ID_MAX_LENGTH ||
        blob.size() != sizeof(header) + payload_size) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    const uint8_t* cursor = blob.data() + sizeof(header);
    const std::string_view origin(reinterpret_cast<const char*>(cursor), header.origin_length);
    const uint8_t* ca_start = cursor + header.origin_length;
    const uint8_t* token_start = ca_start + header.ca_length;
    const uint8_t* app_id_start = token_start + header.token_length;
    const std::string_view ca(reinterpret_cast<const char*>(ca_start), header.ca_length);
    const std::string_view token(reinterpret_cast<const char*>(token_start), header.token_length);
    const std::string_view app_id(reinterpret_cast<const char*>(app_id_start), header.app_id_length);
    if (!ValidOrigin(origin) || ca.find("-----BEGIN CERTIFICATE-----") == std::string_view::npos ||
        !PrintableSecret(token) || !ValidAppId(app_id)) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    if (!TakeLock()) return MICROPIXEL_STATUS_INTERNAL;
    if (AnyRequestsLocked()) {
        GiveLock();
        return MICROPIXEL_STATUS_WOULD_BLOCK;
    }
    auto* candidate = static_cast<ProfileRecord*>(
        heap_caps_calloc(1U, sizeof(ProfileRecord), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (candidate == nullptr) {
        GiveLock();
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    candidate->magic = kProfileMagic;
    candidate->version = kProfileVersion;
    candidate->size = sizeof(*candidate);
    candidate->revision = profile_.revision + 1U;
    if (candidate->revision == 0U) candidate->revision = 1U;
    candidate->store_id = header.store_id;
    candidate->origin_length = header.origin_length;
    candidate->ca_length = header.ca_length;
    candidate->token_length = header.token_length;
    candidate->app_id_length = header.app_id_length;
    std::memcpy(candidate->origin.data(), cursor, header.origin_length);
    cursor += header.origin_length;
    std::memcpy(candidate->ca_pem.data(), cursor, header.ca_length);
    cursor += header.ca_length;
    std::memcpy(candidate->bearer_token.data(), cursor, header.token_length);
    cursor += header.token_length;
    std::memcpy(candidate->allowed_app_id.data(), cursor, header.app_id_length);
    Hash(std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(candidate->ca_pem.data()), header.ca_length),
         candidate->certificate_sha256);
    const bool stored = StoreProfile(*candidate);
    if (stored) profile_ = *candidate;
    // Wipe credentials through a volatile byte view so the compiler cannot
    // elide the clear, while avoiding class-memaccess on ProfileRecord.
    SecureWipe(candidate, sizeof(*candidate));
    heap_caps_free(candidate);
    GiveLock();
    return stored ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t ManagedHttpClient::ClearProfile() {
    if (!TakeLock()) return MICROPIXEL_STATUS_INTERNAL;
    if (AnyRequestsLocked()) {
        GiveLock();
        return MICROPIXEL_STATUS_WOULD_BLOCK;
    }
    nvs_handle_t handle{};
    esp_err_t error = nvs_open_from_partition(kNvsPartition, kProfileNamespace, NVS_READWRITE, &handle);
    if (error == ESP_OK) error = nvs_erase_key(handle, kProfileKey);
    if (error == ESP_ERR_NVS_NOT_FOUND) error = ESP_OK;
    if (error == ESP_OK) error = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    if (error == ESP_OK) SecureWipe(&profile_, sizeof(profile_));
    GiveLock();
    return error == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

TerminalProfileStatus ManagedHttpClient::ProfileStatus() const {
    TerminalProfileStatus status{};
    if (!TakeLock()) return status;
    status.configured = profile_.magic == kProfileMagic;
    status.revision = profile_.revision;
    status.store_id = profile_.store_id;
    status.origin = profile_.origin;
    status.allowed_app_id = profile_.allowed_app_id;
    if (status.configured) {
        for (size_t index = 0U; index < 8U; ++index) {
            (void)std::snprintf(status.certificate_fingerprint.data() + index * 2U, 3U, "%02x",
                                profile_.certificate_sha256[index]);
        }
        const size_t token_length = std::strlen(profile_.bearer_token.data());
        const size_t suffix = std::min<size_t>(8U, token_length);
        std::memcpy(status.token_suffix.data(), profile_.bearer_token.data() + token_length - suffix, suffix);
    }
    GiveLock();
    return status;
}

}  // namespace micropixel::platform::network
