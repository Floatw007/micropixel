// bear: an interactive spring-driven vector pet.
//
// The body uses the ESP32-P4 hardware benchmark's chosen quality tier:
// 96 vertices, a one-pixel palette-interpolated AA fringe and two buffers.

#include <stdint.h>

#include "bear_sfx_profiles.hpp"
#include "sdk/micropixel.hpp"
#include "sdk/tone_sequencer.hpp"

namespace {

namespace mp = micropixel;

constexpr uint32_t kPaletteRows = 32U;
constexpr uint8_t kPaletteSlot = 0U;
constexpr uint32_t kBufferCount = 2U;
constexpr uint32_t kBodyVertices = 96U;
constexpr uint32_t kEyeVertices = 48U;
constexpr uint32_t kStatsFrames = 120U;
constexpr uint64_t kSettingsHoldUs = 5000000ULL;
constexpr uint64_t kSettingsProgressDelayUs = 500000ULL;
constexpr float kHoldMoveCancelSquared = 400.0F;
constexpr char kSettingsKey[] = "settings_v1";
constexpr uint32_t kSettingsMagic = 0x31544542U;  // BET1
constexpr uint16_t kSettingsVersion = 1U;

constexpr mp::Color kBackground = mp::Color::Rgb(247U, 245U, 240U);
constexpr mp::Color kBody = mp::Color::Rgb(10U, 10U, 11U);
constexpr mp::Color kEye = mp::Color::Rgb(247U, 245U, 240U);
constexpr mp::Color kPanel = mp::Color::Rgb(255U, 253U, 249U);
constexpr mp::Color kSidebar = mp::Color::Rgb(30U, 30U, 32U);
constexpr mp::Color kSidebarMuted = mp::Color::Rgb(174U, 171U, 164U);
constexpr mp::Color kInk = mp::Color::Rgb(24U, 24U, 25U);
constexpr mp::Color kMuted = mp::Color::Rgb(105U, 101U, 94U);
constexpr mp::Color kLine = mp::Color::Rgb(221U, 216U, 207U);
constexpr mp::Color kGood = mp::Color::Rgb(44U, 145U, 91U);

constexpr uint8_t kBodyColor = 1U;
constexpr uint8_t kEyeColor = 2U;
constexpr uint8_t kHoldColor = 3U;

using Line = mp::FixedString<240U>;

struct Vector final {
    float x{};
    float y{};
};

struct Spring final {
    float x{};
    float velocity{};
    float target{};
};

enum class Mood : uint8_t {
    kIdle,
    kListening,
    kCurious,
    kExcited,
    kDrowsy,
    kSleeping,
    kWaking,
};

enum class TouchRegion : uint8_t {
    kOutside,
    kLeftEye,
    kRightEye,
    kUpperBody,
    kLowerBody,
};

enum class View : uint8_t {
    kPet,
    kSettings,
};

enum class SettingsPage : uint8_t {
    kOverview,
    kBehavior,
    kVoice,
    kCommands,
    kPrivacy,
    kDiagnostics,
    kAbout,
    kCount,
};

enum PreferenceFlag : uint8_t {
    kTouchSounds = 1U << 0U,
    kCuriousReaction = 1U << 1U,
    kWakeCue = 1U << 2U,
    kLocalCommands = 1U << 3U,
    // Legacy stored bit from settings v1. Response streaming is a Host capability with no Guest-side consumer,
    // so it is no longer a setting; keep accepting the bit so existing stores remain valid.
    kStreamingResponse = 1U << 4U,
};

struct Preferences final {
    uint32_t magic{kSettingsMagic};
    uint16_t version{kSettingsVersion};
    uint16_t size{sizeof(Preferences)};
    uint16_t idle_timeout_seconds{40U};
    uint16_t answer_gain_per_mille{300U};
    uint8_t animation_percent{65U};
    uint8_t flags{kTouchSounds | kCuriousReaction | kWakeCue | kLocalCommands};
    uint16_t reserved{};
};
static_assert(sizeof(Preferences) == 16U);

float Clamp(float value, float low, float high) {
    if (value < low) return low;
    if (value > high) return high;
    return value;
}

uint8_t MixChannel(uint8_t from, uint8_t to, uint32_t numerator, uint32_t denominator) {
    return static_cast<uint8_t>((static_cast<uint32_t>(from) * (denominator - numerator) +
                                 static_cast<uint32_t>(to) * numerator + denominator / 2U) /
                                denominator);
}

mp::Color Mix(mp::Color from, mp::Color to, uint32_t numerator, uint32_t denominator) {
    return mp::Color::Rgb(MixChannel(from.red(), to.red(), numerator, denominator),
                          MixChannel(from.green(), to.green(), numerator, denominator),
                          MixChannel(from.blue(), to.blue(), numerator, denominator));
}

void StepSpring(Spring& spring, float frequency, float damping, float dt) {
    spring.velocity +=
        (-2.0F * damping * frequency * spring.velocity - frequency * frequency * (spring.x - spring.target)) * dt;
    spring.x += spring.velocity * dt;
}

class Bear final {
   public:
    int Run() {
        if (!app_.renderer().ConfigureDisplay({}).has_value()) return Fail("bear: configure display failed");
        const mp::RendererInfo renderer = app_.renderer().info();
        if (!renderer.raster_supported() || !renderer.polygon_supported()) {
            return Fail("bear: Host raster polygon capability required");
        }

        auto resources = app_.renderer().CreateRasterResources();
        if (!resources.has_value()) return Fail("bear: raster resources unavailable");
        resources_ = resources.value();
        BuildPalette();
        if (!resources_.UploadLitPalette(kPaletteSlot, kPaletteRows, palette_).has_value()) {
            return Fail("bear: palette upload failed");
        }

        auto surface = app_.renderer().CreateHostSurface(kBufferCount, 1U);
        if (!surface.has_value()) return Fail("bear: HostSurface creation failed");
        surface_ = static_cast<mp::HostSurface&&>(surface.value());
        width_ = static_cast<int32_t>(surface_.buffer_width());
        height_ = static_cast<int32_t>(surface_.buffer_height());
        const auto audio = app_.audio().info();
        tones_.set_enabled(audio.has_value() && audio.value().Supports(mp::Waveform::kSine) &&
                           audio.value().Supports(mp::Waveform::kTriangle));
        const auto voice = app_.voice().info();
        voice_available_ = voice.has_value() && voice.value().streaming_text && voice.value().streaming_audio;
        if (voice.has_value()) voice_info_ = voice.value();
        LoadPreferences();
        SyncVoicePreferences();
        ResetPose();
        LogSetup(renderer);

        for (;;) {
            if (!DrainEvents()) break;
            uint32_t buffer = 0U;
            const uint64_t wait_started = app_.clock().Now().microseconds();
            while (!surface_.AcquireFree(buffer)) {
                if (!HandleEvent(app_.WaitEvent())) {
                    surface_.Reset();
                    return 0;
                }
            }
            const uint64_t frame_started = app_.clock().Now().microseconds();
            Advance(frame_started);
            BuildGeometry(frame_started);
            const uint64_t geometry_done = app_.clock().Now().microseconds();
            bool drawn = false;
            auto updated = surface_.Update(buffer, [&](mp::RasterDrawList& list) { drawn = Draw(list, buffer); });
            const uint64_t update_done = app_.clock().Now().microseconds();
            if (!drawn || !updated.has_value() || !surface_.Present(buffer).has_value()) {
                return Fail("bear: frame rejected");
            }
            const uint64_t present_done = app_.clock().Now().microseconds();
            RecordStats(wait_started, frame_started, geometry_done, update_done, present_done);
        }
        surface_.Reset();
        return 0;
    }

