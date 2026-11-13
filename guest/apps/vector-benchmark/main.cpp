// Animated vector workload for choosing a character rendering quality tier.
//
// The body is a continuously deforming radial ring. Two blinking eyes follow
// it. Filled fans exercise FlatTriangle; the optional one-pixel fringe uses
// FlatQuad vertex-light interpolation through a preblended 32-row palette.

#include <stdint.h>

#include "sdk/micropixel.hpp"

namespace {

namespace mp = micropixel;

constexpr uint32_t kPaletteRows = 32U;
constexpr uint8_t kPaletteSlot = 0U;
constexpr uint32_t kWarmupFrames = 60U;
constexpr uint32_t kMeasuredFrames = 240U;
constexpr uint32_t kMaxBodyVertices = 96U;
constexpr uint32_t kMaxEyeVertices = kMaxBodyVertices / 2U;
constexpr uint32_t kMaxBuffers = 3U;
constexpr uint32_t kResultCount = 12U;
constexpr uint32_t kVertexOptions[] = {48U, 72U, 96U};
constexpr mp::Color kBackground = mp::Color::Rgb(243U, 239U, 230U);
constexpr mp::Color kBody = mp::Color::Rgb(24U, 27U, 32U);
constexpr mp::Color kEye = mp::Color::Rgb(243U, 239U, 230U);
constexpr mp::Color kAccent = mp::Color::Rgb(65U, 145U, 254U);

using Line = mp::FixedString<240U>;

struct Vector final {
    float x{};
    float y{};
};

struct Config final {
    uint32_t vertices{};
    uint32_t buffers{};
    bool antialias{};
};

struct Measurement final {
    Config config{};
    uint64_t geometry_us{};
    uint64_t update_us{};
    uint64_t wait_us{};
    uint64_t present_us{};
    uint32_t fps_x100{};
    uint32_t records{};
    uint32_t clear_pixels{};
};

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

class Benchmark final {
   public:
    int Run() {
        if (!app_.renderer().ConfigureDisplay({}).has_value()) return Fail("configure display failed");
        const mp::RendererInfo display = app_.renderer().info();
        if (!display.raster_supported() || !display.polygon_supported()) {
            return Fail("Host raster polygon capability required");
        }
        auto resources = app_.renderer().CreateRasterResources();
        if (!resources.has_value()) return Fail("raster resources unavailable");
        resources_ = resources.value();
        BuildPalette();
        if (!resources_.UploadLitPalette(kPaletteSlot, kPaletteRows, palette_).has_value()) {
            return Fail("palette upload failed");
        }

        LogSetup(display);
        for (uint32_t buffers = 2U; buffers <= 3U; ++buffers) {
            if (!CreateSurface(buffers)) return 2;
            for (uint32_t vertices : kVertexOptions) {
                if (!RunConfig({vertices, buffers, false}) || !RunConfig({vertices, buffers, true})) return 3;
            }
            surface_.Reset();
        }
        LogSelection();
        app_.log().Info("vector-benchmark: complete");
        return 0;
    }

   private:
    int Fail(const char* reason) {
        app_.log().Error(reason);
        return 1;
    }

    void BuildPalette() {
        for (uint32_t level = 0U; level < kPaletteRows; ++level) {
            uint16_t* row = palette_ + level * mp::RasterResources::kPaletteEntries;
            for (uint32_t index = 0U; index < mp::RasterResources::kPaletteEntries; ++index) {
                row[index] = kBackground.rgb565();
            }
            row[1] = Mix(kBackground, kBody, level, kPaletteRows - 1U).rgb565();
            row[2] = Mix(kBody, kEye, level, kPaletteRows - 1U).rgb565();
            row[3] = Mix(kBackground, kAccent, level, kPaletteRows - 1U).rgb565();
        }
    }

    bool CreateSurface(uint32_t buffers) {
        auto created = app_.renderer().CreateHostSurface(buffers, 1U);
        if (!created.has_value()) {
            app_.log().Error("vector-benchmark: HostSurface creation failed");
            return false;
        }
        surface_ = static_cast<mp::HostSurface&&>(created.value());
        width_ = static_cast<int32_t>(surface_.buffer_width());
        height_ = static_cast<int32_t>(surface_.buffer_height());
        for (uint32_t index = 0U; index < kMaxBuffers; ++index) {
            buffer_ready_[index] = false;
            previous_[index] = {};
        }
        return true;
    }

