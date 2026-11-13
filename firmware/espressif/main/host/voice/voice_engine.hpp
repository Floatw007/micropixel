#ifndef MICROPIXEL_HOST_VOICE_VOICE_ENGINE_HPP
#define MICROPIXEL_HOST_VOICE_VOICE_ENGINE_HPP

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>

#include "device/contracts/audio.hpp"
#include "device/contracts/audio_input.hpp"
#include "device/contracts/voice.hpp"
#include "sdkconfig.h"
#if CONFIG_IDF_TARGET_ESP32P4
#include "esp_afe_sr_iface.h"
#include "esp_mn_iface.h"
#include "model_path.h"
#endif
#include "esp_websocket_client.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "host/network/network.hpp"
#include "opus.h"

namespace micropixel::host::voice {

inline constexpr uint32_t kVoiceProfileMagic = 0x3158504dU;  // MPX1
inline constexpr uint16_t kVoiceProfileVersion = 1U;
inline constexpr uint16_t kVoiceProfileAllowInsecure = 1U << 0U;

struct VoiceProfileWireHeader final {
    uint32_t magic{};
    uint16_t version{};
    uint16_t size{};
    uint32_t revision{};
    uint16_t url_length{};
    uint16_t ca_length{};
    uint16_t token_length{};
    uint16_t flags{};
    uint32_t reserved0{};
};
static_assert(sizeof(VoiceProfileWireHeader) == 24U);

struct VoiceProfileStatus final {
    bool configured{};
    bool allow_insecure{};
    uint32_t revision{};
    std::array<char, 257U> url{};
    std::array<char, 65U> certificate_fingerprint{};
    std::array<char, 9U> token_suffix{};
};

class VoiceEngine final : public device::Voice {
   public:
    VoiceEngine(device::Audio& audio, device::AudioInput& input, network::Network& network);
    VoiceEngine(const VoiceEngine&) = delete;
    VoiceEngine& operator=(const VoiceEngine&) = delete;
    ~VoiceEngine() override;

    [[nodiscard]] bool valid() const { return mutex_ready_; }
    [[nodiscard]] int32_t Configure(std::span<const uint8_t> bytes);
    [[nodiscard]] int32_t ClearProfile();
    [[nodiscard]] VoiceProfileStatus ProfileStatus() const;

    [[nodiscard]] int32_t GetInfo(micropixel_voice_info_t& info) const override;
    [[nodiscard]] int32_t StartSession() override;
    [[nodiscard]] int32_t CancelSession() override;
    [[nodiscard]] int32_t GetState(micropixel_voice_state_response_t& state) const override;
    [[nodiscard]] int32_t ReadText(uint8_t kind, uint32_t revision, uint32_t offset, std::span<uint8_t> destination,
                                   device::VoiceTextResult& result) const override;
    [[nodiscard]] int32_t GetPreferences(micropixel_voice_preferences_t& preferences) const override;
    [[nodiscard]] int32_t SetPreferences(const micropixel_voice_set_preferences_request_t& preferences) override;
    [[nodiscard]] int32_t GetDiagnostics(micropixel_voice_diagnostics_t& diagnostics) const override;
    [[nodiscard]] int32_t RunDiagnostic(uint8_t kind) override;
    [[nodiscard]] int32_t ClearContext() override;
    void BindUpdateSink(device::VoiceUpdateSink sink, void* context) override;
    void UnbindUpdateSink(void* context) override;

   private:
    static constexpr uint32_t kSampleRate = 16000U;
    static constexpr uint32_t kFrameSamples = 320U;
    static constexpr uint32_t kCaptureReadFrames = MICROPIXEL_AUDIO_INPUT_MAX_READ_FRAMES;
    static constexpr uint32_t kPlaybackCapacity = 16000U;
    static constexpr uint32_t kPlaybackStartFrames = 3200U;
    static constexpr uint32_t kPrerollFrames = 6400U;
    static constexpr uint32_t kHandshakeCaptureFrames = kSampleRate * 21U;
    static constexpr uint32_t kSpeechFeedCapacity = 1024U;
    static constexpr uint32_t kMultiNetChunkCapacity = 1024U;
    static constexpr uint32_t kTranscriptCapacity = 512U;
    static constexpr uint32_t kResponseCapacity = 4096U;
    static constexpr uint32_t kAudioQueueCapacity = 256U;