   private:
    int Fail(const char* message) {
        app_.log().Error(message);
        return 1;
    }

    static bool ValidPreferences(const Preferences& preferences) {
        // Accept the legacy streaming bit so settings stored by earlier builds remain valid.
        constexpr uint8_t kKnownFlags =
            kTouchSounds | kCuriousReaction | kWakeCue | kLocalCommands | kStreamingResponse;
        return preferences.magic == kSettingsMagic && preferences.version == kSettingsVersion &&
               preferences.size == sizeof(Preferences) &&
               (preferences.idle_timeout_seconds == 30U || preferences.idle_timeout_seconds == 40U ||
                preferences.idle_timeout_seconds == 50U) &&
               preferences.answer_gain_per_mille <= 1000U && preferences.animation_percent <= 100U &&
               (preferences.flags & ~kKnownFlags) == 0U && preferences.reserved == 0U;
    }

    void LoadPreferences() {
        auto size = app_.storage().GetBytesSize(kSettingsKey);
        if (!size.has_value() || size.value() != sizeof(Preferences)) return;
        Preferences loaded{};
        auto value = app_.storage().GetBytes(kSettingsKey, reinterpret_cast<uint8_t*>(&loaded), sizeof(loaded));
        if (value.has_value() && value.value() == sizeof(loaded) && ValidPreferences(loaded)) {
            preferences_ = loaded;
        }
    }

    void SavePreferences() {
        preferences_.magic = kSettingsMagic;
        preferences_.version = kSettingsVersion;
        preferences_.size = sizeof(Preferences);
        preferences_.reserved = 0U;
        const auto saved = app_.storage().SetBytes(kSettingsKey, reinterpret_cast<const uint8_t*>(&preferences_),
                                                   sizeof(preferences_));
        if (!saved.has_value()) app_.log().Warning("bear: settings save failed");
        SyncVoicePreferences();
    }

    void SyncVoicePreferences() {
        if (!voice_available_) return;
        mp::VoicePreferenceUpdate update{};
        update.set_response_gain = true;
        update.set_local_commands = true;
        update.response_gain_per_mille = preferences_.answer_gain_per_mille;
        update.local_commands_enabled = PreferenceEnabled(kLocalCommands);
        (void)app_.voice().SetPreferences(update);
    }

    bool PreferenceEnabled(PreferenceFlag flag) const { return (preferences_.flags & flag) != 0U; }

    void TogglePreference(PreferenceFlag flag) {
        preferences_.flags ^= flag;
        SavePreferences();
    }

    void BuildPalette() {
        for (uint32_t level = 0U; level < kPaletteRows; ++level) {
            uint16_t* row = palette_ + level * mp::RasterResources::kPaletteEntries;
            for (uint32_t index = 0U; index < mp::RasterResources::kPaletteEntries; ++index) {
                row[index] = kBackground.rgb565();
            }
            row[kBodyColor] = Mix(kBackground, kBody, level, kPaletteRows - 1U).rgb565();
            row[kEyeColor] = Mix(kBody, kEye, level, kPaletteRows - 1U).rgb565();
            row[kHoldColor] = Mix(kBackground, kGood, level, kPaletteRows - 1U).rgb565();
        }
    }

    void ResetPose() {
        const float center_x = static_cast<float>(width_) * 0.5F;
        const float center_y = static_cast<float>(height_) * 0.49F;
        center_x_ = {center_x, 0.0F, center_x};
        center_y_ = {center_y, 0.0F, center_y};
        tilt_ = {};
        gaze_x_ = {};
        gaze_y_ = {};
        squash_ = {1.0F, 0.0F, 1.0F};
        happiness_ = {};
        blink_ = {1.0F, 0.0F, 1.0F};
        const uint64_t now = app_.clock().Now().microseconds();
        started_us_ = now;
        last_frame_us_ = now;
        last_interaction_us_ = now;
        mood_since_us_ = now;
        next_blink_us_ = now + 2600000ULL;
        stats_started_us_ = now;
        mood_ = Mood::kIdle;
        touch_region_ = TouchRegion::kOutside;
        active_touch_ = false;
        hold_candidate_ = false;
        view_ = View::kPet;
        for (uint32_t index = 0U; index < kBufferCount; ++index) {
            buffer_ready_[index] = false;
            previous_[index] = {};
        }
    }

    bool DrainEvents() {
        mp::Event event;
        while (app_.PollEvent(event)) {
            if (!HandleEvent(event)) return false;
        }
        return true;
    }

    bool HandleEvent(const mp::Event& event) {
        if (event.type() == mp::EventType::kStop) {
            if (voice_active_) (void)app_.voice().CancelSession();
            (void)tones_.StopAll();
            return false;
        }
        if (event.type() == mp::EventType::kResume) {
            const uint64_t now = app_.clock().Now().microseconds();
            last_frame_us_ = now;
            last_interaction_us_ = now;
            for (uint32_t index = 0U; index < kBufferCount; ++index) buffer_ready_[index] = false;
        }
        if (const mp::VoiceEvent* voice = event.voice()) OnVoice(*voice);
        if (const mp::TouchEvent* touch = event.touch()) OnTouch(*touch);
        if (const mp::KeyEvent* key = event.key()) {
            if ((key->code() == mp::KeyCode::kConfirm || key->code() == mp::KeyCode::kSouth) &&
                key->phase() == mp::KeyPhase::kDown) {
                const uint64_t now = app_.clock().Now().microseconds();
                last_interaction_us_ = now;
                SetMood(mood_ == Mood::kSleeping ? Mood::kWaking : Mood::kExcited, now);
                center_y_.velocity -= static_cast<float>(height_) * 0.34F;
            }
        }
        return true;
    }