    bool RunConfig(Config config) {
        uint64_t geometry_us = 0U;
        uint64_t update_us = 0U;
        uint64_t wait_us = 0U;
        uint64_t present_us = 0U;
        uint64_t measured_start_us = 0U;
        uint64_t measured_end_us = 0U;
        uint64_t record_total = 0U;
        uint64_t clear_total = 0U;
        const uint32_t total = kWarmupFrames + kMeasuredFrames;

        for (uint32_t frame = 0U; frame < total; ++frame) {
            if (!DrainEvents()) return false;
            const bool measuring = frame >= kWarmupFrames;
            uint32_t buffer_index = 0U;
            const uint64_t wait_started = app_.clock().Now().microseconds();
            if (frame == kWarmupFrames) measured_start_us = wait_started;
            while (!surface_.AcquireFree(buffer_index)) {
                if (!HandleEvent(app_.WaitEvent())) return false;
            }
            const uint64_t wait_done = app_.clock().Now().microseconds();
            BuildGeometry(config, sequence_++);
            const uint64_t geometry_done = app_.clock().Now().microseconds();
            bool drawn = false;
            uint32_t records = 0U;
            uint32_t clear_pixels = 0U;
            auto updated = surface_.Update(buffer_index, [&](mp::RasterDrawList& list) {
                drawn = Draw(list, buffer_index, config, records, clear_pixels);
            });
            const uint64_t update_done = app_.clock().Now().microseconds();
            if (!drawn || !updated.has_value() || !surface_.Present(buffer_index).has_value()) {
                app_.log().Error("vector-benchmark: frame rejected");
                return false;
            }
            const uint64_t present_done = app_.clock().Now().microseconds();
            if (measuring) {
                wait_us += wait_done - wait_started;
                geometry_us += geometry_done - wait_done;
                update_us += update_done - geometry_done;
                present_us += present_done - update_done;
                record_total += records;
                clear_total += clear_pixels;
                measured_end_us = present_done;
            }
        }

        const uint64_t elapsed_us = measured_end_us - measured_start_us;
        Measurement result{
            .config = config,
            .geometry_us = geometry_us / kMeasuredFrames,
            .update_us = update_us / kMeasuredFrames,
            .wait_us = wait_us / kMeasuredFrames,
            .present_us = present_us / kMeasuredFrames,
            .fps_x100 = elapsed_us == 0U ? 0U : static_cast<uint32_t>(kMeasuredFrames * 100000000ULL / elapsed_us),
            .records = static_cast<uint32_t>(record_total / kMeasuredFrames),
            .clear_pixels = static_cast<uint32_t>(clear_total / kMeasuredFrames),
        };
        results_[result_count_++] = result;
        LogResult(result);
        return true;
    }

    void BuildGeometry(Config config, uint32_t sequence) {
        const float t = static_cast<float>(sequence) / 60.0F;
        const float minimum = static_cast<float>(width_ < height_ ? width_ : height_);
        center_.x = static_cast<float>(width_) * 0.5F + mp::math::Sin(t * 0.73F) * minimum * 0.035F;
        center_.y = static_cast<float>(height_) * 0.5F + mp::math::Sin(t * 0.91F) * minimum * 0.020F;
        const float base = minimum * 0.245F;
        const float squash_x = 1.0F + mp::math::Sin(t * 1.31F) * 0.025F;
        const float squash_y = 1.0F - mp::math::Sin(t * 1.31F) * 0.018F;
        for (uint32_t index = 0U; index < config.vertices; ++index) {
            const float angle = mp::math::kTwoPi * static_cast<float>(index) / static_cast<float>(config.vertices);
            const float ripple = 1.0F + mp::math::Sin(angle * 3.0F + t * 0.83F) * 0.047F +
                                 mp::math::Sin(angle * 5.0F - t * 1.07F) * 0.021F;
            const float dx = mp::math::Cos(angle) * base * ripple * squash_x;
            const float dy = mp::math::Sin(angle) * base * ripple * squash_y;
            body_[index] = {center_.x + dx, center_.y + dy};
            const float length = mp::math::Sqrt(dx * dx + dy * dy);
            const float scale = length > 0.0F ? (length + 1.35F) / length : 1.0F;
            body_outer_[index] = {center_.x + dx * scale, center_.y + dy * scale};
        }

        eye_vertices_ = config.vertices / 2U;
        const float blink = 0.62F + 0.38F * (0.5F + 0.5F * mp::math::Sin(t * 2.3F));
        const float gaze_x = mp::math::Sin(t * 0.67F) * 7.0F;
        const float gaze_y = mp::math::Sin(t * 0.89F) * 4.0F;
        BuildEye(left_eye_, left_eye_outer_, center_.x - base * 0.34F + gaze_x, center_.y - base * 0.08F + gaze_y,
                 base * 0.185F, base * 0.145F * blink);
        BuildEye(right_eye_, right_eye_outer_, center_.x + base * 0.34F + gaze_x, center_.y - base * 0.08F + gaze_y,
                 base * 0.185F, base * 0.145F * blink);

        const int32_t margin = static_cast<int32_t>(base * 1.12F + 4.0F);
        current_bounds_ = {static_cast<int32_t>(center_.x) - margin, static_cast<int32_t>(center_.y) - margin,
                           margin * 2 + 1, margin * 2 + 1};
        current_bounds_ = current_bounds_.intersection({0, 0, width_, height_});
    }