    struct QueuedAudioFrame final {
        uint64_t timestamp_us{};
        uint32_t sequence{};
        uint16_t sample_count{};
        uint16_t reserved{};
        std::array<int16_t, kFrameSamples> samples{};
    };

    struct Profile final {
        uint32_t magic{};
        uint32_t revision{};
        uint16_t flags{};
        uint16_t url_length{};
        uint16_t ca_length{};
        uint16_t token_length{};
        std::array<char, 257U> url{};
        std::array<char, 4097U> ca_pem{};
        std::array<char, 1025U> token{};
    };

    static void TaskEntry(void* context);
    static void AudioSenderTaskEntry(void* context);
    static void SpeechFeedTaskEntry(void* context);
    static void SpeechDetectTaskEntry(void* context);
    void RunSession();
    void RunAudioSender();
    void RunSpeechFeed();
    void RunSpeechDetect();
    [[nodiscard]] bool InitializeSpeechRecognition();
    [[nodiscard]] bool InitializeLocalCommands();
    void ShutdownSpeechRecognition();
    [[nodiscard]] uint8_t FeedLocalCommands(const int16_t* samples, uint32_t frames);
    void ResetLocalCommands();
    void CompleteLocalCommand(uint8_t command_id);
    void EmitLocalCommand(uint8_t command_id, int32_t status);
    [[nodiscard]] bool FinishPendingLocalCommand();
    void ArmHandshakeCaptureLocked();
    static void WebsocketEvent(void* args, const char* base, int32_t event_id, void* event_data);
    void OnWebsocketEvent(int32_t event_id, void* event_data);
    void HandleText(const char* bytes, size_t length);
    void HandleAudio(const uint8_t* bytes, size_t length);
    [[nodiscard]] bool SendText(const char* text);
    [[nodiscard]] bool SendAudio(const uint8_t* packet, size_t length, uint32_t sequence, uint64_t timestamp_us);
    [[nodiscard]] bool StartAudioSender();
    [[nodiscard]] bool QueueAudio(const int16_t* samples, uint32_t sequence, uint64_t timestamp_us);
    [[nodiscard]] bool FlushAudioSender();
    void StopAudioSender();
    static device::PcmReadResult ReadPlayback(void* context, int16_t* samples, uint32_t capacity);
    device::PcmReadResult DrainPlayback(int16_t* samples, uint32_t capacity);
    void StartBufferedPlayback();
    void AppendText(uint8_t kind, const char* text, size_t length, bool final);
    void ReplaceTranscript(const char* text, size_t length, bool final);
    void ResetText();
    void SetState(uint8_t state, int32_t status = MICROPIXEL_STATUS_OK);
    void Emit(uint16_t event_id, int32_t status, const micropixel_voice_event_payload_t& payload);
    void FinishSession(int32_t status);
    [[nodiscard]] bool LoadProfile();
    void LoadPreferences();
    [[nodiscard]] int32_t SavePreferences(const micropixel_voice_preferences_t& preferences) const;
    [[nodiscard]] bool RouteOnline() const;
    void DestroyTransport();

    device::Audio& audio_;
    device::AudioInput& input_;
    network::Network& network_;
    mutable std::mutex mutex_;
    bool mutex_ready_{true};
    Profile profile_{};
    Profile profile_staging_{};
    micropixel_voice_preferences_t preferences_{};
    std::array<char, kTranscriptCapacity + 1U> transcript_{};
    std::array<char, kResponseCapacity + 1U> response_{};
    uint32_t transcript_length_{};
    uint32_t response_length_{};
    uint32_t transcript_revision_{};
    uint32_t response_revision_{};
    uint8_t transcript_flags_{};
    uint8_t response_flags_{};
    uint8_t state_{MICROPIXEL_VOICE_STATE_IDLE};
    uint16_t input_level_{};
    uint32_t session_id_{};
    uint32_t generation_{};
    device::VoiceUpdateSink sink_{};
    void* sink_context_{};