    void OnTouch(const mp::TouchEvent& touch) {
        const mp::Point point = surface_.ToBuffer(touch.position());
        const uint64_t now = app_.clock().Now().microseconds();
        if (touch.phase() == mp::TouchPhase::kDown && voice_active_) {
            const bool restart = voice_state_ == mp::VoiceState::kSpeaking;
            (void)app_.voice().CancelSession();
            voice_active_ = false;
            restart_voice_ = restart;
            next_voice_retry_us_ = now + 100000ULL;
            if (view_ == View::kSettings) LeaveSettings(now);
            SetMood(Mood::kIdle, now);
            return;
        }
        if (view_ == View::kSettings) {
            OnSettingsTouch(touch, point, now);
            return;
        }
        if (touch.phase() == mp::TouchPhase::kDown && !active_touch_) {
            active_touch_ = true;
            touch_id_ = touch.id();
            touch_ = {static_cast<float>(point.x), static_cast<float>(point.y)};
            touch_origin_ = touch_;
            touch_region_ = RegionAt(touch_);
            touch_on_bear_ = touch_region_ != TouchRegion::kOutside;
            last_interaction_us_ = now;
            if (mood_ == Mood::kSleeping || mood_ == Mood::kDrowsy) {
                SetMood(Mood::kWaking, now);
                return;
            }
            if (touch_on_bear_ && mood_ == Mood::kIdle && voice_state_ == mp::VoiceState::kIdle) {
                hold_candidate_ = true;
                hold_started_us_ = now;
                return;
            }
            switch (touch_region_) {
                case TouchRegion::kOutside:
                    if (voice_available_ && app_.voice().StartSession().has_value()) {
                        voice_active_ = true;
                        voice_state_ = mp::VoiceState::kWoken;
                        SetTouchMood(Mood::kListening, now);
                    } else {
                        SetMood(Mood::kDrowsy, now);
                    }
                    break;
                case TouchRegion::kLeftEye:
                case TouchRegion::kRightEye:
                    wink_eye_ = touch_region_ == TouchRegion::kLeftEye ? 0U : 1U;
                    wink_started_us_ = now;
                    SetTouchMood(Mood::kExcited, now);
                    break;
                case TouchRegion::kUpperBody:
                    SetTouchMood(Mood::kExcited, now);
                    center_y_.velocity -= static_cast<float>(height_) * 0.30F;
                    squash_.velocity -= 0.55F;
                    break;
                case TouchRegion::kLowerBody:
                    SetTouchMood(Mood::kExcited, now);
                    center_y_.velocity -= static_cast<float>(height_) * 0.20F;
                    tilt_.velocity += touch_.x < center_x_.x ? 1.1F : -1.1F;
                    break;
            }
            return;
        }
        if (!active_touch_ || touch.id() != touch_id_) return;
        if (touch.phase() == mp::TouchPhase::kMove) {
            touch_ = {static_cast<float>(point.x), static_cast<float>(point.y)};
            touch_region_ = RegionAt(touch_);
            touch_on_bear_ = touch_region_ != TouchRegion::kOutside;
            last_interaction_us_ = now;
            const float dx = touch_.x - touch_origin_.x;
            const float dy = touch_.y - touch_origin_.y;
            if (dx * dx + dy * dy > kHoldMoveCancelSquared) {
                hold_candidate_ = false;
                if (mood_ != Mood::kWaking && PreferenceEnabled(kCuriousReaction)) SetTouchMood(Mood::kCurious, now);
            }
        } else if (touch.phase() == mp::TouchPhase::kUp || touch.phase() == mp::TouchPhase::kCancel) {
            const bool short_tap = hold_candidate_ && touch.phase() == mp::TouchPhase::kUp;
            const TouchRegion released_region = touch_region_;
            hold_candidate_ = false;
            active_touch_ = false;
            touch_on_bear_ = false;
            touch_region_ = TouchRegion::kOutside;
            last_interaction_us_ = now;
            if (short_tap) ApplyBearTap(released_region, now);
            if (!voice_active_ && (mood_ == Mood::kListening || mood_ == Mood::kCurious)) SetMood(Mood::kIdle, now);
        }
    }

    void ApplyBearTap(TouchRegion region, uint64_t now) {
        switch (region) {
            case TouchRegion::kLeftEye:
            case TouchRegion::kRightEye:
                wink_eye_ = region == TouchRegion::kLeftEye ? 0U : 1U;
                wink_started_us_ = now;
                SetTouchMood(Mood::kExcited, now);
                break;
            case TouchRegion::kUpperBody:
                SetTouchMood(Mood::kExcited, now);
                center_y_.velocity -= static_cast<float>(height_) * 0.30F;
                squash_.velocity -= 0.55F;
                break;
            case TouchRegion::kLowerBody:
                SetTouchMood(Mood::kExcited, now);
                center_y_.velocity -= static_cast<float>(height_) * 0.20F;
                tilt_.velocity += touch_.x < center_x_.x ? 1.1F : -1.1F;
                break;
            case TouchRegion::kOutside:
                break;
        }
    }

    void EnterSettings(uint64_t now) {
        hold_candidate_ = false;
        active_touch_ = false;
        touch_on_bear_ = false;
        touch_region_ = TouchRegion::kOutside;
        (void)tones_.StopAll();
        view_ = View::kSettings;
        settings_page_ = SettingsPage::kOverview;
        last_interaction_us_ = now;
        auto voice = app_.voice().info();
        if (voice.has_value()) voice_info_ = voice.value();
        auto diagnostics = app_.voice().diagnostics();
        if (diagnostics.has_value()) voice_diagnostics_ = diagnostics.value();
        for (uint32_t index = 0U; index < kBufferCount; ++index) buffer_ready_[index] = false;
    }

    void LeaveSettings(uint64_t now) {
        view_ = View::kPet;
        last_interaction_us_ = now;
        SetMood(Mood::kIdle, now);
        for (uint32_t index = 0U; index < kBufferCount; ++index) buffer_ready_[index] = false;
    }

    void CycleIdleTimeout() {
        preferences_.idle_timeout_seconds =
            preferences_.idle_timeout_seconds == 30U ? 40U : (preferences_.idle_timeout_seconds == 40U ? 50U : 30U);
        SavePreferences();
    }

    void OnSettingsTouch(const mp::TouchEvent& touch, mp::Point point, uint64_t now) {
        if (touch.phase() != mp::TouchPhase::kDown) return;
        last_interaction_us_ = now;
        if (point.x >= width_ - 170 && point.y < 76) {
            LeaveSettings(now);
            return;
        }
        constexpr int32_t kSidebarWidth = 235;
        constexpr int32_t kNavTop = 92;
        constexpr int32_t kNavHeight = 58;
        if (point.x < kSidebarWidth && point.y >= kNavTop) {
            const uint32_t index = static_cast<uint32_t>((point.y - kNavTop) / kNavHeight);
            if (index < static_cast<uint32_t>(SettingsPage::kCount)) {
                settings_page_ = static_cast<SettingsPage>(index);
            }
            return;
        }
        if (point.x < kSidebarWidth || point.y < 132) return;
        const uint32_t row = static_cast<uint32_t>((point.y - 132) / 70);
        switch (settings_page_) {
            case SettingsPage::kBehavior:
                if (row == 0U) CycleIdleTimeout();
                if (row == 1U) {
                    const uint8_t current = preferences_.animation_percent;
                    preferences_.animation_percent =
                        current < 50U ? 50U : (current < 75U ? 75U : (current < 100U ? 100U : 25U));
                    SavePreferences();
                }
                if (row == 2U) TogglePreference(kCuriousReaction);
                if (row == 3U) TogglePreference(kTouchSounds);
                break;
            case SettingsPage::kVoice:
                if (row == 0U) {
                    preferences_.answer_gain_per_mille =
                        preferences_.answer_gain_per_mille >= 1000U ? 100U : preferences_.answer_gain_per_mille + 100U;
                    SavePreferences();
                }
                if (row == 1U) TogglePreference(kWakeCue);
                break;
            case SettingsPage::kCommands:
                if (row == 0U) TogglePreference(kLocalCommands);
                break;
            case SettingsPage::kPrivacy:
                if (row == 1U) (void)app_.voice().ClearContext();
                break;
            case SettingsPage::kDiagnostics:
                if (row == 0U) (void)app_.voice().RunDiagnostic(mp::VoiceDiagnosticKind::kMicrophone);
                if (row == 1U) (void)app_.voice().RunDiagnostic(mp::VoiceDiagnosticKind::kSpeaker);
                {
                    auto diagnostics = app_.voice().diagnostics();
                    if (diagnostics.has_value()) voice_diagnostics_ = diagnostics.value();
                }
                break;
            case SettingsPage::kAbout:
                if (row == 2U && !app_.system().OpenSettings()) {
                    app_.log().Warning("bear: system settings unavailable");
                }
                break;
            case SettingsPage::kOverview:
            case SettingsPage::kCount:
                break;
        }
    }