    void BuildEye(Vector* inner, Vector* outer, float x, float y, float radius_x, float radius_y) {
        for (uint32_t index = 0U; index < eye_vertices_; ++index) {
            const float angle = mp::math::kTwoPi * static_cast<float>(index) / static_cast<float>(eye_vertices_);
            const float dx = mp::math::Cos(angle) * radius_x;
            const float dy = mp::math::Sin(angle) * radius_y;
            inner[index] = {x + dx, y + dy};
            const float length = mp::math::Sqrt(dx * dx + dy * dy);
            const float scale = length > 0.0F ? (length + 1.15F) / length : 1.0F;
            outer[index] = {x + dx * scale, y + dy * scale};
        }
    }

    bool Draw(mp::RasterDrawList& list, uint32_t buffer_index, Config config, uint32_t& records,
              uint32_t& clear_pixels) {
        list.SetPalette(kPaletteSlot);
        mp::Rect clear = buffer_ready_[buffer_index] ? previous_[buffer_index].united(current_bounds_)
                                                     : mp::Rect{0, 0, width_, height_};
        if (!list.FillRect(clear, kBackground)) return false;
        ++records;
        clear_pixels = static_cast<uint32_t>(clear.width) * static_cast<uint32_t>(clear.height);

        if (!DrawFan(list, center_, body_, config.vertices, 1U, records)) return false;
        if (config.antialias && !DrawFringe(list, body_, body_outer_, config.vertices, 1U, records)) return false;

        const Vector left_center = Centroid(left_eye_, eye_vertices_);
        const Vector right_center = Centroid(right_eye_, eye_vertices_);
        if (!DrawFan(list, left_center, left_eye_, eye_vertices_, 2U, records) ||
            !DrawFan(list, right_center, right_eye_, eye_vertices_, 2U, records)) {
            return false;
        }
        if (config.antialias && (!DrawFringe(list, left_eye_, left_eye_outer_, eye_vertices_, 2U, records) ||
                                 !DrawFringe(list, right_eye_, right_eye_outer_, eye_vertices_, 2U, records))) {
            return false;
        }

        previous_[buffer_index] = current_bounds_;
        buffer_ready_[buffer_index] = true;
        return true;
    }

