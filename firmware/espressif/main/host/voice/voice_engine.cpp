#include "host/voice/voice_engine.hpp"

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "cJSON.h"
#include "esp_afe_config.h"
#include "esp_afe_sr_models.h"
#include "esp_log.h"
#include "esp_mn_models.h"
#include "esp_mn_speech_commands.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs.h"
#include "opus.h"
#include "psa/crypto.h"
#include "work/task_policy.hpp"

namespace micropixel::host::voice {
namespace {

constexpr char kTag[] = "micropixel_voice";
constexpr char kNvsPartition[] = "nvs";
constexpr char kNvsNamespace[] = "voice";
constexpr char kNvsKey[] = "profile";
constexpr char kPreferencesKey[] = "settings";
constexpr uint32_t kPreferencesMagic = 0x31505642U;  // BVP1
constexpr uint16_t kPreferencesVersion = 1U;
constexpr uint8_t kProtocolVersion = 2U;
constexpr uint8_t kInputAudioKind = 1U;
constexpr uint8_t kOutputAudioKind = 2U;
constexpr uint8_t kWebsocketTextOpcode = 0x1U;
constexpr uint8_t kWebsocketBinaryOpcode = 0x2U;
constexpr size_t kAudioHeaderSize = 28U;
constexpr uint32_t kSilenceCommitFrames = 35U;
constexpr uint32_t kNoSpeechCaptureFrames = 300U;
constexpr uint32_t kMaximumCaptureFrames = 750U;
constexpr uint32_t kConnectTimeoutMs = 15000U;
constexpr uint32_t kSendTimeoutMs = 5000U;
constexpr uint32_t kAudioQueueWaitMs = 40U;
constexpr uint32_t kAudioFlushTimeoutMs = 30000U;
constexpr uint32_t kAudioSenderStackBytes = 6144U;
constexpr int kEncoderComplexity = 3;
// Buffered capture and queued encoding can both stay runnable on the system
// core. A real blocked interval (not taskYIELD) lets display and idle run.
constexpr TickType_t kVoiceWorkPauseTicks = pdMS_TO_TICKS(2) + 1U;
constexpr uint64_t kSpeechFeedSummaryIntervalUs = 8'000'000U;
constexpr uint32_t kSpeechFeedWarmupChunks = 8U;
constexpr float kLocalCommandThreshold = 0.90F;
constexpr int kLocalCommandWindowMs = 6000;
constexpr int8_t kLocalVolumeStepPercent = 10;

struct StoredPreferences final {
    uint32_t magic{};
    uint16_t version{};
    uint16_t size{};
    micropixel_voice_preferences_t value{};
};

void Store32(uint8_t* out, uint32_t value) {
    for (uint32_t index = 0U; index < 4U; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}
void Store64(uint8_t* out, uint64_t value) {
    for (uint32_t index = 0U; index < 8U; ++index) out[index] = static_cast<uint8_t>(value >> (index * 8U));
}
uint32_t Load32(const uint8_t* in) {
    return static_cast<uint32_t>(in[0]) | (static_cast<uint32_t>(in[1]) << 8U) | (static_cast<uint32_t>(in[2]) << 16U) |
           (static_cast<uint32_t>(in[3]) << 24U);
}
bool StartsWith(const char* text, const char* prefix) { return std::strncmp(text, prefix, std::strlen(prefix)) == 0; }

}  // namespace

VoiceEngine::VoiceEngine(device::Audio& audio, device::AudioInput& input, network::Network& network)
    : audio_(audio), input_(input), network_(network) {
    audio_queue_ = xQueueCreateStatic(kAudioQueueCapacity, sizeof(QueuedAudioFrame), audio_queue_storage_.data(),
                                      &audio_queue_state_);
    if (audio_queue_ == nullptr) {
        mutex_ready_ = false;
        ESP_LOGE(kTag, "unable to create voice audio queue");
        return;
    }
    preferences_.size = sizeof(preferences_);
    preferences_.response_gain_per_mille = 300U;
    preferences_.local_commands_enabled = 1U;
    (void)LoadProfile();
    LoadPreferences();
    if (!InitializeSpeechRecognition()) ESP_LOGW(kTag, "ESP-SR wake listener is unavailable");
}

VoiceEngine::~VoiceEngine() {
    (void)CancelSession();
    for (uint32_t index = 0U; task_running_.load(std::memory_order_acquire) && index < 100U; ++index) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    DestroyTransport();
    ShutdownSpeechRecognition();
    if (audio_queue_ != nullptr) vQueueDelete(audio_queue_);
}

bool VoiceEngine::InitializeSpeechRecognition() {
    speech_models_ = esp_srmodel_init("model");
    if (speech_models_ == nullptr || speech_models_->num == 0) return false;
    afe_config_t* config = afe_config_init("M", speech_models_, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (config == nullptr) return false;
    config->aec_init = false;
    config->se_init = false;
    config->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
    // CPU1 is reserved for the foreground Guest. Keep the continuous AFE
    // pipeline on the Host core so wake-word inference cannot starve animation.
    config->afe_perferred_core = 0;
    config->afe_perferred_priority = task_policy::kHostPriority;
    config->vad_min_speech_ms = 128;
    config->vad_min_noise_ms = 600;
    config->vad_delay_ms = 400;
    afe_handle_ = esp_afe_handle_from_config(config);
    afe_data_ = afe_handle_ == nullptr ? nullptr : afe_handle_->create_from_config(config);
    afe_config_free(config);
    if (afe_data_ == nullptr) return false;
    const int feed_frames = afe_handle_->get_feed_chunksize(afe_data_);
    if (feed_frames <= 0 || static_cast<uint32_t>(feed_frames) > speech_feed_workspace_.size()) {
        afe_handle_->destroy(afe_data_);
        afe_data_ = nullptr;
        return false;
    }
    speech_feed_frames_ = static_cast<uint32_t>(feed_frames);
    if (!InitializeLocalCommands()) ESP_LOGW(kTag, "MultiNet local commands are unavailable");
    if (afe_handle_->set_vadnet_threshold != nullptr) (void)afe_handle_->set_vadnet_threshold(afe_data_, 0.5F);
    if (afe_handle_->set_vad_energy_threshold != nullptr) {
        (void)afe_handle_->set_vad_energy_threshold(afe_data_, -60.0F);
    }
    ESP_LOGI(kTag, "session reuses AFE VADNet: frames=%" PRIu32 " silence=700ms", speech_feed_frames_);
    speech_feed_generation_.store(0U, std::memory_order_release);
    speech_feed_healthy_.store(false, std::memory_order_release);
    speech_running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(SpeechFeedTaskEntry, "voice_sr_feed", 6144, this, task_policy::kAudioPriority, nullptr,
                                task_policy::kSystemCore) != pdPASS ||
        xTaskCreatePinnedToCore(SpeechDetectTaskEntry, "voice_sr_detect", 6144, this, task_policy::kDisplayPriority,
                                nullptr, task_policy::kSystemCore) != pdPASS) {
        speech_running_.store(false, std::memory_order_release);
        return false;
    }
    ESP_LOGI(kTag, "ESP-SR ready: models=%d feed=%" PRIu32 " wake=Hi ESP local_commands=%s preroll=400ms",
             speech_models_->num, speech_feed_frames_, multinet_data_ == nullptr ? "no" : "yes");
    return true;
}

bool VoiceEngine::InitializeLocalCommands() {
    char* model_name = esp_srmodel_filter(speech_models_, ESP_MN_PREFIX, ESP_MN_CHINESE);
    if (model_name == nullptr) return false;
    multinet_handle_ = esp_mn_handle_from_name(model_name);
    if (multinet_handle_ == nullptr) return false;
    multinet_data_ = multinet_handle_->create(model_name, kLocalCommandWindowMs);
    if (multinet_data_ == nullptr) {
        multinet_handle_ = nullptr;
        return false;
    }
    const int chunk_frames = multinet_handle_->get_samp_chunksize(multinet_data_);
    if (multinet_handle_->get_samp_rate(multinet_data_) != static_cast<int>(kSampleRate) || chunk_frames <= 0 ||
        static_cast<uint32_t>(chunk_frames) > multinet_workspace_.size() ||
        multinet_handle_->set_det_threshold(multinet_data_, kLocalCommandThreshold) != 0 ||
        esp_mn_commands_alloc(multinet_handle_, multinet_data_) != ESP_OK ||
        // Chinese MultiNet7 consumes space-separated, tone-free pinyin rather
        // than UTF-8 Han characters. Keep the user-visible phrases in the ABI
        // contract while registering their acoustic forms here.
        esp_mn_commands_add(MICROPIXEL_VOICE_LOCAL_COMMAND_SLEEP, "shui jiao ba") != ESP_OK ||
        esp_mn_commands_add(MICROPIXEL_VOICE_LOCAL_COMMAND_WAKE, "xing xing") != ESP_OK ||
        esp_mn_commands_add(MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_UP, "da sheng yi dian") != ESP_OK ||
        esp_mn_commands_add(MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_DOWN, "xiao sheng yi dian") != ESP_OK ||
        esp_mn_commands_update() != nullptr) {
        (void)esp_mn_commands_free();
        multinet_handle_->destroy(multinet_data_);
        multinet_data_ = nullptr;
        multinet_handle_ = nullptr;
        return false;
    }
    multinet_chunk_frames_ = static_cast<uint32_t>(chunk_frames);
    ESP_LOGI(kTag, "MultiNet ready: model=%s chunk=%" PRIu32 " threshold=%.2f commands=4", model_name,
             multinet_chunk_frames_, static_cast<double>(kLocalCommandThreshold));
    return true;
}

void VoiceEngine::ShutdownSpeechRecognition() {
    speech_running_.store(false, std::memory_order_release);
    input_.EndCapture(this);
    vTaskDelay(pdMS_TO_TICKS(20));
    if (multinet_data_ != nullptr) {
        (void)esp_mn_commands_free();
        multinet_handle_->destroy(multinet_data_);
    }
    multinet_data_ = nullptr;
    multinet_handle_ = nullptr;
    multinet_chunk_frames_ = 0U;
    if (afe_handle_ != nullptr && afe_data_ != nullptr) afe_handle_->destroy(afe_data_);
    afe_data_ = nullptr;
    afe_handle_ = nullptr;
    if (speech_models_ != nullptr) esp_srmodel_deinit(speech_models_);
    speech_models_ = nullptr;
}

void VoiceEngine::ResetLocalCommands() {
    multinet_workspace_size_ = 0U;
    if (multinet_handle_ != nullptr && multinet_data_ != nullptr) multinet_handle_->clean(multinet_data_);
}

uint8_t VoiceEngine::FeedLocalCommands(const int16_t* samples, uint32_t frames) {
    if (!local_commands_enabled_.load(std::memory_order_acquire) || samples == nullptr || multinet_handle_ == nullptr ||
        multinet_data_ == nullptr || multinet_chunk_frames_ == 0U) {
        return 0U;
    }
    uint32_t offset = 0U;
    while (offset < frames) {
        const uint32_t copied = std::min(multinet_chunk_frames_ - multinet_workspace_size_, frames - offset);
        std::copy_n(samples + offset, copied, multinet_workspace_.data() + multinet_workspace_size_);
        offset += copied;
        multinet_workspace_size_ += copied;
        if (multinet_workspace_size_ != multinet_chunk_frames_) continue;
        multinet_workspace_size_ = 0U;
        const esp_mn_state_t state = multinet_handle_->detect(multinet_data_, multinet_workspace_.data());
        if (state == ESP_MN_STATE_TIMEOUT) {
            multinet_handle_->clean(multinet_data_);
            continue;
        }
        if (state != ESP_MN_STATE_DETECTED) continue;
        esp_mn_results_t* results = multinet_handle_->get_results(multinet_data_);
        if (results == nullptr || results->num <= 0 || results->prob[0] < kLocalCommandThreshold ||
            results->command_id[0] < MICROPIXEL_VOICE_LOCAL_COMMAND_SLEEP ||
            results->command_id[0] > MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_DOWN) {
            continue;
        }
        ESP_LOGI(kTag, "local command detected: id=%d phrase=%d probability=%.3f", results->command_id[0],
                 results->phrase_id[0], static_cast<double>(results->prob[0]));
        return static_cast<uint8_t>(results->command_id[0]);
    }
    return 0U;
}

void VoiceEngine::SpeechFeedTaskEntry(void* context) {
    static_cast<VoiceEngine*>(context)->RunSpeechFeed();
    vTaskDelete(nullptr);
}

void VoiceEngine::SpeechDetectTaskEntry(void* context) {
    static_cast<VoiceEngine*>(context)->RunSpeechDetect();
    vTaskDelete(nullptr);
}

void VoiceEngine::RunSpeechFeed() {
    uint32_t begin_attempts = 0U;
    uint32_t begin_failures = 0U;
    uint32_t read_attempts = 0U;
    uint32_t read_failures = 0U;
    uint32_t feed_attempts = 0U;
    uint32_t feed_failures = 0U;
    uint64_t requested_frames = 0U;
    uint64_t actual_frames = 0U;
    int32_t last_begin_status = MICROPIXEL_STATUS_OK;
    int32_t last_read_status = MICROPIXEL_STATUS_OK;
    int last_feed_status = 0;
    uint64_t last_summary_us = esp_timer_get_time();
    bool logged_first_begin = false;
    bool logged_first_read_request = false;
    bool logged_first_read = false;
    bool logged_first_feed = false;
    uint32_t consecutive_feed_successes = 0U;
    auto log_summary_if_due = [&]() {
        const uint64_t now_us = esp_timer_get_time();
        if (now_us - last_summary_us < kSpeechFeedSummaryIntervalUs) return;
        last_summary_us = now_us;
        ESP_LOGI(kTag,
                 "speech feed: begin=%" PRIu32 "/%" PRIu32 " last=%" PRId32 " read=%" PRIu32 "/%" PRIu32
                 " frames=%" PRIu64 "/%" PRIu64 " last=%" PRId32 " afe=%" PRIu32 "/%" PRIu32 " last=%d",
                 begin_attempts, begin_failures, last_begin_status, read_attempts, read_failures, actual_frames,
                 requested_frames, last_read_status, feed_attempts, feed_failures, last_feed_status);
    };
    while (speech_running_.load(std::memory_order_acquire)) {
        if (capture_takeover_requested_.load(std::memory_order_acquire)) {
            consecutive_feed_successes = 0U;
            speech_feed_healthy_.store(false, std::memory_order_release);
            speech_feed_paused_.store(true, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        speech_feed_paused_.store(false, std::memory_order_release);
        ++begin_attempts;
        last_begin_status = input_.BeginCapture(this);
        if (!logged_first_begin) {
            ESP_LOGI(kTag, "speech capture begin: status=%" PRId32 " feed_frames=%" PRIu32, last_begin_status,
                     speech_feed_frames_);
            logged_first_begin = true;
        }
        if (last_begin_status != MICROPIXEL_STATUS_OK) {
            ++begin_failures;
            consecutive_feed_successes = 0U;
            speech_feed_healthy_.store(false, std::memory_order_release);
            log_summary_if_due();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        bool complete = true;
        for (uint32_t offset = 0U; offset < speech_feed_frames_;) {
            const uint32_t requested =
                std::min<uint32_t>(MICROPIXEL_AUDIO_INPUT_MAX_READ_FRAMES, speech_feed_frames_ - offset);
            uint32_t frames = 0U;
            ++read_attempts;
            requested_frames += requested;
            if (!logged_first_read_request) {
                ESP_LOGI(kTag, "speech capture first read: requested=%" PRIu32, requested);
                logged_first_read_request = true;
            }
            last_read_status = input_.ReadCapture(this, speech_feed_workspace_.data() + offset, requested, frames);
            actual_frames += frames;
            if (last_read_status == MICROPIXEL_STATUS_OK && frames == requested && !logged_first_read) {
                ESP_LOGI(kTag, "speech capture first read completed: frames=%" PRIu32, frames);
                logged_first_read = true;
            }
            if (last_read_status != MICROPIXEL_STATUS_OK || frames != requested) {
                ++read_failures;
                consecutive_feed_successes = 0U;
                speech_feed_healthy_.store(false, std::memory_order_release);
                complete = false;
                break;
            }
            offset += frames;
        }
        if (!complete) {
            input_.EndCapture(this);
            log_summary_if_due();
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        {
            std::lock_guard lock(mutex_);
            for (uint32_t index = 0U; index < speech_feed_frames_; ++index) {
                const int16_t sample = speech_feed_workspace_[index];
                preroll_[preroll_write_] = sample;
                preroll_write_ = (preroll_write_ + 1U) % kPrerollFrames;
                if (handshake_capture_active_) {
                    if (handshake_capture_size_ == kHandshakeCaptureFrames) {
                        handshake_capture_overflow_ = true;
                        if (handshake_preroll_size_ != 0U) --handshake_preroll_size_;
                    } else {
                        ++handshake_capture_size_;
                    }
                    handshake_capture_[handshake_capture_write_] = sample;
                    handshake_capture_write_ = (handshake_capture_write_ + 1U) % kHandshakeCaptureFrames;
                }
            }
            preroll_size_ = std::min<uint32_t>(kPrerollFrames, preroll_size_ + speech_feed_frames_);
        }
        ++feed_attempts;
        last_feed_status = afe_handle_->feed(afe_data_, speech_feed_workspace_.data());
        if (last_feed_status < 0) {
            ++feed_failures;
            consecutive_feed_successes = 0U;
            speech_feed_healthy_.store(false, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(10));
        } else {
            const uint32_t generation = speech_feed_generation_.fetch_add(1U, std::memory_order_acq_rel) + 1U;
            consecutive_feed_successes = std::min(consecutive_feed_successes + 1U, kSpeechFeedWarmupChunks);
            if (consecutive_feed_successes == kSpeechFeedWarmupChunks) {
                speech_feed_healthy_.store(true, std::memory_order_release);
            }
            if (!logged_first_feed) {
                ESP_LOGI(kTag, "speech AFE first feed completed: generation=%" PRIu32, generation);
                logged_first_feed = true;
            }
        }
        log_summary_if_due();
    }
    speech_feed_paused_.store(true, std::memory_order_release);
    speech_feed_healthy_.store(false, std::memory_order_release);
    input_.EndCapture(this);
}

void VoiceEngine::RunSpeechDetect() {
    uint32_t required_feed_generation = kSpeechFeedWarmupChunks;
    bool takeover_observed = false;
    while (speech_running_.load(std::memory_order_acquire)) {
        // Keep draining AFE while the gateway connects so its internal ring
        // cannot fill. Only the explicit microphone takeover pauses fetch.
        if (capture_takeover_requested_.load(std::memory_order_acquire)) {
            if (!takeover_observed) {
                required_feed_generation =
                    speech_feed_generation_.load(std::memory_order_acquire) + kSpeechFeedWarmupChunks;
                takeover_observed = true;
            }
            speech_detect_paused_.store(true, std::memory_order_release);
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        takeover_observed = false;
        speech_detect_paused_.store(false, std::memory_order_release);
        if (!speech_feed_healthy_.load(std::memory_order_acquire) ||
            speech_feed_generation_.load(std::memory_order_acquire) < required_feed_generation) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }
        afe_fetch_result_t* result = afe_handle_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(50));
        if (!speech_running_.load(std::memory_order_acquire)) break;
        if (result == nullptr || result->ret_value != ESP_OK) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (task_running_.load(std::memory_order_acquire)) {
            if (local_commands_enabled_.load(std::memory_order_acquire) &&
                local_command_pending_.load(std::memory_order_acquire) == 0U && result->data != nullptr &&
                result->data_size > 0 && (result->data_size % static_cast<int>(sizeof(int16_t))) == 0) {
                const uint8_t command =
                    FeedLocalCommands(result->data, static_cast<uint32_t>(result->data_size) / sizeof(int16_t));
                if (command != 0U) local_command_pending_.store(command, std::memory_order_release);
            }
            continue;
        }
        if (result->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(kTag, "wake word detected: model=%d word=%d", result->wakenet_model_index,
                     result->wake_word_index);
            (void)StartSession();
        }
    }
    speech_detect_paused_.store(true, std::memory_order_release);
}

void VoiceEngine::ArmHandshakeCaptureLocked() {
    handshake_capture_write_ = 0U;
    handshake_capture_size_ = preroll_size_ - (preroll_size_ % kFrameSamples);
    handshake_preroll_size_ = handshake_capture_size_;
    handshake_capture_overflow_ = false;
    const uint32_t oldest = (preroll_write_ + kPrerollFrames - preroll_size_) % kPrerollFrames;
    for (uint32_t index = 0U; index < handshake_capture_size_; ++index) {
        handshake_capture_[index] = preroll_[(oldest + index) % kPrerollFrames];
    }
    handshake_capture_write_ = handshake_capture_size_ % kHandshakeCaptureFrames;
    handshake_capture_active_ = true;
}

bool VoiceEngine::LoadProfile() {
    nvs_handle_t handle{};
    if (nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return false;
    size_t size = sizeof(profile_);
    const esp_err_t status = nvs_get_blob(handle, kNvsKey, &profile_, &size);
    nvs_close(handle);
    if (status != ESP_OK || size != sizeof(profile_) || profile_.magic != kVoiceProfileMagic ||
        profile_.url_length == 0U || profile_.url_length >= profile_.url.size() ||
        profile_.ca_length >= profile_.ca_pem.size() || profile_.token_length == 0U ||
        profile_.token_length >= profile_.token.size() || profile_.url[profile_.url_length] != '\0' ||
        profile_.ca_pem[profile_.ca_length] != '\0' || profile_.token[profile_.token_length] != '\0') {
        profile_ = {};
        return false;
    }
    const bool secure = StartsWith(profile_.url.data(), "wss://");
    const bool insecure =
        StartsWith(profile_.url.data(), "ws://") && (profile_.flags & kVoiceProfileAllowInsecure) != 0U;
    if ((!secure && !insecure) || (secure && profile_.ca_length == 0U)) {
        profile_ = {};
        return false;
    }
    return true;
}

void VoiceEngine::LoadPreferences() {
    StoredPreferences stored{};
    nvs_handle_t handle{};
    if (nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READONLY, &handle) != ESP_OK) return;
    size_t size = sizeof(stored);
    const esp_err_t status = nvs_get_blob(handle, kPreferencesKey, &stored, &size);
    nvs_close(handle);
    if (status != ESP_OK || size != sizeof(stored) || stored.magic != kPreferencesMagic ||
        stored.version != kPreferencesVersion || stored.size != sizeof(stored) ||
        stored.value.size != sizeof(stored.value) || stored.value.response_gain_per_mille > 1000U ||
        stored.value.local_commands_enabled > 1U || stored.value.reserved0 != 0U || stored.value.reserved1 != 0U) {
        return;
    }
    preferences_ = stored.value;
    local_commands_enabled_.store(preferences_.local_commands_enabled != 0U, std::memory_order_release);
}

int32_t VoiceEngine::SavePreferences(const micropixel_voice_preferences_t& preferences) const {
    const StoredPreferences stored{.magic = kPreferencesMagic,
                                   .version = kPreferencesVersion,
                                   .size = sizeof(StoredPreferences),
                                   .value = preferences};
    nvs_handle_t handle{};
    esp_err_t status = nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READWRITE, &handle);
    if (status == ESP_OK) status = nvs_set_blob(handle, kPreferencesKey, &stored, sizeof(stored));
    if (status == ESP_OK) status = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    return status == ESP_OK ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t VoiceEngine::Configure(std::span<const uint8_t> bytes) {
    if (task_running_.load(std::memory_order_acquire) || bytes.size() < sizeof(VoiceProfileWireHeader)) {
        return MICROPIXEL_STATUS_WOULD_BLOCK;
    }
    VoiceProfileWireHeader header{};
    std::memcpy(&header, bytes.data(), sizeof(header));
    const size_t expected = sizeof(header) + header.url_length + header.ca_length + header.token_length;
    if (header.magic != kVoiceProfileMagic || header.version != kVoiceProfileVersion || header.size != sizeof(header) ||
        header.reserved0 != 0U || (header.flags & ~kVoiceProfileAllowInsecure) != 0U || expected != bytes.size() ||
        header.url_length == 0U || header.url_length > 256U || header.ca_length > 4096U || header.token_length == 0U ||
        header.token_length > 1024U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    Profile& candidate = profile_staging_;
    candidate = {};
    candidate.magic = kVoiceProfileMagic;
    candidate.revision = header.revision == 0U ? profile_.revision + 1U : header.revision;
    candidate.flags = header.flags;
    candidate.url_length = header.url_length;
    candidate.ca_length = header.ca_length;
    candidate.token_length = header.token_length;
    const uint8_t* cursor = bytes.data() + sizeof(header);
    std::memcpy(candidate.url.data(), cursor, header.url_length);
    cursor += header.url_length;
    std::memcpy(candidate.ca_pem.data(), cursor, header.ca_length);
    cursor += header.ca_length;
    std::memcpy(candidate.token.data(), cursor, header.token_length);
    const bool secure = StartsWith(candidate.url.data(), "wss://");
    const bool insecure =
        StartsWith(candidate.url.data(), "ws://") && (candidate.flags & kVoiceProfileAllowInsecure) != 0U;
    if ((!secure && !insecure) || (secure && candidate.ca_length == 0U)) {
        candidate = {};
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    nvs_handle_t handle{};
    esp_err_t status = nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READWRITE, &handle);
    if (status == ESP_OK) status = nvs_set_blob(handle, kNvsKey, &candidate, sizeof(candidate));
    if (status == ESP_OK) status = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    if (status != ESP_OK) {
        candidate = {};
        return MICROPIXEL_STATUS_INTERNAL;
    }
    {
        std::lock_guard lock(mutex_);
        profile_ = candidate;
    }
    candidate = {};
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::ClearProfile() {
    if (task_running_.load(std::memory_order_acquire)) return MICROPIXEL_STATUS_WOULD_BLOCK;
    nvs_handle_t handle{};
    esp_err_t status = nvs_open_from_partition(kNvsPartition, kNvsNamespace, NVS_READWRITE, &handle);
    if (status == ESP_OK) status = nvs_erase_key(handle, kNvsKey);
    if (status == ESP_ERR_NVS_NOT_FOUND) status = ESP_OK;
    if (status == ESP_OK) status = nvs_commit(handle);
    if (handle != 0U) nvs_close(handle);
    if (status != ESP_OK) return MICROPIXEL_STATUS_INTERNAL;
    std::lock_guard lock(mutex_);
    profile_ = {};
    return MICROPIXEL_STATUS_OK;
}

VoiceProfileStatus VoiceEngine::ProfileStatus() const {
    std::lock_guard lock(mutex_);
    VoiceProfileStatus status{};
    status.configured = profile_.magic == kVoiceProfileMagic;
    status.allow_insecure = (profile_.flags & kVoiceProfileAllowInsecure) != 0U;
    status.revision = profile_.revision;
    status.url = profile_.url;
    if (status.configured && profile_.ca_length != 0U) {
        std::array<uint8_t, 32U> digest{};
        size_t digest_length = 0U;
        if (psa_crypto_init() == PSA_SUCCESS &&
            psa_hash_compute(PSA_ALG_SHA_256, reinterpret_cast<const uint8_t*>(profile_.ca_pem.data()),
                             profile_.ca_length, digest.data(), digest.size(), &digest_length) == PSA_SUCCESS &&
            digest_length == digest.size()) {
            for (size_t index = 0U; index < digest.size(); ++index) {
                std::snprintf(status.certificate_fingerprint.data() + index * 2U, 3U, "%02x", digest[index]);
            }
        }
    }
    if (status.configured) {
        const size_t suffix = std::min<size_t>(8U, profile_.token_length);
        std::memcpy(status.token_suffix.data(), profile_.token.data() + profile_.token_length - suffix, suffix);
    }
    return status;
}

bool VoiceEngine::RouteOnline() const {
    network::NetworkSnapshot snapshot{};
    network_.CopySnapshot(snapshot);
    return snapshot.connected;
}

int32_t VoiceEngine::GetInfo(micropixel_voice_info_t& info) const {
    std::lock_guard lock(mutex_);
    info = {};
    info.size = sizeof(info);
    info.configured = profile_.magic == kVoiceProfileMagic ? 1U : 0U;
    info.route_online = RouteOnline() ? 1U : 0U;
    info.sample_rate = kSampleRate;
    info.channels = 1U;
    info.frame_duration_ms = 20U;
    info.max_transcript_bytes = kTranscriptCapacity;
    info.max_response_bytes = kResponseCapacity;
    info.profile_revision = profile_.revision;
    info.capabilities =
        MICROPIXEL_VOICE_CAP_STREAMING_TEXT | MICROPIXEL_VOICE_CAP_STREAMING_AUDIO | MICROPIXEL_VOICE_CAP_TOUCH_CANCEL;
    if (afe_data_ != nullptr) info.capabilities |= MICROPIXEL_VOICE_CAP_WAKE_WORD;
    if (multinet_data_ != nullptr) info.capabilities |= MICROPIXEL_VOICE_CAP_LOCAL_COMMANDS;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::StartSession() {
    bool unavailable = false;
    {
        std::lock_guard lock(mutex_);
        if (task_running_.load(std::memory_order_acquire) || state_ == MICROPIXEL_VOICE_STATE_LISTENING ||
            state_ == MICROPIXEL_VOICE_STATE_THINKING || state_ == MICROPIXEL_VOICE_STATE_SPEAKING) {
            return MICROPIXEL_STATUS_WOULD_BLOCK;
        }
        session_online_ = profile_.magic == kVoiceProfileMagic && RouteOnline();
        if (!session_online_ && multinet_data_ == nullptr) {
            state_ = MICROPIXEL_VOICE_STATE_OFFLINE;
            unavailable = true;
        } else {
            cancel_.store(false, std::memory_order_release);
            local_command_pending_.store(0U, std::memory_order_release);
            ResetLocalCommands();
            ++session_id_;
            if (session_id_ == 0U) ++session_id_;
            ++generation_;
            if (generation_ == 0U) ++generation_;
            ResetText();
            ArmHandshakeCaptureLocked();
            state_ = MICROPIXEL_VOICE_STATE_WOKEN;
            session_started_us_.store(static_cast<uint64_t>(esp_timer_get_time()), std::memory_order_release);
            first_transcript_ms_.store(0U, std::memory_order_release);
            first_response_text_ms_.store(0U, std::memory_order_release);
            first_audio_ms_.store(0U, std::memory_order_release);
            audio_queue_dropped_.store(0U, std::memory_order_release);
            last_status_.store(MICROPIXEL_STATUS_OK, std::memory_order_release);
        }
    }
    if (unavailable) {
        SetState(MICROPIXEL_VOICE_STATE_OFFLINE, MICROPIXEL_STATUS_WOULD_BLOCK);
        return MICROPIXEL_STATUS_WOULD_BLOCK;
    }
    SetState(MICROPIXEL_VOICE_STATE_WOKEN);
    task_running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(TaskEntry, "micropixel_voice", 8192, this, task_policy::kAudioPriority, nullptr,
                                task_policy::kSystemCore) != pdPASS) {
        task_running_.store(false, std::memory_order_release);
        {
            std::lock_guard lock(mutex_);
            handshake_capture_active_ = false;
            handshake_capture_size_ = 0U;
            handshake_preroll_size_ = 0U;
        }
        SetState(MICROPIXEL_VOICE_STATE_ERROR, MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
        return MICROPIXEL_STATUS_RESOURCE_EXHAUSTED;
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::CancelSession() {
    cancel_.store(true, std::memory_order_release);
    esp_websocket_client_handle_t websocket = nullptr;
    device::PcmStreamHandle playback = 0U;
    uint32_t session_id = 0U;
    uint32_t generation = 0U;
    {
        std::lock_guard lock(mutex_);
        websocket = websocket_;
        playback = playback_handle_;
        playback_handle_ = 0U;
        session_id = session_id_;
        generation = generation_;
        server_audio_finished_.store(true, std::memory_order_release);
        playback_finished_.store(true, std::memory_order_release);
    }
    if (websocket != nullptr && connected_.load(std::memory_order_acquire)) {
        // Cancel runs on the caller thread; keep the buffer local so it cannot
        // be rewritten while the session thread formats its own cancel message.
        std::array<char, 128U> message{};
        const int length =
            std::snprintf(message.data(), message.size(),
                          "{\"type\":\"session.cancel\",\"session_id\":%" PRIu32 ",\"generation\":%" PRIu32 "}",
                          session_id, generation);
        if (length > 0 && static_cast<size_t>(length) < message.size()) {
            (void)esp_websocket_client_send_text(websocket, message.data(), length, pdMS_TO_TICKS(100));
        }
    }
    if (playback != 0U) (void)audio_.StopPcm(playback);
    SetState(MICROPIXEL_VOICE_STATE_IDLE, MICROPIXEL_STATUS_CANCELLED);
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::GetState(micropixel_voice_state_response_t& state) const {
    std::lock_guard lock(mutex_);
    state = {};
    state.size = sizeof(state);
    state.state = state_;
    state.session_id = session_id_;
    state.input_level_per_mille = input_level_;
    state.transcript_revision = transcript_revision_;
    state.response_revision = response_revision_;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::GetPreferences(micropixel_voice_preferences_t& preferences) const {
    std::lock_guard lock(mutex_);
    preferences = preferences_;
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::SetPreferences(const micropixel_voice_set_preferences_request_t& request) {
    constexpr uint16_t kKnownMask =
        MICROPIXEL_VOICE_PREFERENCE_RESPONSE_GAIN | MICROPIXEL_VOICE_PREFERENCE_LOCAL_COMMANDS;
    if (request.size != sizeof(request) || request.mask == 0U || (request.mask & ~kKnownMask) != 0U ||
        request.response_gain_per_mille > 1000U || request.local_commands_enabled > 1U || request.reserved0 != 0U) {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    micropixel_voice_preferences_t candidate{};
    device::PcmStreamHandle playback = 0U;
    {
        std::lock_guard lock(mutex_);
        candidate = preferences_;
        if ((request.mask & MICROPIXEL_VOICE_PREFERENCE_RESPONSE_GAIN) != 0U) {
            candidate.response_gain_per_mille = request.response_gain_per_mille;
        }
        if ((request.mask & MICROPIXEL_VOICE_PREFERENCE_LOCAL_COMMANDS) != 0U) {
            candidate.local_commands_enabled = request.local_commands_enabled;
        }
        if (candidate.response_gain_per_mille == preferences_.response_gain_per_mille &&
            candidate.local_commands_enabled == preferences_.local_commands_enabled) {
            return MICROPIXEL_STATUS_OK;
        }
        ++candidate.revision;
        if (candidate.revision == 0U) ++candidate.revision;
        playback = playback_handle_;
    }
    const int32_t status = SavePreferences(candidate);
    if (status != MICROPIXEL_STATUS_OK) return status;
    {
        std::lock_guard lock(mutex_);
        preferences_ = candidate;
    }
    local_commands_enabled_.store(candidate.local_commands_enabled != 0U, std::memory_order_release);
    if (playback != 0U && (request.mask & MICROPIXEL_VOICE_PREFERENCE_RESPONSE_GAIN) != 0U) {
        (void)audio_.SetPcmVolume(playback, candidate.response_gain_per_mille);
    }
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::GetDiagnostics(micropixel_voice_diagnostics_t& diagnostics) const {
    micropixel_audio_info_t audio_info{};
    micropixel_audio_input_info_t input_info{};
    const bool speaker_ready = audio_.GetInfo(audio_info) == MICROPIXEL_STATUS_OK;
    const bool microphone_ready = input_.GetInfo(input_info) == MICROPIXEL_STATUS_OK;
    std::lock_guard lock(mutex_);
    diagnostics = {};
    diagnostics.size = sizeof(diagnostics);
    if (profile_.magic == kVoiceProfileMagic) diagnostics.flags |= MICROPIXEL_VOICE_DIAGNOSTIC_CONFIGURED;
    if (RouteOnline()) diagnostics.flags |= MICROPIXEL_VOICE_DIAGNOSTIC_ROUTE_ONLINE;
    if (microphone_ready && afe_data_ != nullptr) diagnostics.flags |= MICROPIXEL_VOICE_DIAGNOSTIC_MICROPHONE_READY;
    if (speaker_ready) diagnostics.flags |= MICROPIXEL_VOICE_DIAGNOSTIC_SPEAKER_READY;
    if (task_running_.load(std::memory_order_acquire)) diagnostics.flags |= MICROPIXEL_VOICE_DIAGNOSTIC_SESSION_ACTIVE;
    diagnostics.input_level_per_mille = input_level_;
    diagnostics.queue_depth =
        audio_queue_ == nullptr ? 0U : static_cast<uint16_t>(uxQueueMessagesWaiting(audio_queue_));
    diagnostics.queue_high_water =
        static_cast<uint16_t>(std::min<uint32_t>(UINT16_MAX, audio_queue_high_water_.load(std::memory_order_relaxed)));
    diagnostics.frames_sent = audio_sender_frames_sent_.load(std::memory_order_relaxed);
    diagnostics.frames_dropped = audio_queue_dropped_.load(std::memory_order_relaxed);
    diagnostics.first_transcript_ms = first_transcript_ms_.load(std::memory_order_relaxed);
    diagnostics.first_response_text_ms = first_response_text_ms_.load(std::memory_order_relaxed);
    diagnostics.first_audio_ms = first_audio_ms_.load(std::memory_order_relaxed);
    diagnostics.last_status = last_status_.load(std::memory_order_relaxed);
    return MICROPIXEL_STATUS_OK;
}

int32_t VoiceEngine::RunDiagnostic(uint8_t kind) {
    if (task_running_.load(std::memory_order_acquire)) return MICROPIXEL_STATUS_WOULD_BLOCK;
    if (kind == MICROPIXEL_VOICE_DIAGNOSTIC_MICROPHONE) {
        return afe_data_ == nullptr ? MICROPIXEL_STATUS_UNSUPPORTED : MICROPIXEL_STATUS_OK;
    }
    if (kind != MICROPIXEL_VOICE_DIAGNOSTIC_SPEAKER) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    micropixel_audio_tone_t tone{};
    tone.size = sizeof(tone);
    tone.waveform = MICROPIXEL_AUDIO_WAVE_SINE;
    tone.volume_per_mille = 150U;
    tone.frequency_millihz = 523250U;
    tone.duration_ms = 180U;
    tone.attack_ms = 12U;
    tone.release_ms = 30U;
    return audio_.PlayTone(tone);
}

int32_t VoiceEngine::ClearContext() {
    if (task_running_.load(std::memory_order_acquire)) return MICROPIXEL_STATUS_WOULD_BLOCK;
    if (!connected_.load(std::memory_order_acquire)) return MICROPIXEL_STATUS_OK;
    return SendText("{\"type\":\"context.clear\"}") ? MICROPIXEL_STATUS_OK : MICROPIXEL_STATUS_INTERNAL;
}

int32_t VoiceEngine::ReadText(uint8_t kind, uint32_t revision, uint32_t offset, std::span<uint8_t> destination,
                              device::VoiceTextResult& result) const {
    std::lock_guard lock(mutex_);
    const char* source = nullptr;
    uint32_t length = 0U;
    uint32_t current_revision = 0U;
    uint8_t flags = 0U;
    if (kind == MICROPIXEL_VOICE_TEXT_TRANSCRIPT) {
        source = transcript_.data();
        length = transcript_length_;
        current_revision = transcript_revision_;
        flags = transcript_flags_;
    } else if (kind == MICROPIXEL_VOICE_TEXT_RESPONSE) {
        source = response_.data();
        length = response_length_;
        current_revision = response_revision_;
        flags = response_flags_;
    } else {
        return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    }
    if (revision != current_revision) return MICROPIXEL_STATUS_STALE_STATE;
    if (offset > length) return MICROPIXEL_STATUS_INVALID_ARGUMENT;
    const uint32_t copied = std::min<uint32_t>(destination.size(), length - offset);
    if (copied != 0U) std::memcpy(destination.data(), source + offset, copied);
    result = {.revision = current_revision,
              .total_length = length,
              .chunk_length = static_cast<uint16_t>(copied),
              .flags = flags};
    return MICROPIXEL_STATUS_OK;
}

void VoiceEngine::BindUpdateSink(device::VoiceUpdateSink sink, void* context) {
    std::lock_guard lock(mutex_);
    sink_ = sink;
    sink_context_ = context;
}

void VoiceEngine::UnbindUpdateSink(void* context) {
    std::lock_guard lock(mutex_);
    if (sink_context_ == context) {
        sink_ = nullptr;
        sink_context_ = nullptr;
    }
}

void VoiceEngine::TaskEntry(void* context) {
    auto* self = static_cast<VoiceEngine*>(context);
    self->RunSession();
    self->task_running_.store(false, std::memory_order_release);
    vTaskDelete(nullptr);
}

void VoiceEngine::AudioSenderTaskEntry(void* context) {
    static_cast<VoiceEngine*>(context)->RunAudioSender();
    vTaskDelete(nullptr);
}

bool VoiceEngine::StartAudioSender() {
    if (audio_queue_ == nullptr || audio_sender_running_.load(std::memory_order_acquire)) return false;
    (void)xQueueReset(audio_queue_);
    audio_sender_failed_.store(false, std::memory_order_release);
    audio_sender_stop_.store(false, std::memory_order_release);
    audio_sender_frames_sent_.store(0U, std::memory_order_release);
    audio_queue_high_water_.store(0U, std::memory_order_release);
    audio_sender_running_.store(true, std::memory_order_release);
    if (xTaskCreatePinnedToCore(AudioSenderTaskEntry, "voice_ws_send", kAudioSenderStackBytes, this,
                                task_policy::kHostPriority, nullptr, task_policy::kSystemCore) != pdPASS) {
        audio_sender_running_.store(false, std::memory_order_release);
        return false;
    }
    return true;
}

bool VoiceEngine::QueueAudio(const int16_t* samples, uint32_t sequence, uint64_t timestamp_us) {
    if (samples == nullptr || audio_queue_ == nullptr || !audio_sender_running_.load(std::memory_order_acquire) ||
        audio_sender_failed_.load(std::memory_order_acquire) || audio_sender_stop_.load(std::memory_order_acquire)) {
        return false;
    }
    QueuedAudioFrame frame{};
    frame.timestamp_us = timestamp_us;
    frame.sequence = sequence;
    frame.sample_count = kFrameSamples;
    std::copy_n(samples, kFrameSamples, frame.samples.data());
    if (xQueueSend(audio_queue_, &frame, pdMS_TO_TICKS(kAudioQueueWaitMs)) != pdTRUE) {
        audio_queue_dropped_.fetch_add(1U, std::memory_order_relaxed);
        return false;
    }
    const uint32_t depth = static_cast<uint32_t>(uxQueueMessagesWaiting(audio_queue_));
    uint32_t high_water = audio_queue_high_water_.load(std::memory_order_relaxed);
    while (depth > high_water && !audio_queue_high_water_.compare_exchange_weak(
                                     high_water, depth, std::memory_order_relaxed, std::memory_order_relaxed)) {
    }
    return true;
}

void VoiceEngine::RunAudioSender() {
    QueuedAudioFrame frame{};
    uint64_t encode_total_us = 0U;
    uint64_t encode_max_us = 0U;
    uint32_t encoded_frames = 0U;
    while (!audio_sender_stop_.load(std::memory_order_acquire)) {
        if (xQueueReceive(audio_queue_, &frame, portMAX_DELAY) != pdTRUE) continue;
        if (frame.sample_count == 0U) break;
        const int64_t encode_started = esp_timer_get_time();
        const int encoded = opus_encode(encoder_, frame.samples.data(), frame.sample_count, packet_workspace_.data(),
                                        packet_workspace_.size());
        const uint64_t encode_us = static_cast<uint64_t>(esp_timer_get_time() - encode_started);
        encode_total_us += encode_us;
        encode_max_us = std::max(encode_max_us, encode_us);
        ++encoded_frames;
        if (encoded <= 0 ||
            !SendAudio(packet_workspace_.data(), static_cast<size_t>(encoded), frame.sequence, frame.timestamp_us)) {
            audio_sender_failed_.store(true, std::memory_order_release);
            break;
        }
        audio_sender_frames_sent_.fetch_add(1U, std::memory_order_relaxed);
        vTaskDelay(kVoiceWorkPauseTicks);
    }
    ESP_LOGI(kTag, "voice encode timing: frames=%" PRIu32 " mean_us=%" PRIu64 " max_us=%" PRIu64, encoded_frames,
             encoded_frames == 0U ? 0U : encode_total_us / encoded_frames, encode_max_us);
    audio_sender_running_.store(false, std::memory_order_release);
}

bool VoiceEngine::FlushAudioSender() {
    if (audio_queue_ == nullptr) return false;
    const QueuedAudioFrame stop{};
    bool queued = false;
    for (uint32_t waited = 0U; waited < kAudioFlushTimeoutMs; waited += 10U) {
        if (!audio_sender_running_.load(std::memory_order_acquire)) {
            return !audio_sender_failed_.load(std::memory_order_acquire);
        }
        if (xQueueSend(audio_queue_, &stop, 0U) == pdTRUE) {
            queued = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if (!queued) return false;
    for (uint32_t waited = 0U; audio_sender_running_.load(std::memory_order_acquire) && waited < kAudioFlushTimeoutMs;
         waited += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    return !audio_sender_running_.load(std::memory_order_acquire) &&
           !audio_sender_failed_.load(std::memory_order_acquire);
}

void VoiceEngine::StopAudioSender() {
    if (audio_queue_ == nullptr || !audio_sender_running_.load(std::memory_order_acquire)) return;
    audio_sender_stop_.store(true, std::memory_order_release);
    (void)xQueueReset(audio_queue_);
    const QueuedAudioFrame stop{};
    (void)xQueueSend(audio_queue_, &stop, portMAX_DELAY);
    while (audio_sender_running_.load(std::memory_order_acquire)) vTaskDelay(pdMS_TO_TICKS(10U));
}

void VoiceEngine::RunSession() {
    const int32_t output_pause_status = audio_.PauseOutputForCapture(this);
    if (output_pause_status != MICROPIXEL_STATUS_OK) {
        FinishSession(output_pause_status);
        DestroyTransport();
        return;
    }
    output_paused_for_capture_ = true;
    if (FinishPendingLocalCommand()) return;
    if (session_online_) {
        network_realtime_ = network_.WifiControl().SetRealtimeMode(true).has_value();
        // Configure()/ClearProfile() refuse to run while task_running_ is set,
        // and the WebSocket client borrows config.cert_pem, so profile_ stays
        // immutable and alive for the whole connection.
        std::snprintf(header_workspace_.data(), header_workspace_.size(), "Authorization: Bearer %s\r\n",
                      profile_.token.data());
        esp_websocket_client_config_t config{};
        config.uri = profile_.url.data();
        config.headers = header_workspace_.data();
        config.cert_pem = profile_.ca_length == 0U ? nullptr : profile_.ca_pem.data();
        config.disable_auto_reconnect = true;
        config.network_timeout_ms = kConnectTimeoutMs;
        esp_websocket_client_handle_t websocket = esp_websocket_client_init(&config);
        {
            std::lock_guard lock(mutex_);
            websocket_ = websocket;
        }
        if (websocket == nullptr ||
            esp_websocket_register_events(websocket, WEBSOCKET_EVENT_ANY, WebsocketEvent, this) != ESP_OK ||
            esp_websocket_client_start(websocket) != ESP_OK) {
            if (FinishPendingLocalCommand()) return;
            FinishSession(MICROPIXEL_STATUS_INTERNAL);
            DestroyTransport();
            return;
        }
        for (uint32_t waited = 0U;
             !cancel_.load(std::memory_order_acquire) && local_command_pending_.load(std::memory_order_acquire) == 0U &&
             !connected_.load(std::memory_order_acquire) && waited < kConnectTimeoutMs;
             waited += 10U) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (FinishPendingLocalCommand()) return;
        if (!connected_.load(std::memory_order_acquire) || cancel_.load(std::memory_order_acquire)) {
            FinishSession(MICROPIXEL_STATUS_TIMEOUT);
            DestroyTransport();
            return;
        }
        if (!SendText(
                "{\"type\":\"hello\",\"protocol\":2,\"codec\":\"opus\",\"sample_rate\":16000,\"channels\":1,\"frame_"
                "duration_ms\":20}")) {
            if (FinishPendingLocalCommand()) return;
            FinishSession(MICROPIXEL_STATUS_INTERNAL);
            DestroyTransport();
            return;
        }
        for (uint32_t waited = 0U;
             !cancel_.load(std::memory_order_acquire) && local_command_pending_.load(std::memory_order_acquire) == 0U &&
             !ready_.load(std::memory_order_acquire) && waited < 5000U;
             waited += 10U) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (FinishPendingLocalCommand()) return;
        const int start_length =
            std::snprintf(session_start_message_.data(), session_start_message_.size(),
                          "{\"type\":\"session.start\",\"session_id\":%" PRIu32 ",\"generation\":%" PRIu32 "}",
                          session_id_, generation_);
        if (!ready_.load(std::memory_order_acquire) || start_length <= 0 ||
            static_cast<size_t>(start_length) >= session_start_message_.size() ||
            !SendText(session_start_message_.data())) {
            FinishSession(MICROPIXEL_STATUS_TIMEOUT);
            DestroyTransport();
            return;
        }
        for (uint32_t waited = 0U;
             !cancel_.load(std::memory_order_acquire) && local_command_pending_.load(std::memory_order_acquire) == 0U &&
             !session_started_.load(std::memory_order_acquire) && waited < 10000U;
             waited += 10U) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
        if (FinishPendingLocalCommand()) return;
        if (!session_started_.load(std::memory_order_acquire) || cancel_.load(std::memory_order_acquire)) {
            FinishSession(cancel_.load(std::memory_order_acquire) ? MICROPIXEL_STATUS_CANCELLED
                                                                  : MICROPIXEL_STATUS_TIMEOUT);
            DestroyTransport();
            return;
        }
        int opus_error = OPUS_OK;
        OpusEncoder* encoder = opus_encoder_create(kSampleRate, 1, OPUS_APPLICATION_VOIP, &opus_error);
        OpusDecoder* decoder = opus_decoder_create(kSampleRate, 1, &opus_error);
        {
            std::lock_guard lock(mutex_);
            encoder_ = encoder;
            decoder_ = decoder;
        }
        if (encoder == nullptr || decoder == nullptr || opus_error != OPUS_OK) {
            FinishSession(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
            DestroyTransport();
            return;
        }
        if (opus_encoder_ctl(encoder_, OPUS_SET_BITRATE(24000)) != OPUS_OK ||
            opus_encoder_ctl(encoder_, OPUS_SET_COMPLEXITY(kEncoderComplexity)) != OPUS_OK) {
            FinishSession(MICROPIXEL_STATUS_INTERNAL);
            DestroyTransport();
            return;
        }
        if (!StartAudioSender()) {
            FinishSession(MICROPIXEL_STATUS_RESOURCE_EXHAUSTED);
            DestroyTransport();
            return;
        }
        if (FinishPendingLocalCommand()) return;
    }
    capture_takeover_requested_.store(true, std::memory_order_release);
    for (uint32_t waited = 0U; speech_running_.load(std::memory_order_acquire) &&
                               (!speech_feed_paused_.load(std::memory_order_acquire) ||
                                !speech_detect_paused_.load(std::memory_order_acquire)) &&
                               waited < 1000U;
         waited += 10U) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (speech_running_.load(std::memory_order_acquire) && (!speech_feed_paused_.load(std::memory_order_acquire) ||
                                                            !speech_detect_paused_.load(std::memory_order_acquire))) {
        FinishSession(MICROPIXEL_STATUS_TIMEOUT);
        DestroyTransport();
        return;
    }
    const bool vadnet_available = afe_handle_ != nullptr && afe_data_ != nullptr &&
                                  speech_feed_paused_.load(std::memory_order_acquire) &&
                                  speech_detect_paused_.load(std::memory_order_acquire);
    if (vadnet_available) {
        (void)afe_handle_->reset_buffer(afe_data_);
        (void)afe_handle_->reset_vad(afe_data_);
    }
    const int32_t capture_status = input_.BeginCapture(this);
    if (capture_status != MICROPIXEL_STATUS_OK) {
        FinishSession(capture_status);
        DestroyTransport();
        return;
    }
    capture_owned_ = true;
    SetState(MICROPIXEL_VOICE_STATE_LISTENING);
    uint32_t buffered_write = 0U;
    uint32_t buffered_size = 0U;
    uint32_t buffered_preroll = 0U;
    bool buffered_overflow = false;
    {
        std::lock_guard lock(mutex_);
        handshake_capture_active_ = false;
        buffered_write = handshake_capture_write_;
        buffered_size = handshake_capture_size_;
        buffered_preroll = handshake_preroll_size_;
        buffered_overflow = handshake_capture_overflow_;
    }
    const uint32_t buffered_oldest =
        (buffered_write + kHandshakeCaptureFrames - buffered_size) % kHandshakeCaptureFrames;
    uint32_t sequence = 0U;
    uint32_t silent_frames = 0U;
    bool speech_seen = false;
    bool vadnet_speech = false;
    uint32_t vad_workspace_size = 0U;
    uint32_t captured_frames = 0U;
    uint32_t speech_frames = 0U;
    uint32_t vadnet_speech_frames = 0U;
    uint32_t energy_speech_frames = 0U;
    uint32_t minimum_level = 1000U;
    uint32_t maximum_level = 0U;
    uint32_t noise_level = 0U;
    uint32_t fallback_threshold = 0U;
    uint32_t capture_read_offset = 0U;
    uint32_t capture_read_size = 0U;
    uint8_t local_command = local_command_pending_.exchange(0U, std::memory_order_acq_rel);
    auto capture_complete = [&]() {
        return cancel_.load(std::memory_order_acquire) || local_command != 0U ||
               captured_frames >= kMaximumCaptureFrames ||
               (!speech_seen && captured_frames >= kNoSpeechCaptureFrames) ||
               (speech_seen && silent_frames >= kSilenceCommitFrames);
    };
    uint64_t analysis_total_us = 0U;
    uint64_t analysis_max_us = 0U;
    auto submit_frame = [&](const int16_t* samples, bool analyze) {
        if (!analyze) {
            return !session_online_ || QueueAudio(samples, sequence++, esp_timer_get_time());
        }
        const int64_t analysis_started = esp_timer_get_time();
        uint64_t energy = 0U;
        for (uint32_t index = 0U; index < kFrameSamples; ++index) {
            energy += static_cast<uint32_t>(std::abs(static_cast<int32_t>(samples[index])));
        }
        const uint32_t level = std::min<uint32_t>(1000U, energy / kFrameSamples * 1000U / 12000U);
        minimum_level = std::min(minimum_level, level);
        maximum_level = std::max(maximum_level, level);
        {
            std::lock_guard lock(mutex_);
            input_level_ = static_cast<uint16_t>(level);
        }
        if ((captured_frames % 5U) == 0U) {
            micropixel_voice_event_payload_t payload{};
            payload.value = level;
            Emit(MICROPIXEL_VOICE_EVENT_INPUT_LEVEL, MICROPIXEL_STATUS_OK, payload);
        }
        ++captured_frames;
        if (!speech_seen) noise_level = noise_level == 0U ? level : (noise_level * 31U + level) / 32U;
        fallback_threshold = std::clamp<uint32_t>(std::max(noise_level + 24U, noise_level * 2U + 12U), 24U, 240U);
        const bool energy_speech = level >= fallback_threshold;
        if (energy_speech) ++energy_speech_frames;
        if (vadnet_available) {
            uint32_t source_offset = 0U;
            while (source_offset < kFrameSamples) {
                const uint32_t copied =
                    std::min(speech_feed_frames_ - vad_workspace_size, kFrameSamples - source_offset);
                std::copy_n(samples + source_offset, copied, session_vad_workspace_.data() + vad_workspace_size);
                source_offset += copied;
                vad_workspace_size += copied;
                if (vad_workspace_size == speech_feed_frames_) {
                    const int fed = afe_handle_->feed(afe_data_, session_vad_workspace_.data());
                    afe_fetch_result_t* result =
                        fed < 0 ? nullptr : afe_handle_->fetch_with_delay(afe_data_, pdMS_TO_TICKS(100));
                    if (result != nullptr && result->ret_value == ESP_OK) {
                        vadnet_speech = result->vad_state == VAD_SPEECH;
                        if (local_command == 0U && result->data != nullptr && result->data_size > 0 &&
                            (result->data_size % static_cast<int>(sizeof(int16_t))) == 0) {
                            local_command = FeedLocalCommands(
                                result->data, static_cast<uint32_t>(result->data_size) / sizeof(int16_t));
                        }
                    }
                    vad_workspace_size = 0U;
                }
            }
        }
        if (vadnet_speech) ++vadnet_speech_frames;
        const bool vad_speech = vadnet_speech || energy_speech;
        if (vad_speech) {
            speech_seen = true;
            silent_frames = 0U;
            ++speech_frames;
        } else if (speech_seen) {
            ++silent_frames;
        }
        const uint64_t analysis_us = static_cast<uint64_t>(esp_timer_get_time() - analysis_started);
        analysis_total_us += analysis_us;
        analysis_max_us = std::max(analysis_max_us, analysis_us);
        const bool queued = !session_online_ || QueueAudio(samples, sequence++, esp_timer_get_time());
        vTaskDelay(kVoiceWorkPauseTicks);
        return queued;
    };

    uint32_t buffered_offset = 0U;
    const uint32_t buffered_full_samples = buffered_size - (buffered_size % kFrameSamples);
    while (buffered_offset < buffered_full_samples && !capture_complete()) {
        for (uint32_t index = 0U; index < kFrameSamples; ++index) {
            capture_workspace_[index] =
                handshake_capture_[(buffered_oldest + buffered_offset + index) % kHandshakeCaptureFrames];
        }
        const bool analyze = buffered_offset >= buffered_preroll;
        if (!submit_frame(capture_workspace_.data(), analyze)) {
            FinishSession(MICROPIXEL_STATUS_INTERNAL);
            DestroyTransport();
            return;
        }
        buffered_offset += kFrameSamples;
    }

    uint32_t carried_frames = 0U;
    if (buffered_offset == buffered_full_samples && buffered_full_samples < buffered_size) {
        carried_frames = buffered_size - buffered_full_samples;
        for (uint32_t index = 0U; index < carried_frames; ++index) {
            capture_workspace_[index] =
                handshake_capture_[(buffered_oldest + buffered_full_samples + index) % kHandshakeCaptureFrames];
        }
    }
    while (!capture_complete()) {
        int32_t read_status = MICROPIXEL_STATUS_OK;
        uint32_t frame_offset = carried_frames;
        carried_frames = 0U;
        while (frame_offset < kFrameSamples) {
            if (capture_read_offset == capture_read_size) {
                capture_read_size = 0U;
                read_status =
                    input_.ReadCapture(this, capture_read_workspace_.data(), kCaptureReadFrames, capture_read_size);
                capture_read_offset = 0U;
                if (read_status != MICROPIXEL_STATUS_OK || capture_read_size != kCaptureReadFrames) break;
            }
            const uint32_t copied = std::min(kFrameSamples - frame_offset, capture_read_size - capture_read_offset);
            std::copy_n(capture_read_workspace_.data() + capture_read_offset, copied,
                        capture_workspace_.data() + frame_offset);
            capture_read_offset += copied;
            frame_offset += copied;
        }
        if (frame_offset != kFrameSamples) {
            FinishSession(read_status == MICROPIXEL_STATUS_OK ? static_cast<int32_t>(MICROPIXEL_STATUS_INTERNAL)
                                                              : read_status);
            DestroyTransport();
            return;
        }
        if (!submit_frame(capture_workspace_.data(), true)) {
            FinishSession(MICROPIXEL_STATUS_INTERNAL);
            DestroyTransport();
            return;
        }
    }
    const char* capture_reason = cancel_.load(std::memory_order_acquire)                     ? "cancelled"
                                 : local_command != 0U                                       ? "local_command"
                                 : !speech_seen && captured_frames >= kNoSpeechCaptureFrames ? "no_speech_timeout"
                                 : speech_seen && silent_frames >= kSilenceCommitFrames      ? "vad_silence"
                                                                                             : "maximum";
    ESP_LOGI(kTag,
             "voice capture complete: reason=%s elapsed_ms=%" PRIu32 " vad=%s speech_frames=%" PRIu32
             " (vadnet=%" PRIu32 " energy=%" PRIu32 ") silence_frames=%" PRIu32
             " level_min/max/noise/threshold=%" PRIu32 "/%" PRIu32 "/%" PRIu32 "/%" PRIu32,
             capture_reason, captured_frames * 20U, vadnet_available ? "hybrid" : "energy", speech_frames,
             vadnet_speech_frames, energy_speech_frames, silent_frames, minimum_level, maximum_level, noise_level,
             fallback_threshold);
    ESP_LOGI(kTag, "voice analysis timing: frames=%" PRIu32 " mean_us=%" PRIu64 " max_us=%" PRIu64, captured_frames,
             captured_frames == 0U ? 0U : analysis_total_us / captured_frames, analysis_max_us);
    ESP_LOGI(kTag, "voice handshake capture: buffered_ms=%" PRIu32 " preroll_ms=%" PRIu32 " overflow=%s",
             buffered_size * 1000U / kSampleRate, buffered_preroll * 1000U / kSampleRate,
             buffered_overflow ? "yes" : "no");
    if (cancel_.load(std::memory_order_acquire)) {
        input_.EndCapture(this);
        capture_owned_ = false;
        DestroyTransport();
        return;
    }
    input_.EndCapture(this);
    capture_owned_ = false;
    audio_.ResumeOutputAfterCapture(this);
    output_paused_for_capture_ = false;
    if (local_command != 0U) {
        local_command_pending_.store(local_command, std::memory_order_release);
        (void)FinishPendingLocalCommand();
        return;
    }
    if (!session_online_) {
        SetState(MICROPIXEL_VOICE_STATE_OFFLINE, MICROPIXEL_STATUS_WOULD_BLOCK);
        micropixel_voice_event_payload_t payload{};
        Emit(MICROPIXEL_VOICE_EVENT_SESSION_ENDED, MICROPIXEL_STATUS_WOULD_BLOCK, payload);
        DestroyTransport();
        return;
    }
    if (!FlushAudioSender()) {
        FinishSession(MICROPIXEL_STATUS_INTERNAL);
        DestroyTransport();
        return;
    }
    ESP_LOGI(kTag, "voice upload drained: frames=%" PRIu32 " queue_high_water=%" PRIu32,
             audio_sender_frames_sent_.load(std::memory_order_relaxed),
             audio_queue_high_water_.load(std::memory_order_relaxed));
    SetState(MICROPIXEL_VOICE_STATE_THINKING);
    const int commit_length = std::snprintf(
        commit_message_.data(), commit_message_.size(),
        "{\"type\":\"input.commit\",\"session_id\":%" PRIu32 ",\"generation\":%" PRIu32 "}", session_id_, generation_);
    if (commit_length <= 0 || static_cast<size_t>(commit_length) >= commit_message_.size() ||
        !SendText(commit_message_.data())) {
        FinishSession(MICROPIXEL_STATUS_INTERNAL);
        DestroyTransport();
        return;
    }
    for (uint32_t waited = 0U; !cancel_.load(std::memory_order_acquire) &&
                               !playback_finished_.load(std::memory_order_acquire) && waited < 60000U;
         waited += 20U) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (!cancel_.load(std::memory_order_acquire)) {
        FinishSession(playback_finished_.load(std::memory_order_acquire) ? MICROPIXEL_STATUS_OK
                                                                         : MICROPIXEL_STATUS_TIMEOUT);
    }
    DestroyTransport();
}

void VoiceEngine::WebsocketEvent(void* args, const char*, int32_t event_id, void* event_data) {
    if (args != nullptr) static_cast<VoiceEngine*>(args)->OnWebsocketEvent(event_id, event_data);
}

void VoiceEngine::OnWebsocketEvent(int32_t event_id, void* raw_event) {
    if (event_id == WEBSOCKET_EVENT_CONNECTED) {
        connected_.store(true, std::memory_order_release);
        return;
    }
    if (event_id == WEBSOCKET_EVENT_DISCONNECTED || event_id == WEBSOCKET_EVENT_CLOSED ||
        event_id == WEBSOCKET_EVENT_FINISH || event_id == WEBSOCKET_EVENT_ERROR) {
        connected_.store(false, std::memory_order_release);
        return;
    }
    if (event_id != WEBSOCKET_EVENT_DATA || raw_event == nullptr) return;
    auto* event = static_cast<esp_websocket_event_data_t*>(raw_event);
    if (event->data_ptr == nullptr || event->data_len <= 0) return;
    if (event->op_code == kWebsocketBinaryOpcode && event->fin) {
        HandleAudio(reinterpret_cast<const uint8_t*>(event->data_ptr), static_cast<size_t>(event->data_len));
    } else if (event->op_code == kWebsocketTextOpcode && event->fin) {
        HandleText(event->data_ptr, static_cast<size_t>(event->data_len));
    }
}

void VoiceEngine::HandleText(const char* bytes, size_t length) {
    if (length == 0U || length >= text_message_.size()) return;
    // The session thread may advance the identity while a session is being torn
    // down; evaluate every event against one consistent snapshot.
    uint32_t current_session_id = 0U;
    uint32_t current_generation = 0U;
    {
        std::lock_guard lock(mutex_);
        current_session_id = session_id_;
        current_generation = generation_;
    }
    std::memcpy(text_message_.data(), bytes, length);
    text_message_[length] = '\0';
    cJSON* root = cJSON_ParseWithLength(text_message_.data(), length);
    if (root == nullptr) return;
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const char* type_text = cJSON_IsString(type) ? type->valuestring : nullptr;
    if (type_text == nullptr) {
        cJSON_Delete(root);
        return;
    }
    const bool connection_event = std::strcmp(type_text, "ready") == 0 || std::strcmp(type_text, "pong") == 0;
    const cJSON* session_id = cJSON_GetObjectItemCaseSensitive(root, "session_id");
    const cJSON* generation = cJSON_GetObjectItemCaseSensitive(root, "generation");
    const bool has_identity = cJSON_IsNumber(session_id) && cJSON_IsNumber(generation) &&
                              session_id->valuedouble >= 1.0 && session_id->valuedouble <= 4294967295.0 &&
                              generation->valuedouble >= 1.0 && generation->valuedouble <= 4294967295.0 &&
                              std::floor(session_id->valuedouble) == session_id->valuedouble &&
                              std::floor(generation->valuedouble) == generation->valuedouble;
    const bool identity_matches = has_identity &&
                                  static_cast<uint32_t>(session_id->valuedouble) == current_session_id &&
                                  static_cast<uint32_t>(generation->valuedouble) == current_generation;
    // Handshake failures can be connection-scoped. Once an event carries a
    // session identity, never allow a late generation to mutate current state.
    if (!connection_event && std::strcmp(type_text, "error") != 0 && !identity_matches) {
        cJSON_Delete(root);
        return;
    }
    if (std::strcmp(type_text, "error") == 0 && has_identity && !identity_matches) {
        cJSON_Delete(root);
        return;
    }
    if (std::strcmp(type_text, "ready") == 0) {
        ready_.store(true, std::memory_order_release);
    } else if (std::strcmp(type_text, "session.started") == 0) {
        session_started_.store(true, std::memory_order_release);
    } else if (std::strcmp(type_text, "transcript.snapshot") == 0 || std::strcmp(type_text, "transcript.final") == 0) {
        const uint64_t started = session_started_us_.load(std::memory_order_acquire);
        uint32_t empty = 0U;
        const uint32_t elapsed =
            started == 0U ? 0U
                          : static_cast<uint32_t>(std::min<uint64_t>(
                                UINT32_MAX, (static_cast<uint64_t>(esp_timer_get_time()) - started) / 1000U));
        if (elapsed != 0U) (void)first_transcript_ms_.compare_exchange_strong(empty, elapsed);
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            ReplaceTranscript(text->valuestring, std::strlen(text->valuestring),
                              std::strcmp(type_text, "transcript.final") == 0);
        }
    } else if (std::strcmp(type_text, "response.text.delta") == 0) {
        const uint64_t started = session_started_us_.load(std::memory_order_acquire);
        uint32_t empty = 0U;
        const uint32_t elapsed =
            started == 0U ? 0U
                          : static_cast<uint32_t>(std::min<uint64_t>(
                                UINT32_MAX, (static_cast<uint64_t>(esp_timer_get_time()) - started) / 1000U));
        if (elapsed != 0U) (void)first_response_text_ms_.compare_exchange_strong(empty, elapsed);
        const cJSON* text = cJSON_GetObjectItemCaseSensitive(root, "text");
        if (cJSON_IsString(text)) {
            AppendText(MICROPIXEL_VOICE_TEXT_RESPONSE, text->valuestring, std::strlen(text->valuestring), false);
        }
    } else if (std::strcmp(type_text, "response.text.final") == 0) {
        AppendText(MICROPIXEL_VOICE_TEXT_RESPONSE, nullptr, 0U, true);
    } else if (std::strcmp(type_text, "response.audio.start") == 0) {
        SetState(MICROPIXEL_VOICE_STATE_SPEAKING);
    } else if (std::strcmp(type_text, "response.audio.stop") == 0) {
        server_audio_finished_.store(true, std::memory_order_release);
        bool empty = false;
        {
            std::lock_guard lock(mutex_);
            empty = playback_size_ == 0U;
        }
        if (empty) {
            playback_finished_.store(true, std::memory_order_release);
        } else {
            StartBufferedPlayback();
        }
    } else if (std::strcmp(type_text, "session.ended") == 0) {
        server_audio_finished_.store(true, std::memory_order_release);
        playback_finished_.store(true, std::memory_order_release);
    } else if (std::strcmp(type_text, "error") == 0) {
        SetState(MICROPIXEL_VOICE_STATE_ERROR, MICROPIXEL_STATUS_INTERNAL);
        server_audio_finished_.store(true, std::memory_order_release);
        playback_finished_.store(true, std::memory_order_release);
    }
    cJSON_Delete(root);
}

void VoiceEngine::HandleAudio(const uint8_t* bytes, size_t length) {
    // Snapshot the session identity and reorder state once. The session thread
    // rewrites them while a session is being torn down, so reading the members
    // directly from the WebSocket thread would race. The decoder pointer stays
    // valid outside the lock because DestroyTransport stops and joins the
    // WebSocket task before destroying the codec.
    OpusDecoder* decoder = nullptr;
    uint32_t current_session_id = 0U;
    uint32_t current_generation = 0U;
    bool sequence_valid = false;
    uint32_t expected_sequence = 0U;
    {
        std::lock_guard lock(mutex_);
        decoder = decoder_;
        current_session_id = session_id_;
        current_generation = generation_;
        sequence_valid = output_sequence_valid_;
        expected_sequence = expected_output_sequence_;
    }
    if (decoder == nullptr || length <= kAudioHeaderSize || bytes[0] != kProtocolVersion ||
        bytes[1] != kOutputAudioKind || bytes[2] != 0U || bytes[3] != 0U || Load32(bytes + 4U) != current_session_id ||
        Load32(bytes + 8U) != current_generation || Load32(bytes + 24U) != length - kAudioHeaderSize) {
        return;
    }
    const uint64_t started = session_started_us_.load(std::memory_order_acquire);
    uint32_t empty = 0U;
    const uint32_t elapsed = started == 0U
                                 ? 0U
                                 : static_cast<uint32_t>(std::min<uint64_t>(
                                       UINT32_MAX, (static_cast<uint64_t>(esp_timer_get_time()) - started) / 1000U));
    if (elapsed != 0U) (void)first_audio_ms_.compare_exchange_strong(empty, elapsed);
    const uint32_t sequence = Load32(bytes + 12U);
    int decoded_frames = 0;
    if (sequence_valid && sequence != expected_sequence) {
        decoded_frames = opus_decode(decoder, nullptr, 0, decode_workspace_.data(), kFrameSamples, 0);
    }
    if (decoded_frames <= 0) {
        decoded_frames = opus_decode(decoder, bytes + kAudioHeaderSize, length - kAudioHeaderSize,
                                     decode_workspace_.data(), kFrameSamples, 0);
    }
    if (decoded_frames <= 0) return;
    bool should_start = false;
    {
        std::lock_guard lock(mutex_);
        expected_output_sequence_ = sequence + 1U;
        output_sequence_valid_ = true;
        if (playback_size_ + static_cast<uint32_t>(decoded_frames) > kPlaybackCapacity) {
            playback_read_ = (playback_read_ + decoded_frames) % kPlaybackCapacity;
            playback_size_ -= decoded_frames;
            ++playback_overflow_count_;
        }
        for (int index = 0; index < decoded_frames; ++index) {
            playback_[playback_write_] = decode_workspace_[index];
            playback_write_ = (playback_write_ + 1U) % kPlaybackCapacity;
        }
        playback_size_ += decoded_frames;
        ++playback_packets_;
        playback_high_water_ = std::max(playback_high_water_, playback_size_);
        should_start = !playback_started_ && playback_size_ >= kPlaybackStartFrames;
        if (should_start) playback_started_ = true;
    }
    if (should_start) StartBufferedPlayback();
}

void VoiceEngine::StartBufferedPlayback() {
    uint32_t owner = 0U;
    uint16_t gain = 1000U;
    {
        std::lock_guard lock(mutex_);
        if (playback_size_ == 0U || playback_handle_ != 0U) return;
        playback_started_ = true;
        owner = session_id_;
        gain = preferences_.response_gain_per_mille;
    }
    device::PcmStreamHandle handle = 0U;
    if (audio_.StartPcm({.read = ReadPlayback, .context = this}, owner, gain, handle) == MICROPIXEL_STATUS_OK) {
        std::lock_guard lock(mutex_);
        playback_handle_ = handle;
    } else {
        server_audio_finished_.store(true, std::memory_order_release);
        playback_finished_.store(true, std::memory_order_release);
        SetState(MICROPIXEL_VOICE_STATE_ERROR, MICROPIXEL_STATUS_WOULD_BLOCK);
    }
}

bool VoiceEngine::SendText(const char* text) {
    return websocket_ != nullptr && connected_.load(std::memory_order_acquire) &&
           esp_websocket_client_send_text(websocket_, text, std::strlen(text), pdMS_TO_TICKS(kSendTimeoutMs)) > 0;
}

bool VoiceEngine::SendAudio(const uint8_t* packet, size_t length, uint32_t sequence, uint64_t timestamp_us) {
    if (length == 0U || length > 1500U) return false;
    std::array<uint8_t, kAudioHeaderSize + 512U> wire{};
    if (length > wire.size() - kAudioHeaderSize) return false;
    // The sender task does not own session state; tag each packet with one
    // consistent identity snapshot.
    uint32_t session_id = 0U;
    uint32_t generation = 0U;
    {
        std::lock_guard lock(mutex_);
        session_id = session_id_;
        generation = generation_;
    }
    wire[0] = kProtocolVersion;
    wire[1] = kInputAudioKind;
    wire[2] = 0U;
    wire[3] = 0U;
    Store32(wire.data() + 4U, session_id);
    Store32(wire.data() + 8U, generation);
    Store32(wire.data() + 12U, sequence);
    Store64(wire.data() + 16U, timestamp_us);
    Store32(wire.data() + 24U, length);
    std::memcpy(wire.data() + kAudioHeaderSize, packet, length);
    return esp_websocket_client_send_bin(websocket_, reinterpret_cast<const char*>(wire.data()),
                                         kAudioHeaderSize + length, pdMS_TO_TICKS(kSendTimeoutMs)) > 0;
}

device::PcmReadResult VoiceEngine::ReadPlayback(void* context, int16_t* samples, uint32_t capacity) {
    return static_cast<VoiceEngine*>(context)->DrainPlayback(samples, capacity);
}

device::PcmReadResult VoiceEngine::DrainPlayback(int16_t* samples, uint32_t capacity) {
    std::lock_guard lock(mutex_);
    const uint32_t count = std::min(capacity, playback_size_);
    for (uint32_t index = 0U; index < count; ++index) {
        samples[index] = playback_[playback_read_];
        playback_read_ = (playback_read_ + 1U) % kPlaybackCapacity;
    }
    playback_size_ -= count;
    if (count < capacity && !server_audio_finished_.load(std::memory_order_acquire)) {
        ++playback_low_water_count_;
    }
    const bool finished = server_audio_finished_.load(std::memory_order_acquire) && playback_size_ == 0U;
    if (finished) {
        playback_handle_ = 0U;
        playback_finished_.store(true, std::memory_order_release);
    }
    return {.frames = count, .finished = finished};
}

void VoiceEngine::AppendText(uint8_t kind, const char* text, size_t length, bool final) {
    micropixel_voice_event_payload_t payload{};
    uint16_t event_id = 0U;
    {
        std::lock_guard lock(mutex_);
        char* destination = nullptr;
        uint32_t* used = nullptr;
        uint32_t capacity = 0U;
        uint32_t* revision = nullptr;
        uint8_t* flags = nullptr;
        if (kind == MICROPIXEL_VOICE_TEXT_TRANSCRIPT) {
            destination = transcript_.data();
            used = &transcript_length_;
            capacity = kTranscriptCapacity;
            revision = &transcript_revision_;
            flags = &transcript_flags_;
            event_id = MICROPIXEL_VOICE_EVENT_TRANSCRIPT_UPDATED;
        } else {
            destination = response_.data();
            used = &response_length_;
            capacity = kResponseCapacity;
            revision = &response_revision_;
            flags = &response_flags_;
            event_id = MICROPIXEL_VOICE_EVENT_RESPONSE_UPDATED;
        }
        const size_t copied = std::min<size_t>(length, capacity - *used);
        if (copied != 0U && text != nullptr) std::memcpy(destination + *used, text, copied);
        *used += copied;
        destination[*used] = '\0';
        if (copied != length) *flags |= MICROPIXEL_VOICE_TEXT_TRUNCATED;
        if (final) *flags |= MICROPIXEL_VOICE_TEXT_FINAL;
        ++*revision;
        payload.kind = kind;
        payload.flags = *flags;
        payload.revision = *revision;
        payload.total_length = *used;
    }
    Emit(event_id, MICROPIXEL_STATUS_OK, payload);
}

void VoiceEngine::ReplaceTranscript(const char* text, size_t length, bool final) {
    micropixel_voice_event_payload_t payload{};
    {
        std::lock_guard lock(mutex_);
        const size_t copied = std::min<size_t>(length, kTranscriptCapacity);
        transcript_.fill('\0');
        if (copied != 0U && text != nullptr) std::memcpy(transcript_.data(), text, copied);
        transcript_length_ = copied;
        transcript_flags_ =
            copied != length ? static_cast<uint8_t>(MICROPIXEL_VOICE_TEXT_TRUNCATED) : static_cast<uint8_t>(0U);
        if (final) transcript_flags_ |= MICROPIXEL_VOICE_TEXT_FINAL;
        ++transcript_revision_;
        payload.kind = MICROPIXEL_VOICE_TEXT_TRANSCRIPT;
        payload.flags = transcript_flags_;
        payload.revision = transcript_revision_;
        payload.total_length = transcript_length_;
    }
    Emit(MICROPIXEL_VOICE_EVENT_TRANSCRIPT_UPDATED, MICROPIXEL_STATUS_OK, payload);
}

void VoiceEngine::ResetText() {
    transcript_.fill('\0');
    response_.fill('\0');
    transcript_length_ = 0U;
    response_length_ = 0U;
    transcript_flags_ = 0U;
    response_flags_ = 0U;
    ++transcript_revision_;
    ++response_revision_;
    playback_read_ = 0U;
    playback_write_ = 0U;
    playback_size_ = 0U;
    playback_low_water_count_ = 0U;
    playback_overflow_count_ = 0U;
    playback_high_water_ = 0U;
    playback_packets_ = 0U;
    output_sequence_valid_ = false;
    playback_started_ = false;
    server_audio_finished_.store(false, std::memory_order_release);
    playback_finished_.store(false, std::memory_order_release);
    connected_.store(false, std::memory_order_release);
    ready_.store(false, std::memory_order_release);
    session_started_.store(false, std::memory_order_release);
}

void VoiceEngine::SetState(uint8_t state, int32_t status) {
    last_status_.store(status, std::memory_order_release);
    {
        std::lock_guard lock(mutex_);
        state_ = state;
    }
    micropixel_voice_event_payload_t payload{};
    payload.value = state;
    Emit(MICROPIXEL_VOICE_EVENT_STATE_CHANGED, status, payload);
}

void VoiceEngine::Emit(uint16_t event_id, int32_t status, const micropixel_voice_event_payload_t& payload) {
    device::VoiceUpdateSink sink = nullptr;
    void* context = nullptr;
    {
        std::lock_guard lock(mutex_);
        sink = sink_;
        context = sink_context_;
    }
    if (sink != nullptr)
        sink(context, {.event_id = event_id,
                       .status = status,
                       .payload = payload,
                       .timestamp_us = static_cast<uint64_t>(esp_timer_get_time())});
}

void VoiceEngine::FinishSession(int32_t status) {
    {
        std::lock_guard lock(mutex_);
        if (playback_packets_ != 0U) {
            ESP_LOGI(kTag,
                     "voice playback: packets=%" PRIu32 " low_water=%" PRIu32 " overflow=%" PRIu32
                     " high_water_ms=%" PRIu32,
                     playback_packets_, playback_low_water_count_, playback_overflow_count_,
                     playback_high_water_ * 1000U / kSampleRate);
        }
    }
    if (status == MICROPIXEL_STATUS_OK || status == MICROPIXEL_STATUS_CANCELLED) {
        SetState(MICROPIXEL_VOICE_STATE_IDLE, status);
    } else {
        SetState(MICROPIXEL_VOICE_STATE_ERROR, status);
    }
    micropixel_voice_event_payload_t payload{};
    Emit(MICROPIXEL_VOICE_EVENT_SESSION_ENDED, status, payload);
}

void VoiceEngine::CompleteLocalCommand(uint8_t command_id) {
    int32_t status = MICROPIXEL_STATUS_OK;
    uint8_t volume_percent = 0U;
    if (command_id == MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_UP) {
        status = audio_.AdjustMasterVolumePercent(kLocalVolumeStepPercent, volume_percent);
    } else if (command_id == MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_DOWN) {
        status = audio_.AdjustMasterVolumePercent(-kLocalVolumeStepPercent, volume_percent);
    }
    if (command_id == MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_UP ||
        command_id == MICROPIXEL_VOICE_LOCAL_COMMAND_VOLUME_DOWN) {
        ESP_LOGI(kTag, "local volume command: id=%u volume=%u%% status=%" PRId32, static_cast<unsigned>(command_id),
                 static_cast<unsigned>(volume_percent), status);
    }
    EmitLocalCommand(command_id, status);
}

bool VoiceEngine::FinishPendingLocalCommand() {
    const uint8_t command_id = local_command_pending_.exchange(0U, std::memory_order_acq_rel);
    if (command_id == 0U) return false;
    uint32_t session_id = 0U;
    uint32_t generation = 0U;
    {
        std::lock_guard lock(mutex_);
        session_id = session_id_;
        generation = generation_;
        // Cancel carries the identity of the abandoned generation, then the
        // counter advances so late events for it are rejected.
        ++generation_;
        if (generation_ == 0U) ++generation_;
    }
    if (session_online_ && websocket_ != nullptr && connected_.load(std::memory_order_acquire)) {
        std::array<char, 128U> message{};
        const int length =
            std::snprintf(message.data(), message.size(),
                          "{\"type\":\"session.cancel\",\"session_id\":%" PRIu32 ",\"generation\":%" PRIu32 "}",
                          session_id, generation);
        if (length > 0 && static_cast<size_t>(length) < message.size()) {
            (void)esp_websocket_client_send_text(websocket_, message.data(), length, pdMS_TO_TICKS(100));
        }
    }
    DestroyTransport();
    FinishSession(MICROPIXEL_STATUS_OK);
    CompleteLocalCommand(command_id);
    return true;
}

void VoiceEngine::EmitLocalCommand(uint8_t command_id, int32_t status) {
    micropixel_voice_event_payload_t payload{};
    payload.revision = generation_;
    payload.value = command_id;
    Emit(MICROPIXEL_VOICE_EVENT_LOCAL_COMMAND, status, payload);
}

void VoiceEngine::DestroyTransport() {
    {
        std::lock_guard lock(mutex_);
        handshake_capture_active_ = false;
        handshake_capture_size_ = 0U;
        handshake_preroll_size_ = 0U;
    }
    if (capture_owned_) {
        input_.EndCapture(this);
        capture_owned_ = false;
    }
    if (output_paused_for_capture_) {
        audio_.ResumeOutputAfterCapture(this);
        output_paused_for_capture_ = false;
    }
    StopAudioSender();
    esp_websocket_client_handle_t websocket = nullptr;
    {
        std::lock_guard lock(mutex_);
        websocket = websocket_;
        websocket_ = nullptr;
    }
    if (websocket != nullptr) {
        // stop() waits for the WebSocket task to finish, so no callback can be
        // running on the codecs below once it returns.
        (void)esp_websocket_client_stop(websocket);
        (void)esp_websocket_client_destroy(websocket);
    }
    connected_.store(false, std::memory_order_release);
    OpusEncoder* encoder = nullptr;
    OpusDecoder* decoder = nullptr;
    {
        std::lock_guard lock(mutex_);
        encoder = encoder_;
        encoder_ = nullptr;
        decoder = decoder_;
        decoder_ = nullptr;
    }
    if (encoder != nullptr) opus_encoder_destroy(encoder);
    if (decoder != nullptr) opus_decoder_destroy(decoder);
    if (network_realtime_) {
        (void)network_.WifiControl().SetRealtimeMode(false);
        network_realtime_ = false;
    }
    if (afe_handle_ != nullptr && afe_data_ != nullptr) (void)afe_handle_->reset_buffer(afe_data_);
    capture_takeover_requested_.store(false, std::memory_order_release);
}

}  // namespace micropixel::host::voice