    void OnVoice(const mp::VoiceEvent& event) {
        const uint64_t now = app_.clock().Now().microseconds();
        if (event.event_type() == mp::VoiceEventType::kStateChanged && event.state() != mp::VoiceState::kIdle) {
            hold_candidate_ = false;
        }
        if (event.event_type() == mp::VoiceEventType::kLocalCommand) {
            voice_active_ = false;
            voice_state_ = mp::VoiceState::kIdle;
            voice_level_ = 0U;
            last_interaction_us_ = now;
            switch (event.local_command()) {
                case mp::VoiceLocalCommand::kSleep:
                    SetMood(Mood::kSleeping, now);
                    break;
                case mp::VoiceLocalCommand::kWake:
                    SetMood(Mood::kWaking, now);
                    break;
                case mp::VoiceLocalCommand::kVolumeUp:
                case mp::VoiceLocalCommand::kVolumeDown:
                    SetMood(Mood::kExcited, now);
                    break;
            }
            return;
        }
        if (event.event_type() == mp::VoiceEventType::kInputLevel) {
            voice_level_ = event.input_level_per_mille();
            return;
        }
        if (event.event_type() == mp::VoiceEventType::kTranscriptUpdated ||
            event.event_type() == mp::VoiceEventType::kResponseUpdated) {
            RefreshText(event.text_kind(), event.revision(), event.total_length());
            return;
        }
        if (event.event_type() == mp::VoiceEventType::kSessionEnded) {
            voice_active_ = false;
            voice_state_ = mp::VoiceState::kIdle;
            voice_level_ = 0U;
            if (!restart_voice_) SetMood(Mood::kIdle, now);
            return;
        }
        if (event.event_type() != mp::VoiceEventType::kStateChanged && event.event_type() != mp::VoiceEventType::kError)
            return;
        voice_state_ = event.event_type() == mp::VoiceEventType::kError ? mp::VoiceState::kError : event.state();
        voice_active_ = voice_state_ == mp::VoiceState::kWoken || voice_state_ == mp::VoiceState::kListening ||
                        voice_state_ == mp::VoiceState::kThinking || voice_state_ == mp::VoiceState::kSpeaking;
        if (voice_active_ && view_ == View::kSettings) LeaveSettings(now);
        switch (voice_state_) {
            case mp::VoiceState::kWoken:
            case mp::VoiceState::kListening:
                SetMood(Mood::kListening, now);
                break;
            case mp::VoiceState::kThinking:
                SetMood(Mood::kCurious, now);
                break;
            case mp::VoiceState::kSpeaking:
                SetMood(Mood::kExcited, now);
                break;
            case mp::VoiceState::kOffline:
            case mp::VoiceState::kError:
                voice_active_ = false;
                SetMood(Mood::kDrowsy, now);
                break;
            case mp::VoiceState::kIdle:
                voice_active_ = false;
                if (!restart_voice_) SetMood(Mood::kIdle, now);
                break;
        }
    }

    void RefreshText(mp::VoiceTextKind kind, uint32_t revision, uint32_t total_length) {
        char* destination = kind == mp::VoiceTextKind::kTranscript ? transcript_ : response_;
        const uint32_t capacity = kind == mp::VoiceTextKind::kTranscript ? sizeof(transcript_) : sizeof(response_);
        const uint32_t wanted = total_length < capacity - 1U ? total_length : capacity - 1U;
        uint32_t offset = 0U;
        while (offset < wanted) {
            const uint32_t chunk_capacity = (wanted - offset) < 512U ? wanted - offset : 512U;
            auto chunk = app_.voice().ReadText(kind, revision, offset, destination + offset, chunk_capacity);
            if (!chunk.has_value() || chunk.value().length == 0U) break;
            offset += chunk.value().length;
        }
        destination[offset] = '\0';
        if (kind == mp::VoiceTextKind::kTranscript) {
            transcript_revision_ = revision;
        } else {
            response_revision_ = revision;
        }
        for (uint32_t index = 0U; index < kBufferCount; ++index) buffer_ready_[index] = false;
    }

    TouchRegion RegionAt(Vector point) const {
        const float minimum = static_cast<float>(width_ < height_ ? width_ : height_);
        const float radius = minimum * 0.245F;
        const float local_x = point.x - center_x_.x;
        const float local_y = point.y - center_y_.x;
        for (uint32_t side = 0U; side < 2U; ++side) {
            const float eye_x = (side == 0U ? -0.27F : 0.27F) * radius;
            const float dx = (local_x - eye_x) / (radius * 0.22F);
            const float dy = (local_y + radius * 0.05F) / (radius * 0.20F);
            if (dx * dx + dy * dy < 1.0F) return side == 0U ? TouchRegion::kLeftEye : TouchRegion::kRightEye;
        }
        if (!HitBear(point)) return TouchRegion::kOutside;
        return local_y < 0.0F ? TouchRegion::kUpperBody : TouchRegion::kLowerBody;
    }

    bool HitBear(Vector point) const {
        const float minimum = static_cast<float>(width_ < height_ ? width_ : height_);
        const float radius = minimum * 0.245F;
        const float dx = (point.x - center_x_.x) / radius;
        const float dy = (point.y - center_y_.x) / (radius * 0.92F);
        return dx * dx + dy * dy < 1.18F;
    }

    const char* MoodName(Mood mood) const {
        switch (mood) {
            case Mood::kIdle:
                return "idle";
            case Mood::kListening:
                return "listening";
            case Mood::kCurious:
                return "curious";
            case Mood::kExcited:
                return "excited";
            case Mood::kDrowsy:
                return "drowsy";
            case Mood::kSleeping:
                return "sleeping";
            case Mood::kWaking:
                return "waking";
        }
        return "unknown";
    }

    void SetMood(Mood mood, uint64_t now_us) {
        if (mood_ == mood) return;
        mood_ = mood;
        mood_since_us_ = now_us;
        switch (mood) {
            case Mood::kListening:
                happiness_.target = 0.25F;
                break;
            case Mood::kCurious:
                happiness_.target = 0.45F;
                break;
            case Mood::kExcited:
                happiness_.target = 1.0F;
                break;
            case Mood::kDrowsy:
                happiness_.target = 0.0F;
                break;
            case Mood::kSleeping:
                happiness_.target = 0.0F;
                break;
            case Mood::kWaking:
                happiness_.target = 0.8F;
                blink_.target = 1.0F;
                blink_open_us_ = 0U;
                next_blink_us_ = now_us + 2200000ULL;
                center_y_.velocity -= static_cast<float>(height_) * 0.22F;
                if (PreferenceEnabled(kWakeCue)) (void)tones_.Play(bear_sfx::kWake);
                break;
            case Mood::kIdle:
                happiness_.target = 0.0F;
                break;
        }
        Line line;
        line.Append("bear: mood=");
        line.Append(MoodName(mood));
        app_.log().Info(line.c_str());
    }

    void PlayTouchCue(Mood mood) {
        if (!PreferenceEnabled(kTouchSounds)) return;
        switch (mood) {
            case Mood::kListening:
                (void)tones_.Play(bear_sfx::kListen);
                break;
            case Mood::kCurious:
                (void)tones_.Play(bear_sfx::kCurious);
                break;
            case Mood::kExcited:
                (void)tones_.Play(bear_sfx::kExcited);
                break;
            case Mood::kIdle:
            case Mood::kDrowsy:
            case Mood::kSleeping:
            case Mood::kWaking:
                break;
        }
    }