    bool DrawFan(mp::RasterDrawList& list, Vector center, const Vector* ring, uint32_t count, uint8_t color,
                 uint32_t& records) {
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t next = (index + 1U) % count;
            const mp::RasterVertex triangle[3] = {
                mp::RasterVertex::At(center.x, center.y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(ring[index].x, ring[index].y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(ring[next].x, ring[next].y, 0.0F, 0.0F, kPaletteRows - 1U),
            };
            if (!list.FlatTriangle(triangle, color)) return false;
            ++records;
        }
        return true;
    }

    bool DrawFringe(mp::RasterDrawList& list, const Vector* inner, const Vector* outer, uint32_t count, uint8_t color,
                    uint32_t& records) {
        for (uint32_t index = 0U; index < count; ++index) {
            const uint32_t next = (index + 1U) % count;
            const mp::RasterVertex quad[4] = {
                mp::RasterVertex::At(inner[index].x, inner[index].y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(inner[next].x, inner[next].y, 0.0F, 0.0F, kPaletteRows - 1U),
                mp::RasterVertex::At(outer[next].x, outer[next].y, 0.0F, 0.0F, 0U),
                mp::RasterVertex::At(outer[index].x, outer[index].y, 0.0F, 0.0F, 0U),
            };
            if (!list.FlatQuad(quad, color)) return false;
            ++records;
        }
        return true;
    }

    Vector Centroid(const Vector* points, uint32_t count) const {
        Vector result{};
        for (uint32_t index = 0U; index < count; ++index) {
            result.x += points[index].x;
            result.y += points[index].y;
        }
        const float scale = count == 0U ? 0.0F : 1.0F / static_cast<float>(count);
        result.x *= scale;
        result.y *= scale;
        return result;
    }

    bool DrainEvents() {
        mp::Event event;
        while (app_.PollEvent(event)) {
            if (!HandleEvent(event)) return false;
        }
        return true;
    }

    bool HandleEvent(const mp::Event& event) { return event.type() != mp::EventType::kStop; }

    void LogSetup(const mp::RendererInfo& display) {
        Line msg;
        msg.Append("vector-benchmark: display=");
        msg.AppendUint(display.physical_width());
        msg.Append("x");
        msg.AppendUint(display.physical_height());
        msg.Append(" panel_max_fps=");
        msg.AppendUint(display.max_full_frame_fps());
        msg.Append(" warmup=");
        msg.AppendUint(kWarmupFrames);
        msg.Append(" measured=");
        msg.AppendUint(kMeasuredFrames);
        app_.log().Info(msg.c_str());
    }

    void AppendConfig(Line& msg, Config config) const {
        msg.Append("vertices=");
        msg.AppendUint(config.vertices);
        msg.Append(" aa=");
        msg.AppendUint(config.antialias ? 1U : 0U);
        msg.Append(" buffers=");
        msg.AppendUint(config.buffers);
    }

    void LogResult(const Measurement& result) {
        Line msg;
        msg.Append("vector-benchmark: result ");
        AppendConfig(msg, result.config);
        msg.Append(" fps_x100=");
        msg.AppendUint(result.fps_x100);
        msg.Append(" wait_us=");
        msg.AppendUint(result.wait_us);
        msg.Append(" geometry_us=");
        msg.AppendUint(result.geometry_us);
        msg.Append(" update_us=");
        msg.AppendUint(result.update_us);
        msg.Append(" present_us=");
        msg.AppendUint(result.present_us);
        msg.Append(" records=");
        msg.AppendUint(result.records);
        msg.Append(" clear_px=");
        msg.AppendUint(result.clear_pixels);
        app_.log().Info(msg.c_str());
    }

    void LogSelection() {
        uint32_t fastest = 0U;
        for (uint32_t index = 1U; index < result_count_; ++index) {
            if (results_[index].fps_x100 > results_[fastest].fps_x100) fastest = index;
        }
        uint32_t recommended = 0U;
        while (recommended < result_count_ && !results_[recommended].config.antialias) ++recommended;
        if (recommended == result_count_) recommended = fastest;
        for (uint32_t index = recommended + 1U; index < result_count_; ++index) {
            if (results_[index].config.antialias && results_[index].fps_x100 > results_[recommended].fps_x100) {
                recommended = index;
            }
        }
        const uint32_t threshold = results_[fastest].fps_x100 * 97U / 100U;
        for (uint32_t index = 0U; index < result_count_; ++index) {
            const Measurement& candidate = results_[index];
            if (!candidate.config.antialias || candidate.fps_x100 < threshold) continue;
            const bool recommended_below_threshold = results_[recommended].fps_x100 < threshold;
            if (recommended_below_threshold || candidate.config.vertices > results_[recommended].config.vertices ||
                (candidate.config.vertices == results_[recommended].config.vertices &&
                 candidate.fps_x100 > results_[recommended].fps_x100)) {
                recommended = index;
            }
        }
        LogChoice("fastest", results_[fastest]);
        LogChoice("recommended", results_[recommended]);
    }

    void LogChoice(const char* label, const Measurement& result) {
        Line msg;
        msg.Append("vector-benchmark: ");
        msg.Append(label);
        msg.Append(" ");
        AppendConfig(msg, result.config);
        msg.Append(" fps_x100=");
        msg.AppendUint(result.fps_x100);
        msg.Append(" update_us=");
        msg.AppendUint(result.update_us);
        app_.log().Info(msg.c_str());
    }

    mp::Application app_{};
    mp::HostSurface surface_{};
    mp::RasterResources resources_{};
    uint16_t palette_[kPaletteRows * mp::RasterResources::kPaletteEntries]{};
    Vector body_[kMaxBodyVertices]{};
    Vector body_outer_[kMaxBodyVertices]{};
    Vector left_eye_[kMaxEyeVertices]{};
    Vector left_eye_outer_[kMaxEyeVertices]{};
    Vector right_eye_[kMaxEyeVertices]{};
    Vector right_eye_outer_[kMaxEyeVertices]{};
    Measurement results_[kResultCount]{};
    mp::Rect previous_[kMaxBuffers]{};
    bool buffer_ready_[kMaxBuffers]{};
    Vector center_{};
    mp::Rect current_bounds_{};
    uint32_t eye_vertices_{};
    uint32_t result_count_{};
    uint32_t sequence_{};
    int32_t width_{};
    int32_t height_{};
};

}  // namespace

int main() {
    static Benchmark benchmark __attribute__((no_destroy));
    return benchmark.Run();
}