    std::atomic<bool> cancel_{};
    std::atomic<bool> connected_{};
    std::atomic<bool> ready_{};
    std::atomic<bool> session_started_{};
    std::atomic<bool> server_audio_finished_{};
    std::atomic<bool> playback_finished_{};
    std::atomic<bool> task_running_{};
    std::atomic<bool> speech_running_{};
    std::atomic<uint32_t> speech_feed_generation_{};
    std::atomic<bool> speech_feed_healthy_{};
    std::atomic<bool> capture_takeover_requested_{};
    std::atomic<bool> speech_feed_paused_{};
    std::atomic<bool> speech_detect_paused_{};
    std::atomic<bool> audio_sender_running_{};
    std::atomic<bool> audio_sender_failed_{};
    std::atomic<bool> audio_sender_stop_{};
    std::atomic<uint8_t> local_command_pending_{};
    std::atomic<uint32_t> audio_sender_frames_sent_{};
    std::atomic<uint32_t> audio_queue_high_water_{};
    std::atomic<uint32_t> audio_queue_dropped_{};
    std::atomic<bool> local_commands_enabled_{true};
    std::atomic<uint64_t> session_started_us_{};
    std::atomic<uint32_t> first_transcript_ms_{};
    std::atomic<uint32_t> first_response_text_ms_{};
    std::atomic<uint32_t> first_audio_ms_{};
    std::atomic<int32_t> last_status_{MICROPIXEL_STATUS_OK};
    esp_websocket_client_handle_t websocket_{};
    StaticQueue_t audio_queue_state_{};
    std::array<uint8_t, sizeof(QueuedAudioFrame) * kAudioQueueCapacity> audio_queue_storage_{};
    QueueHandle_t audio_queue_{};
    OpusEncoder* encoder_{};
    OpusDecoder* decoder_{};
    device::PcmStreamHandle playback_handle_{};
    std::array<int16_t, kPlaybackCapacity> playback_{};
    uint32_t playback_read_{};
    uint32_t playback_write_{};
    uint32_t playback_size_{};
    uint32_t playback_low_water_count_{};
    uint32_t playback_overflow_count_{};
    uint32_t playback_high_water_{};
    uint32_t playback_packets_{};
    uint32_t expected_output_sequence_{};
    bool output_sequence_valid_{};
    bool playback_started_{};
    bool capture_owned_{};
    bool output_paused_for_capture_{};
    bool network_realtime_{};
    bool session_online_{};
    std::array<char, 8193U> text_message_{};
    std::array<char, 1100U> header_workspace_{};
    std::array<char, 128U> session_start_message_{};
    std::array<char, 128U> commit_message_{};
    std::array<int16_t, kCaptureReadFrames> capture_read_workspace_{};
    std::array<int16_t, kFrameSamples> capture_workspace_{};
    std::array<int16_t, kFrameSamples> decode_workspace_{};
    std::array<uint8_t, 512U> packet_workspace_{};
#if CONFIG_IDF_TARGET_ESP32P4
    const esp_afe_sr_iface_t* afe_handle_{};
    esp_afe_sr_data_t* afe_data_{};
    srmodel_list_t* speech_models_{};
    const esp_mn_iface_t* multinet_handle_{};
    model_iface_data_t* multinet_data_{};
#else
    void* afe_handle_{};
    void* afe_data_{};
    void* speech_models_{};
    void* multinet_handle_{};
    void* multinet_data_{};
#endif
    uint32_t multinet_chunk_frames_{};
    uint32_t multinet_workspace_size_{};
    std::array<int16_t, kMultiNetChunkCapacity> multinet_workspace_{};
    uint32_t speech_feed_frames_{};
    std::array<int16_t, kSpeechFeedCapacity> speech_feed_workspace_{};
    std::array<int16_t, kSpeechFeedCapacity> session_vad_workspace_{};
    std::array<int16_t, kPrerollFrames> preroll_{};
    std::array<int16_t, kHandshakeCaptureFrames> handshake_capture_{};
    uint32_t preroll_write_{};
    uint32_t preroll_size_{};
    uint32_t handshake_capture_write_{};
    uint32_t handshake_capture_size_{};
    uint32_t handshake_preroll_size_{};
    bool handshake_capture_active_{};
    bool handshake_capture_overflow_{};
};

}  // namespace micropixel::host::voice

#endif