    // Touch feedback follows only a real mood change: hold-to-settings, settling
    // and voice-cancelling touches stay silent, and the same mood never retriggers.
    void SetTouchMood(Mood mood, uint64_t now_us) {
        const Mood previous = mood_;
        SetMood(mood, now_us);
        if (mood_ != previous) PlayTouchCue(mood_);
    }

    void Advance(uint64_t now_us) {
        const uint64_t elapsed_frame_us = now_us - last_frame_us_;
        tones_.Advance(mp::Duration::Microseconds(elapsed_frame_us));
        float dt = static_cast<float>(elapsed_frame_us) / 1000000.0F;
        last_frame_us_ = now_us;
        dt = Clamp(dt, 1.0F / 240.0F, 0.05F);
        if (view_ == View::kSettings) return;
        if (hold_candidate_ && now_us - hold_started_us_ >= kSettingsHoldUs) {
            EnterSettings(now_us);
            return;
        }
        const float elapsed = static_cast<float>(now_us - started_us_) / 1000000.0F;
        const float middle_x = static_cast<float>(width_) * 0.5F;
        const float middle_y = static_cast<float>(height_) * 0.49F;
        const float minimum = static_cast<float>(width_ < height_ ? width_ : height_);

        if (restart_voice_ && now_us >= next_voice_retry_us_) {
            if (app_.voice().StartSession().has_value()) {
                restart_voice_ = false;
                voice_active_ = true;
                voice_state_ = mp::VoiceState::kWoken;
                SetMood(Mood::kListening, now_us);
            } else {
                next_voice_retry_us_ = now_us + 100000ULL;
            }
        }

        const float animation_scale = static_cast<float>(preferences_.animation_percent) / 65.0F;
        if (active_touch_) {
            const float dx = touch_.x - middle_x;
            const float dy = touch_.y - middle_y;
            center_x_.target = middle_x + Clamp(dx * 0.075F, -minimum * 0.075F, minimum * 0.075F);
            center_y_.target = middle_y + Clamp(dy * 0.045F, -minimum * 0.045F, minimum * 0.045F);
            tilt_.target = Clamp(dx / static_cast<float>(width_) * 0.34F, -0.16F, 0.16F);
            gaze_x_.target = Clamp(dx * 0.085F, -minimum * 0.033F, minimum * 0.033F);
            gaze_y_.target = Clamp(dy * 0.055F, -minimum * 0.020F, minimum * 0.020F);
            squash_.target = touch_on_bear_ ? 0.965F : 1.0F;
        } else {
            center_x_.target = middle_x + mp::math::Sin(elapsed * 0.43F) * minimum * 0.008F * animation_scale;
            center_y_.target = middle_y + mp::math::Sin(elapsed * 0.81F) * minimum * 0.006F * animation_scale;
            tilt_.target = mp::math::Sin(elapsed * 0.37F) * 0.025F * animation_scale;
            gaze_x_.target = mp::math::Sin(elapsed * 0.61F) * minimum * 0.014F * animation_scale;
            gaze_y_.target = mp::math::Sin(elapsed * 0.83F) * minimum * 0.008F * animation_scale;
            squash_.target = 1.0F + mp::math::Sin(elapsed * 1.65F) * 0.010F * animation_scale;
        }

        if (!active_touch_) {
            if (!voice_active_ && mood_ == Mood::kExcited && now_us - mood_since_us_ >= 1200000ULL) {
                SetMood(Mood::kIdle, now_us);
            } else if (mood_ == Mood::kWaking && now_us - mood_since_us_ >= 1500000ULL) {
                SetMood(Mood::kIdle, now_us);
            } else if (mood_ == Mood::kIdle &&
                       now_us - last_interaction_us_ >=
                           static_cast<uint64_t>(preferences_.idle_timeout_seconds) * 1000000ULL) {
                SetMood(Mood::kDrowsy, now_us);
            } else if (mood_ == Mood::kDrowsy &&
                       now_us - last_interaction_us_ >=
                           static_cast<uint64_t>(preferences_.idle_timeout_seconds + 20U) * 1000000ULL) {
                SetMood(Mood::kSleeping, now_us);
            }
        }

        const float mood_time = static_cast<float>(now_us - mood_since_us_) / 1000000.0F;
        if (mood_ == Mood::kExcited) {
            const float amplitude = voice_state_ == mp::VoiceState::kSpeaking ? 0.007F : 0.018F;
            squash_.target = 0.985F + mp::math::Sin(mood_time * 5.0F) * amplitude;
        } else if (mood_ == Mood::kCurious && active_touch_) {
            tilt_.target += mp::math::Sin(mood_time * 2.5F) * 0.035F;
        } else if (mood_ == Mood::kDrowsy) {
            center_y_.target = middle_y + minimum * 0.030F;
            tilt_.target = 0.075F;
            gaze_x_.target = 0.0F;
            gaze_y_.target = minimum * 0.010F;
            squash_.target = 0.99F;
        } else if (mood_ == Mood::kSleeping) {
            center_y_.target = middle_y + minimum * 0.045F + mp::math::Sin(mood_time * 0.65F) * minimum * 0.006F;
            tilt_.target = 0.105F;
            gaze_x_.target = 0.0F;
            gaze_y_.target = 0.0F;
            squash_.target = 0.985F + mp::math::Sin(mood_time * 0.65F) * 0.012F;
        } else if (mood_ == Mood::kWaking) {
            center_y_.target =
                middle_y - mp::math::Sin(Clamp(mood_time / 1.5F, 0.0F, 1.0F) * mp::math::kPi) * minimum * 0.035F;
            tilt_.target = mp::math::Sin(mood_time * 8.0F) * 0.045F * Clamp(1.0F - mood_time / 1.5F, 0.0F, 1.0F);
        }
        if (voice_state_ == mp::VoiceState::kListening || voice_state_ == mp::VoiceState::kSpeaking) {
            squash_.target += static_cast<float>(voice_level_) / 1000.0F * 0.012F;
        }

        if (mood_ == Mood::kSleeping) {
            blink_.target = 0.035F;
            blink_open_us_ = 0U;
        } else if (mood_ == Mood::kDrowsy) {
            blink_.target = 0.34F;
            blink_open_us_ = 0U;
        } else {
            if (now_us >= next_blink_us_) {
                blink_.target = 0.035F;
                blink_open_us_ = now_us + 105000ULL;
                const uint64_t variation = (blink_sequence_++ % 3U) * 620000ULL;
                next_blink_us_ = now_us + 2700000ULL + variation;
            }
            if (blink_open_us_ != 0U && now_us >= blink_open_us_) {
                blink_.target = 1.0F;
                blink_open_us_ = 0U;
            }
        }

        uint32_t steps = static_cast<uint32_t>(dt * 120.0F) + 1U;
        if (steps > 6U) steps = 6U;
        const float step = dt / static_cast<float>(steps);
        for (uint32_t index = 0U; index < steps; ++index) {
            StepSpring(center_x_, 5.2F, 0.82F, step);
            StepSpring(center_y_, 5.2F, 0.78F, step);
            StepSpring(tilt_, 7.0F, 0.72F, step);
            StepSpring(gaze_x_, 11.0F, 0.75F, step);
            StepSpring(gaze_y_, 11.0F, 0.75F, step);
            StepSpring(squash_, 11.0F, 0.68F, step);
            StepSpring(happiness_, 7.0F, 0.72F, step);
            StepSpring(blink_, 22.0F, 0.82F, step);
        }
    }

    Vector Transform(float local_x, float local_y) const {
        const float cosine = mp::math::Cos(tilt_.x);
        const float sine = mp::math::Sin(tilt_.x);
        return {center_x_.x + local_x * cosine - local_y * sine, center_y_.x + local_x * sine + local_y * cosine};
    }

    void BuildOval(Vector* inner, Vector* outer, uint32_t count, Vector center, float radius_x, float radius_y,
                   float rotation, float fringe = 1.25F) {
        const float cosine = mp::math::Cos(rotation);
        const float sine = mp::math::Sin(rotation);
        for (uint32_t index = 0U; index < count; ++index) {
            const float angle = mp::math::kTwoPi * static_cast<float>(index) / static_cast<float>(count);
            const float local_x = mp::math::Cos(angle) * radius_x;
            const float local_y = mp::math::Sin(angle) * radius_y;
            const float dx = local_x * cosine - local_y * sine;
            const float dy = local_x * sine + local_y * cosine;
            inner[index] = {center.x + dx, center.y + dy};
            const float length = mp::math::Sqrt(dx * dx + dy * dy);
            const float scale = length > 0.0F ? (length + fringe) / length : 1.0F;
            outer[index] = {center.x + dx * scale, center.y + dy * scale};
        }
    }

    void BuildGeometry(uint64_t now_us) {
        if (view_ == View::kSettings) {
            current_bounds_ = {0, 0, width_, height_};
            return;
        }
        const float minimum = static_cast<float>(width_ < height_ ? width_ : height_);
        const float base = minimum * 0.245F;
        const float elapsed = static_cast<float>(now_us - started_us_) / 1000000.0F;
        center_ = {center_x_.x, center_y_.x};

        const float body_x = base * (1.0F + happiness_.x * 0.055F);
        const float body_y = base * (1.0F - happiness_.x * 0.025F) * squash_.x;
        for (uint32_t index = 0U; index < kBodyVertices; ++index) {
            const float angle = mp::math::kTwoPi * static_cast<float>(index) / static_cast<float>(kBodyVertices);
            const float ripple = 1.0F + mp::math::Sin(angle * 3.0F + elapsed * 0.72F) * 0.008F +
                                 mp::math::Sin(angle * 5.0F - elapsed * 0.53F) * 0.004F;
            const float local_x = mp::math::Cos(angle) * body_x * ripple;
            const float local_y = mp::math::Sin(angle) * body_y * ripple;
            body_[index] = Transform(local_x, local_y);
            const float dx = body_[index].x - center_.x;
            const float dy = body_[index].y - center_.y;
            const float length = mp::math::Sqrt(dx * dx + dy * dy);
            const float scale = length > 0.0F ? (length + 1.35F) / length : 1.0F;
            body_outer_[index] = {center_.x + dx * scale, center_.y + dy * scale};
        }

        if (wink_started_us_ != 0U && now_us - wink_started_us_ >= 360000ULL) wink_started_us_ = 0U;
        for (uint32_t side = 0U; side < 2U; ++side) {
            const float sign = side == 0U ? -1.0F : 1.0F;
            eye_center_[side] = Transform(sign * base * 0.27F + gaze_x_.x, -base * 0.05F + gaze_y_.x);
            const float eye_scale = 1.0F + happiness_.x * 0.16F;
            float wink = 1.0F;
            if (wink_started_us_ != 0U && side == wink_eye_) {
                const float progress = static_cast<float>(now_us - wink_started_us_) / 360000.0F;
                const float distance = progress < 0.42F ? 1.0F - progress / 0.42F : (progress - 0.42F) / 0.58F;
                wink = 0.035F + Clamp(distance, 0.0F, 1.0F) * 0.965F;
            }
            BuildOval(eye_[side], eye_outer_[side], kEyeVertices, eye_center_[side], base * 0.155F * eye_scale,
                      base * 0.135F * Clamp(blink_.x * wink, 0.035F, 1.10F) * eye_scale, tilt_.x);
        }

        const int32_t margin = static_cast<int32_t>(base * 1.12F + 6.0F);
        current_bounds_ = {static_cast<int32_t>(center_.x) - margin, static_cast<int32_t>(center_.y) - margin,
                           margin * 2 + 1, margin * 2 + 1};
        current_bounds_ = current_bounds_.intersection({0, 0, width_, height_});
    }

    bool Draw(mp::RasterDrawList& list, uint32_t buffer) {
        if (view_ == View::kSettings) return DrawSettings(list, buffer);
        list.SetPalette(kPaletteSlot);
        const mp::Rect clear =
            buffer_ready_[buffer] ? previous_[buffer].united(current_bounds_) : mp::Rect{0, 0, width_, height_};
        if (!list.FillRect(clear, kBackground)) return false;

        if (!DrawShape(list, center_, body_, body_outer_, kBodyVertices, kBodyColor)) return false;
        for (uint32_t side = 0U; side < 2U; ++side) {
            if (!DrawShape(list, eye_center_[side], eye_[side], eye_outer_[side], kEyeVertices, kEyeColor)) {
                return false;
            }
        }
        if (hold_candidate_) {
            const uint64_t now = app_.clock().Now().microseconds();
            if (now - hold_started_us_ >= kSettingsProgressDelayUs && !DrawHoldProgress(list, now)) return false;
        }
        previous_[buffer] = current_bounds_;
        buffer_ready_[buffer] = true;
        return true;
    }

    bool DrawHoldProgress(mp::RasterDrawList& list, uint64_t now) {
        constexpr uint32_t kSegments = 48U;
        const uint64_t elapsed = now > hold_started_us_ ? now - hold_started_us_ : 0U;
        const uint64_t visible = elapsed > kSettingsProgressDelayUs ? elapsed - kSettingsProgressDelayUs : 0U;
        const uint64_t duration = kSettingsHoldUs - kSettingsProgressDelayUs;
        uint32_t count = static_cast<uint32_t>(visible * kSegments / duration);
        if (count > kSegments) count = kSegments;
        const float minimum = static_cast<float>(width_ < height_ ? width_ : height_);
        const float inner = minimum * 0.245F + 12.0F;
        const float outer = inner + 6.0F;
        for (uint32_t index = 0U; index < count; ++index) {
            const float first =
                -mp::math::kPi * 0.5F + mp::math::kTwoPi * static_cast<float>(index) / static_cast<float>(kSegments);
            const float second = -mp::math::kPi * 0.5F +
                                 mp::math::kTwoPi * static_cast<float>(index + 1U) / static_cast<float>(kSegments);
            const mp::RasterVertex quad[4] = {
                mp::RasterVertex::At(center_.x + mp::math::Cos(first) * inner, center_.y + mp::math::Sin(first) * inner,
                                     0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(center_.x + mp::math::Cos(second) * inner,
                                     center_.y + mp::math::Sin(second) * inner, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(center_.x + mp::math::Cos(second) * outer,
                                     center_.y + mp::math::Sin(second) * outer, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(center_.x + mp::math::Cos(first) * outer, center_.y + mp::math::Sin(first) * outer,
                                     0.0F, 0.0F, kPaletteRows - 1U),
            };
            if (!list.FlatQuad(quad, kHoldColor)) return false;
        }
        return true;
    }

    const char* SettingsTitle() const {
        switch (settings_page_) {
            case SettingsPage::kOverview:
                return "状态总览";
            case SettingsPage::kBehavior:
                return "Bear 行为";
            case SettingsPage::kVoice:
                return "声音与语音";
            case SettingsPage::kCommands:
                return "本地指令";
            case SettingsPage::kPrivacy:
                return "隐私";
            case SettingsPage::kDiagnostics:
                return "测试与诊断";
            case SettingsPage::kAbout:
                return "关于";
            case SettingsPage::kCount:
                break;
        }
        return "Bear 设置";
    }

    bool DrawSettingsRow(mp::RasterDrawList& list, uint32_t row, const char* title, const char* detail,
                         const char* value, mp::Color value_color = kMuted) {
        constexpr int32_t kContentX = 255;
        constexpr int32_t kRowTop = 132;
        constexpr int32_t kRowHeight = 70;
        const int32_t y = kRowTop + static_cast<int32_t>(row) * kRowHeight;
        if (!list.FillRect({kContentX, y, width_ - kContentX - 24, kRowHeight - 6}, kPanel)) return false;
        if (!list.FillRect({kContentX + 16, y + kRowHeight - 7, width_ - kContentX - 56, 1}, kLine)) return false;
        if (!list.Text({kContentX + 22, y + 10}, title, kInk, mp::SystemFont::kMedium)) return false;
        if (!list.Text({kContentX + 22, y + 38}, detail, kMuted, mp::SystemFont::kSmall)) return false;
        return value == nullptr || list.Text({width_ - 170, y + 19}, value, value_color, mp::SystemFont::kMedium);
    }

    bool DrawSettings(mp::RasterDrawList& list, uint32_t buffer) {
        static constexpr const char* kNavigation[] = {"总览", "Bear 行为", "声音与语音", "本地指令",
                                                      "隐私", "诊断",      "关于"};
        if (!list.FillRect({0, 0, width_, height_}, kBackground) || !list.FillRect({0, 0, 235, height_}, kSidebar)) {
            return false;
        }
        if (!list.Text({28, 24}, "Bear 设置", kEye, mp::SystemFont::kLarge) ||
            !list.Text({28, 58}, "设备上的个人伙伴", kSidebarMuted, mp::SystemFont::kSmall)) {
            return false;
        }
        for (uint32_t index = 0U; index < static_cast<uint32_t>(SettingsPage::kCount); ++index) {
            const int32_t y = 92 + static_cast<int32_t>(index) * 58;
            const bool selected = index == static_cast<uint32_t>(settings_page_);
            if (selected && !list.FillRect({12, y + 4, 211, 48}, kBackground)) return false;
            if (!list.Text({28, y + 16}, kNavigation[index], selected ? kInk : kSidebarMuted,
                           mp::SystemFont::kMedium)) {
                return false;
            }
        }
        if (!list.Text({262, 24}, SettingsTitle(), kInk, mp::SystemFont::kTitle) ||
            !list.Text({262, 68}, "设置会自动保存到 Bear 的私有空间", kMuted, mp::SystemFont::kSmall) ||
            !list.FillRect({width_ - 170, 20, 146, 48}, kPanel) ||
            !list.Text({width_ - 146, 34}, "返回 Bear", kInk, mp::SystemFont::kMedium)) {
            return false;
        }

        Line value;
        switch (settings_page_) {
            case SettingsPage::kOverview:
                if (!DrawSettingsRow(
                        list, 0U, "云端语音", "Voice Profile 与网关线路",
                        voice_info_.configured ? (voice_info_.route_online ? "已连接" : "已配置") : "未配置",
                        voice_info_.route_online ? kGood : kMuted) ||
                    !DrawSettingsRow(list, 1U, "本地识别", "MultiNet 常用命令",
                                     voice_info_.local_commands ? "可用" : "不可用",
                                     voice_info_.local_commands ? kGood : kMuted) ||
                    !DrawSettingsRow(list, 2U, "麦克风", "当前输入电平", voice_available_ ? "正常" : "不可用",
                                     voice_available_ ? kGood : kMuted) ||
                    !DrawSettingsRow(list, 3U, "扬声器", "低音量试听位于诊断页", tones_.enabled() ? "正常" : "不可用",
                                     tones_.enabled() ? kGood : kMuted) ||
                    !DrawSettingsRow(list, 4U, "安全", "地址、令牌、CA 与云端密钥", "已隐藏")) {
                    return false;
                }
                break;
            case SettingsPage::kBehavior:
                value.AppendUint(preferences_.idle_timeout_seconds);
                value.Append(" 秒");
                if (!DrawSettingsRow(list, 0U, "自动休息", "点击切换 30 / 40 / 50 秒", value.c_str())) return false;
                value.Clear();
                value.AppendUint(preferences_.animation_percent);
                value.Append("%");
                if (!DrawSettingsRow(list, 1U, "动画强度", "控制呼吸、倾斜与形变", value.c_str()) ||
                    !DrawSettingsRow(list, 2U, "好奇反应", "拖动时跟随手指观察",
                                     PreferenceEnabled(kCuriousReaction) ? "开启" : "关闭") ||
                    !DrawSettingsRow(list, 3U, "触摸音效", "轻触与拖动时播放轻提示",
                                     PreferenceEnabled(kTouchSounds) ? "开启" : "关闭")) {
                    return false;
                }
                break;
            case SettingsPage::kVoice:
                value.AppendUint(preferences_.answer_gain_per_mille / 10U);
                value.Append("%");
                if (!DrawSettingsRow(list, 0U, "回答音量上限", "点击以 10% 为步长调整", value.c_str()) ||
                    !DrawSettingsRow(list, 1U, "首次唤醒提示音", "仅从睡眠进入清醒时播放",
                                     PreferenceEnabled(kWakeCue) ? "开启" : "关闭") ||
                    !DrawSettingsRow(list, 2U, "流式回答", "文本与音频由 Host 固定提供",
                                     voice_available_ ? "Host 固定" : "不可用") ||
                    !DrawSettingsRow(list, 3U, "音色与语速", "由当前 Voice Profile 决定", "只读")) {
                    return false;
                }
                break;
            case SettingsPage::kCommands:
                if (!DrawSettingsRow(list, 0U, "启用本地命令", "命中后立即执行并取消云会话",
                                     PreferenceEnabled(kLocalCommands) ? "开启" : "关闭") ||
                    !DrawSettingsRow(list, 1U, "睡觉吧 / 醒醒", "固定 MultiNet 词表", "本地") ||
                    !DrawSettingsRow(list, 2U, "大声一点 / 小声一点", "固定 MultiNet 词表", "本地") ||
                    !DrawSettingsRow(list, 3U, "识别策略", "本地与云端并行，未命中不等待", "并行")) {
                    return false;
                }
                break;
            case SettingsPage::kPrivacy:
                if (!DrawSettingsRow(list, 0U, "原始音频", "默认不保存；诊断最长开启 10 分钟", "不保存") ||
                    !DrawSettingsRow(list, 1U, "清除对话上下文", "清除当前连接的八轮临时上下文", "清除") ||
                    !DrawSettingsRow(list, 2U, "设备凭据", "Guest 无权读取敏感配置", "Host 保管") ||
                    !DrawSettingsRow(list, 3U, "麦克风提示", "监听时由 Bear 动画明确反馈", "开启")) {
                    return false;
                }
                break;
            case SettingsPage::kDiagnostics:
                value.AppendUint(voice_diagnostics_.input_level_per_mille / 10U);
                value.Append("%");
                if (!DrawSettingsRow(list, 0U, "麦克风电平", "不启动第二路采音会话", value.c_str()) ||
                    !DrawSettingsRow(list, 1U, "扬声器试听", "以安全的应用音效音量播放", "试听") ||
                    !DrawSettingsRow(list, 2U, "Opus 发送队列", "固定容量与背压状态",
                                     voice_diagnostics_.frames_dropped == 0U ? "正常" : "有丢帧") ||
                    !DrawSettingsRow(list, 3U, "最近链路耗时", "ASR / LLM / TTS 阶段",
                                     voice_diagnostics_.first_audio_ms == 0U ? "暂无" : "已有数据") ||
                    !DrawSettingsRow(list, 4U, "下行音频保存", "仅诊断期间临时启用", "关闭")) {
                    return false;
                }
                break;
            case SettingsPage::kAbout:
                if (!DrawSettingsRow(list, 0U, "Bear", "micropixel.bear", "0.1.0") ||
                    !DrawSettingsRow(list, 1U, "Voice 协议", "自有 WebSocket 流式协议", "v2") ||
                    !DrawSettingsRow(list, 2U, "系统设置", "Wi-Fi、亮度、系统音量与固件", "打开") ||
                    !DrawSettingsRow(list, 3U, "许可证", "开源组件与第三方声明", "查看")) {
                    return false;
                }
                break;
            case SettingsPage::kCount:
                return false;
        }
        previous_[buffer] = {0, 0, width_, height_};
        buffer_ready_[buffer] = true;
        return true;
    }

    bool DrawShape(mp::RasterDrawList& list, Vector center, const Vector* inner, const Vector* outer, uint32_t count,
                   uint8_t color) {
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t next = (index + 1U) % count;
            const mp::RasterVertex triangle[3] = {
                mp::RasterVertex::At(center.x, center.y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(inner[index].x, inner[index].y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(inner[next].x, inner[next].y, 0.0F, 0.0F, kPaletteRows - 1U),
            };
            if (!list.FlatTriangle(triangle, color)) return false;
        }
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t next = (index + 1U) % count;
            const mp::RasterVertex fringe[4] = {
                mp::RasterVertex::At(inner[index].x, inner[index].y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(inner[next].x, inner[next].y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(outer[next].x, outer[next].y, 0.0F, 0.0F, 0U),
                mp::RasterVertex::At(outer[index].x, outer[index].y, 0.0F, 0.0F, 0U),
            };
            if (!list.FlatQuad(fringe, color)) return false;
        }
        return true;
    }

    void RecordStats(uint64_t wait_started, uint64_t frame_started, uint64_t geometry_done, uint64_t update_done,
                     uint64_t present_done) {
        wait_us_ += frame_started - wait_started;
        geometry_us_ += geometry_done - frame_started;
        update_us_ += update_done - geometry_done;
        present_us_ += present_done - update_done;
        ++stats_frames_;
        if (stats_frames_ != kStatsFrames) return;

        const uint64_t elapsed = present_done > stats_started_us_ ? present_done - stats_started_us_ : 1U;
        Line line;
        line.Append("bear: fps_x100=");
        line.AppendUint(kStatsFrames * 100000000ULL / elapsed);
        line.Append(" wait_us=");
        line.AppendUint(wait_us_ / kStatsFrames);
        line.Append(" geometry_us=");
        line.AppendUint(geometry_us_ / kStatsFrames);
        line.Append(" update_us=");
        line.AppendUint(update_us_ / kStatsFrames);
        line.Append(" present_us=");
        line.AppendUint(present_us_ / kStatsFrames);
        line.Append(" audio_dropped=");
        line.AppendUint(tones_.dropped());
        app_.log().Info(line.c_str());
        stats_frames_ = 0U;
        wait_us_ = 0U;
        geometry_us_ = 0U;
        update_us_ = 0U;
        present_us_ = 0U;
        stats_started_us_ = present_done;
    }

    void LogSetup(const mp::RendererInfo& renderer) {
        Line line;
        line.Append("bear: display=");
        line.AppendUint(renderer.physical_width());
        line.Append("x");
        line.AppendUint(renderer.physical_height());
        line.Append(" vertices=96 aa=1 buffers=2");
        line.Append(" audio=");
        line.AppendUint(tones_.enabled() ? 1U : 0U);
        line.Append(" voice=");
        line.AppendUint(voice_available_ ? 1U : 0U);
        app_.log().Info(line.c_str());
    }

    mp::Application app_{};
    mp::ToneSequencer<8U> tones_{app_.audio(), false};
    mp::HostSurface surface_{};
    mp::RasterResources resources_{};
    uint16_t palette_[kPaletteRows * mp::RasterResources::kPaletteEntries]{};

    Vector body_[kBodyVertices]{};
    Vector body_outer_[kBodyVertices]{};
    Vector eye_[2][kEyeVertices]{};
    Vector eye_outer_[2][kEyeVertices]{};

    Vector center_{};
    Vector eye_center_[2]{};
    Vector touch_{};
    Vector touch_origin_{};

    Spring center_x_{};
    Spring center_y_{};
    Spring tilt_{};
    Spring gaze_x_{};
    Spring gaze_y_{};
    Spring squash_{};
    Spring happiness_{};
    Spring blink_{};

    mp::Rect previous_[kBufferCount]{};
    bool buffer_ready_[kBufferCount]{};
    mp::Rect current_bounds_{};
    uint64_t started_us_{};
    uint64_t last_frame_us_{};
    uint64_t next_blink_us_{};
    uint64_t blink_open_us_{};
    uint64_t wink_started_us_{};
    uint64_t last_interaction_us_{};
    uint64_t mood_since_us_{};
    uint64_t hold_started_us_{};
    uint64_t stats_started_us_{};
    uint64_t wait_us_{};
    uint64_t geometry_us_{};
    uint64_t update_us_{};
    uint64_t present_us_{};
    uint32_t stats_frames_{};
    uint32_t blink_sequence_{};
    uint32_t touch_id_{};
    uint32_t wink_eye_{};
    uint32_t transcript_revision_{};
    uint32_t response_revision_{};
    uint16_t voice_level_{};
    int32_t width_{};
    int32_t height_{};
    Preferences preferences_{};
    mp::VoiceInfo voice_info_{};
    mp::VoiceDiagnostics voice_diagnostics_{};
    Mood mood_{Mood::kIdle};
    View view_{View::kPet};
    SettingsPage settings_page_{SettingsPage::kOverview};
    TouchRegion touch_region_{TouchRegion::kOutside};
    bool active_touch_{};
    bool touch_on_bear_{};
    bool voice_available_{};
    bool voice_active_{};
    bool restart_voice_{};
    bool hold_candidate_{};
    mp::VoiceState voice_state_{mp::VoiceState::kIdle};
    uint64_t next_voice_retry_us_{};
    char transcript_[513]{};
    char response_[1025]{};
};

}  // namespace

int main() {
    static Bear bear __attribute__((no_destroy));
    return bear.Run();
}
