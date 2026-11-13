#include <stdint.h>

#include <array>
#include <vector>

#include "sdk/micropixel.hpp"

namespace mp = micropixel;
using mp::literals::operator""_ms;

namespace {

constexpr mp::Color kBackground = mp::Color::Rgb(6U, 13U, 25U);
constexpr mp::Color kPanel = mp::Color::Rgb(18U, 34U, 54U);
constexpr mp::Color kAccent = mp::Color::Rgb(57U, 208U, 153U);
constexpr mp::Color kBlue = mp::Color::Rgb(67U, 130U, 232U);
constexpr mp::Color kWarning = mp::Color::Rgb(238U, 174U, 64U);
constexpr mp::Color kDanger = mp::Color::Rgb(225U, 76U, 94U);
constexpr mp::Color kMuted = mp::Color::Rgb(144U, 164U, 188U);

constexpr uint32_t kTestCount = 8U;
constexpr uint32_t kMicSampleCapacity = 320U;
constexpr uint32_t kVoiceTimeoutTicks = 300U;
constexpr uint32_t kSoakDurationTicks = 3000U;

enum class TestId : uint8_t {
    kMicrophone,
    kSpeaker,
    kModel,
    kVoiceFlow,
    kNetwork,
    kTouch,
    kStorage,
    kSoak,
};

enum class Verdict : uint8_t {
    kNotRun,
    kRunning,
    kPass,
    kFail,
    kManual,
};

struct TestEntry final {
    TestId id;
    const char* label;
    const char* subtitle;
};

constexpr std::array<TestEntry, kTestCount> kTests{{
    {TestId::kMicrophone, "语音输入", "麦克风 PCM / 电平"},
    {TestId::kSpeaker, "语音输出", "扬声器 / 音频链路"},
    {TestId::kModel, "大模型", "转写 / 回复文本"},
    {TestId::kVoiceFlow, "完整流程", "录音到回复播放"},
    {TestId::kNetwork, "Wi-Fi / 网络", "路由 / 断线恢复"},
    {TestId::kTouch, "触摸 / 显示", "四区域输入"},
    {TestId::kStorage, "持久存储", "写入 / 读取 / 删除"},
    {TestId::kSoak, "稳定性巡检", "5 分钟 / 重启检测"},
}};

struct Layout final {
    mp::Rect screen{};
    mp::Rect header{};
    mp::Rect content{};
    mp::Rect actions{};
    mp::Rect back{};
    std::array<mp::Rect, kTestCount> menu{};
    std::array<mp::Rect, 4U> action{};
};

class View final {
   public:
    View(mp::Renderer renderer, mp::Scene& scene, mp::ContainerNode root)
        : renderer_(renderer),
          scene_(scene),
          shapes_(root.CreateContainer({.z_order = 0}).value()),
          labels_(root.CreateContainer({.z_order = 1}).value()) {}

    template <typename Function>
    void Update(Function&& function) {
        shape_count_ = 0U;
        label_count_ = 0U;
        for (auto& shape : shape_nodes_) shape.SetVisible(false);
        for (auto& label : label_nodes_) label.SetVisible(false);
        function(*this);
        DestroyUnused(shape_nodes_, shape_count_);
        DestroyUnused(label_nodes_, label_count_);
        mp::Assert(renderer_.Present(scene_).has_value(), "diagnostics: scene present failed");
        shape_nodes_.resize(shape_count_);
        label_nodes_.resize(label_count_);
    }

    void Panel(mp::Rect rect, mp::Color color, uint8_t opacity = 255U) {
        if (shape_count_ == shape_nodes_.size()) {
            shape_nodes_.push_back(shapes_.CreateShape(rect, color, opacity).value());
        }
        auto& node = shape_nodes_[shape_count_++];
        node.SetRect(rect);
        node.SetColor(color);
        node.SetOpacity(opacity);
        node.SetVisible(opacity != 0U);
    }

    void Text(mp::Point position, const char* text, mp::Color color,
              mp::SystemFont font = mp::SystemFont::kMedium, bool centered = false) {
        if (label_count_ == label_nodes_.size()) {
            label_nodes_.push_back(labels_.CreateLabel(position, text, color, font, centered).value());
        }
        auto& node = label_nodes_[label_count_++];
        node.SetPosition(position);
        node.SetText(text);
        node.SetColor(color);
        node.SetFont(font);
        node.SetCentered(centered);
        node.SetVisible(true);
    }

   private:
    template <typename Node>
    static void DestroyUnused(std::vector<Node>& nodes, uint32_t used) {
        for (uint32_t index = used; index < nodes.size(); ++index) nodes[index].Destroy();
    }

    mp::Renderer renderer_;
    mp::Scene& scene_;
    mp::ContainerNode shapes_{};
    mp::ContainerNode labels_{};
    std::vector<mp::ShapeNode> shape_nodes_{};
    std::vector<mp::LabelNode> label_nodes_{};
    uint32_t shape_count_{};
    uint32_t label_count_{};
};

[[nodiscard]] Layout BuildLayout(const mp::RendererInfo& display) {
    Layout result{};
    result.screen = {0, 0, static_cast<int32_t>(display.width()), static_cast<int32_t>(display.height())};
    result.header = {0, 0, result.screen.width, 88};
    result.content = {24, 96, result.screen.width - 48, result.screen.height - 216};
    result.actions = {24, result.screen.height - 108, result.screen.width - 48, 88};
    result.back = {24, 18, 132, 54};

    constexpr int32_t kColumns = 4;
    constexpr int32_t kRows = 2;
    constexpr int32_t kGap = 14;
    const int32_t item_width = (result.content.width - (kColumns - 1) * kGap) / kColumns;
    const int32_t item_height = (result.content.height - (kRows - 1) * kGap) / kRows;
    for (uint32_t index = 0U; index < kTestCount; ++index) {
        const int32_t column = static_cast<int32_t>(index % kColumns);
        const int32_t row = static_cast<int32_t>(index / kColumns);
        result.menu[index] = {result.content.x + column * (item_width + kGap),
                              result.content.y + row * (item_height + kGap), item_width, item_height};
    }
    const int32_t action_width = (result.actions.width - 3 * kGap) / 4;
    for (uint32_t index = 0U; index < result.action.size(); ++index) {
        result.action[index] = {result.actions.x + static_cast<int32_t>(index) * (action_width + kGap),
                                result.actions.y, action_width, result.actions.height};
    }
    return result;
}

[[nodiscard]] const char* VerdictText(Verdict verdict) {
    switch (verdict) {
        case Verdict::kNotRun: return "未测试";
        case Verdict::kRunning: return "测试中";
        case Verdict::kPass: return "通过";
        case Verdict::kFail: return "失败";
        case Verdict::kManual: return "等待确认";
    }
    return "未知";
}

[[nodiscard]] mp::Color VerdictColor(Verdict verdict) {
    switch (verdict) {
        case Verdict::kPass: return kAccent;
        case Verdict::kFail: return kDanger;
        case Verdict::kRunning: return kBlue;
        case Verdict::kManual: return kWarning;
        case Verdict::kNotRun: return kMuted;
    }
    return kMuted;
}

class DiagnosticApp final {
   public:
    DiagnosticApp()
        : app_(),
          renderer_(ConfigureRenderer()),
          display_(renderer_.info()),
          layout_(BuildLayout(display_)),
          scene_(renderer_.CreateScene(kBackground).value()),
          view_root_(scene_.CreateContainer({.z_order = 0}).value()),
          controls_(scene_.CreateContainer({.z_order = 1}).value()),
          view_(renderer_, scene_, view_root_),
          home_controls_(controls_.CreateContainer().value()),
          page_controls_(controls_.CreateContainer().value()),
          ticker_(app_.timers().Every(100_ms).value()) {
        LoadPersistentState();
        CreateControls();
        Render();
        app_.log().Info("diagnostics: app ready");
    }

    int Run() {
        app_.Run([this](const mp::Event& event) { OnEvent(event); });
        return 0;
    }

   private:
    mp::Renderer ConfigureRenderer() {
        app_.renderer()
            .ConfigureDisplay({.logical_size = {1024U, 600U}, .scale_mode = mp::DisplayScaleMode::kAspectFit})
            .value();
        return app_.renderer();
    }

    void LoadPersistentState() {
        pass_mask_ = app_.storage().GetU32Or("diag.pass", 0U);
        fail_mask_ = app_.storage().GetU32Or("diag.fail", 0U);
        for (uint32_t index = 0U; index < kTestCount; ++index) {
            const uint32_t bit = 1U << index;
            if ((fail_mask_ & bit) != 0U) verdicts_[index] = Verdict::kFail;
            if ((pass_mask_ & bit) != 0U) verdicts_[index] = Verdict::kPass;
        }
        if (app_.storage().GetBoolOr("diag.soak", false)) {
            verdicts_[Index(TestId::kSoak)] = Verdict::kFail;
            fail_mask_ |= 1U << Index(TestId::kSoak);
            status_.Clear();
            status_.Append("上次稳定性巡检被重启或异常退出中断");
            (void)app_.storage().SetBool("diag.soak", false);
            SaveResults();
            LogResult(TestId::kSoak, "fail", "previous run interrupted");
        } else {
            status_.Append("选择一个项目开始测试");
        }
    }

    void CreateControls() {
        for (uint32_t index = 0U; index < kTestCount; ++index) {
            menu_buttons_[index] = home_controls_.CreateTextButton(
                {.bounds = layout_.menu[index],
                 .text = kTests[index].label,
                 .style = {.background = kPanel, .font = mp::SystemFont::kLarge}});
        }
        back_button_ = page_controls_.CreateTextButton(
            {.bounds = layout_.back,
             .text = "返回",
             .style = {.background = kPanel, .font = mp::SystemFont::kLarge},
             .hit_padding = 6U});
        constexpr const char* labels[4]{"开始 / 刷新", "停止 / 设置", "确认通过", "确认失败"};
        const mp::Color colors[4]{kBlue, kPanel, kAccent, kDanger};
        for (uint32_t index = 0U; index < action_buttons_.size(); ++index) {
            action_buttons_[index] = page_controls_.CreateTextButton(
                {.bounds = layout_.action[index],
                 .text = labels[index],
                 .style = {.background = colors[index], .font = mp::SystemFont::kMedium}});
        }
    }

    void OnEvent(const mp::Event& event) {
        if (event.type() == mp::EventType::kStop) {
            StopActiveTest(false);
            return;
        }
        bool redraw = false;
        if (const mp::TimerEvent* timer = event.TimerFrom(ticker_)) {
            (void)timer;
            redraw = OnTick();
        }
        if (const mp::VoiceEvent* voice = event.voice()) redraw = OnVoice(*voice) || redraw;
        if (const mp::TouchEvent* touch = event.touch()) redraw = OnTouch(*touch) || redraw;
        if (event.type() == mp::EventType::kResume) {
            if (active_ == TestId::kNetwork) RefreshNetwork();
            redraw = true;
        }
        if (redraw) Render();
    }

    [[nodiscard]] bool OnTouch(const mp::TouchEvent& touch) {
        if (home_) {
            bool redraw = false;
            for (uint32_t index = 0U; index < kTestCount; ++index) {
                const auto update = menu_buttons_[index].OnTouch(touch);
                redraw = update.redraw() || redraw;
                if (update.clicked) {
                    home_ = false;
                    active_ = kTests[index].id;
                    status_.Clear();
                    status_.Append("点击开始执行此项测试");
                    if (active_ == TestId::kNetwork) RefreshNetwork();
                    if (active_ == TestId::kTouch) status_.Clear(), status_.Append("依次点击画面四个测试区域");
                    return true;
                }
            }
            return redraw;
        }

        const auto back = back_button_.OnTouch(touch);
        if (back.clicked) {
            StopActiveTest(false);
            home_ = true;
            return true;
        }
        if (back.handled) return back.redraw();

        if (active_ == TestId::kTouch && touch.phase() == mp::TouchPhase::kUp && layout_.content.contains(touch.position())) {
            const bool right = touch.x() >= layout_.content.center_x();
            const bool bottom = touch.y() >= layout_.content.center_y();
            touch_mask_ |= 1U << (static_cast<uint32_t>(bottom) * 2U + static_cast<uint32_t>(right));
            if (touch_mask_ == 0x0fU) SetVerdict(active_, Verdict::kPass, "四个触摸区域均已收到事件");
            return true;
        }

        bool redraw = false;
        for (uint32_t index = 0U; index < action_buttons_.size(); ++index) {
            const auto update = action_buttons_[index].OnTouch(touch);
            redraw = update.redraw() || redraw;
            if (update.clicked) {
                HandleAction(index);
                return true;
            }
        }
        return redraw;
    }

    void HandleAction(uint32_t action) {
        if (action == 0U) {
            StartActiveTest();
            return;
        }
        if (action == 1U) {
            if (active_ == TestId::kNetwork) {
                if (!app_.system().OpenSettings()) {
                    SetVerdict(active_, Verdict::kFail, "无法打开系统设置");
                }
            } else {
                StopActiveTest(true);
            }
            return;
        }
        if (action == 2U) {
            SetVerdict(active_, Verdict::kPass, "用户确认硬件表现正常");
            return;
        }
        SetVerdict(active_, Verdict::kFail, "用户确认测试失败");
    }

    void StartActiveTest() {
        StopActiveTest(false);
        elapsed_ticks_ = 0U;
        switch (active_) {
            case TestId::kMicrophone: StartMicrophone(); return;
            case TestId::kSpeaker: StartSpeaker(); return;
            case TestId::kModel:
            case TestId::kVoiceFlow: StartVoice(); return;
            case TestId::kNetwork: RefreshNetwork(); return;
            case TestId::kTouch:
                touch_mask_ = 0U;
                SetRunning("请依次点击四个彩色区域");
                return;
            case TestId::kStorage: RunStorage(); return;
            case TestId::kSoak: StartSoak(); return;
        }
    }

    void StartMicrophone() {
        auto audio = app_.audio().info();
        auto input = app_.audio().input_info();
        if (!audio || !audio->supports_input_pcm || !input || input->sample_rate == 0U || input->channels != 1U ||
            input->bits_per_sample != 16U) {
            SetVerdict(active_, Verdict::kFail, "麦克风 PCM 能力不可用或格式无效");
            return;
        }
        mic_blocks_ = 0U;
        mic_min_ = INT16_MAX;
        mic_max_ = INT16_MIN;
        mic_peak_ = 0U;
        SetRunning("请持续说话 3 秒，正在采集 PCM");
    }

    void StartSpeaker() {
        auto audio = app_.audio().info();
        if (!audio || !audio->Supports(mp::Waveform::kSine)) {
            SetVerdict(active_, Verdict::kFail, "音频输出能力不可用");
            return;
        }
        const mp::Tone tone{.waveform = mp::Waveform::kSine,
                            .frequency_hz = 523U,
                            .duration = mp::Duration::Milliseconds(500U),
                            .volume_per_mille = 180U,
                            .attack = mp::Duration::Milliseconds(12U),
                            .release = mp::Duration::Milliseconds(40U)};
        if (!app_.audio().Play(tone)) {
            SetVerdict(active_, Verdict::kFail, "提示音提交失败");
            return;
        }
        verdicts_[Index(active_)] = Verdict::kManual;
        status_.Clear();
        status_.Append("若听到短提示音请选择确认通过，否则确认失败");
        app_.log().Info("diagnostics: test=speaker result=manual-confirmation");
    }

    void StartVoice() {
        auto info = app_.voice().info();
        if (!info || !info->configured) {
            SetVerdict(active_, Verdict::kFail, "语音服务未配置");
            return;
        }
        if (!info->route_online) {
            SetVerdict(active_, Verdict::kFail, "语音网关当前离线");
            return;
        }
        voice_transcript_ = false;
        voice_response_ = false;
        voice_audio_ = false;
        voice_listening_ = false;
        voice_speaking_ = false;
        voice_ended_ = false;
        voice_error_ = false;
        if (!app_.voice().StartSession()) {
            SetVerdict(active_, Verdict::kFail, "语音会话启动失败");
            return;
        }
        SetRunning(active_ == TestId::kModel ? "请说一个简单问题，等待回复文本" : "请说话，正在验证完整语音链路");
    }

    void RefreshNetwork() {
        auto network = app_.network().info();
        auto voice = app_.voice().info();
        network_ready_ = network && network->configured && network->route_online;
        voice_route_ready_ = voice && voice->configured && voice->route_online;
        if (network_ready_ && voice_route_ready_) {
            SetVerdict(active_, Verdict::kPass, "网络路由与语音网关均在线");
        } else {
            verdicts_[Index(active_)] = Verdict::kFail;
            status_.Clear();
            status_.Append(network_ready_ ? "系统网络在线，但语音网关离线" : "系统网络路由离线；请进入设置重连 Wi-Fi");
            PersistVerdict(active_);
            LogResult(active_, "fail", network_ready_ ? "voice route offline" : "network route offline");
        }
    }

    void RunStorage() {
        constexpr const char* key = "diag.probe";
        const uint32_t expected = static_cast<uint32_t>(app_.clock().Now().microseconds()) ^ 0xa53c91e7U;
        auto written = app_.storage().SetU32(key, expected);
        auto read = app_.storage().GetU32(key);
        auto removed = app_.storage().Remove(key);
        if (written && read && *read == expected && removed) {
            SetVerdict(active_, Verdict::kPass, "KV 写入、读取校验和删除均成功");
        } else {
            SetVerdict(active_, Verdict::kFail, "KV 存储事务失败或读回不一致");
        }
    }

    void StartSoak() {
        soak_failures_ = 0U;
        soak_route_losses_ = 0U;
        if (!app_.storage().SetBool("diag.soak", true)) {
            SetVerdict(active_, Verdict::kFail, "无法写入稳定性测试检查点");
            return;
        }
        SetRunning("稳定性巡检运行 5 分钟；可切换页面后返回查看");
    }

    void StopActiveTest(bool user_requested) {
        if (active_ == TestId::kModel || active_ == TestId::kVoiceFlow) (void)app_.voice().CancelSession();
        if (active_ == TestId::kSpeaker) (void)app_.audio().StopAll();
        if (active_ == TestId::kSoak && verdicts_[Index(active_)] == Verdict::kRunning) {
            (void)app_.storage().SetBool("diag.soak", false);
        }
        if (verdicts_[Index(active_)] == Verdict::kRunning) {
            verdicts_[Index(active_)] = Verdict::kNotRun;
            if (user_requested) {
                status_.Clear();
                status_.Append("测试已停止");
            }
        }
    }

    [[nodiscard]] bool OnTick() {
        if (home_ || verdicts_[Index(active_)] != Verdict::kRunning) return false;
        ++elapsed_ticks_;
        if (active_ == TestId::kMicrophone) return TickMicrophone();
        if (active_ == TestId::kModel || active_ == TestId::kVoiceFlow) return TickVoice();
        if (active_ == TestId::kSoak) return TickSoak();
        return false;
    }

    [[nodiscard]] bool TickMicrophone() {
        std::array<int16_t, kMicSampleCapacity> samples{};
        auto read = app_.audio().ReadInput(samples.data(), samples.size());
        if (!read || *read == 0U || *read > samples.size()) {
            SetVerdict(active_, Verdict::kFail, "读取麦克风 PCM 失败");
            return true;
        }
        ++mic_blocks_;
        for (uint32_t index = 0U; index < *read; ++index) {
            const int16_t value = samples[index];
            if (value < mic_min_) mic_min_ = value;
            if (value > mic_max_) mic_max_ = value;
            const uint32_t magnitude = value < 0 ? static_cast<uint32_t>(-(static_cast<int32_t>(value)))
                                                 : static_cast<uint32_t>(value);
            if (magnitude > mic_peak_) mic_peak_ = magnitude;
        }
        if (elapsed_ticks_ >= 30U) {
            const uint32_t range = static_cast<uint32_t>(static_cast<int32_t>(mic_max_) - static_cast<int32_t>(mic_min_));
            if (mic_blocks_ >= 20U && range >= 400U && mic_peak_ >= 250U) {
                SetVerdict(active_, Verdict::kPass, "PCM 连续变化，麦克风输入有效");
            } else {
                SetVerdict(active_, Verdict::kFail, "PCM 电平过低或数据近似静态");
            }
        }
        return true;
    }

    [[nodiscard]] bool TickVoice() {
        auto diagnostics = app_.voice().diagnostics();
        if (diagnostics) voice_audio_ = voice_audio_ || diagnostics->first_audio_ms != 0U;
        if (voice_error_) {
            SetVerdict(active_, Verdict::kFail, "语音服务报告错误");
            return true;
        }
        if (active_ == TestId::kModel && voice_transcript_ && voice_response_) {
            (void)app_.voice().CancelSession();
            SetVerdict(active_, Verdict::kPass, "已收到用户转写与模型回复文本");
            return true;
        }
        if (active_ == TestId::kVoiceFlow && voice_ended_) {
            if (voice_listening_ && voice_transcript_ && voice_response_ && voice_audio_ && voice_speaking_) {
                if (diagnostics && diagnostics->frames_dropped != 0U) {
                    SetVerdict(active_, Verdict::kFail, "流程完成，但检测到语音帧丢失");
                } else {
                    SetVerdict(active_, Verdict::kPass, "录音、转写、模型回复和播放均完成");
                }
            } else {
                SetVerdict(active_, Verdict::kFail, "会话结束，但完整链路里程碑不齐全");
            }
            return true;
        }
        if (elapsed_ticks_ >= kVoiceTimeoutTicks) {
            (void)app_.voice().CancelSession();
            SetVerdict(active_, Verdict::kFail, "语音测试 30 秒超时");
            return true;
        }
        return elapsed_ticks_ % 5U == 0U;
    }

    [[nodiscard]] bool TickSoak() {
        if (elapsed_ticks_ % 10U == 0U) {
            auto network = app_.network().info();
            auto voice = app_.voice().info();
            auto diagnostics = app_.voice().diagnostics();
            if (!network || !voice || !diagnostics) {
                ++soak_failures_;
            } else if (!network->route_online || !voice->route_online) {
                ++soak_route_losses_;
            }
        }
        if (elapsed_ticks_ >= kSoakDurationTicks) {
            (void)app_.storage().SetBool("diag.soak", false);
            if (soak_failures_ == 0U && soak_route_losses_ == 0U) {
                SetVerdict(active_, Verdict::kPass, "5 分钟服务巡检完成，无接口错误或路由掉线");
            } else {
                SetVerdict(active_, Verdict::kFail, "巡检期间出现接口错误或网络路由掉线");
            }
            return true;
        }
        return elapsed_ticks_ % 10U == 0U;
    }

    [[nodiscard]] bool OnVoice(const mp::VoiceEvent& event) {
        if (home_ || (active_ != TestId::kModel && active_ != TestId::kVoiceFlow) ||
            verdicts_[Index(active_)] != Verdict::kRunning) {
            return false;
        }
        if (event.event_type() == mp::VoiceEventType::kStateChanged) {
            voice_listening_ = voice_listening_ || event.state() == mp::VoiceState::kListening;
            voice_speaking_ = voice_speaking_ || event.state() == mp::VoiceState::kSpeaking;
        } else if (event.event_type() == mp::VoiceEventType::kTranscriptUpdated) {
            voice_transcript_ = event.total_length() != 0U;
        } else if (event.event_type() == mp::VoiceEventType::kResponseUpdated) {
            voice_response_ = event.total_length() != 0U;
        } else if (event.event_type() == mp::VoiceEventType::kSessionEnded) {
            voice_ended_ = true;
        } else if (event.event_type() == mp::VoiceEventType::kError) {
            voice_error_ = true;
        }
        return true;
    }

    void SetRunning(const char* message) {
        verdicts_[Index(active_)] = Verdict::kRunning;
        status_.Clear();
        status_.Append(message);
        LogResult(active_, "running", nullptr);
    }

    void SetVerdict(TestId id, Verdict verdict, const char* message) {
        verdicts_[Index(id)] = verdict;
        status_.Clear();
        status_.Append(message);
        PersistVerdict(id);
        LogResult(id, verdict == Verdict::kPass ? "pass" : "fail", message);
    }

    void PersistVerdict(TestId id) {
        const uint32_t bit = 1U << Index(id);
        pass_mask_ &= ~bit;
        fail_mask_ &= ~bit;
        if (verdicts_[Index(id)] == Verdict::kPass) pass_mask_ |= bit;
        if (verdicts_[Index(id)] == Verdict::kFail) fail_mask_ |= bit;
        SaveResults();
    }

    void SaveResults() {
        (void)app_.storage().SetU32("diag.pass", pass_mask_);
        (void)app_.storage().SetU32("diag.fail", fail_mask_);
    }

    void LogResult(TestId id, const char* result, const char* detail) {
        mp::FixedString<192U> line;
        line.Append("diagnostics: test=");
        line.Append(TestName(id));
        line.Append(" result=");
        line.Append(result);
        if (detail != nullptr) {
            line.Append(" detail=");
            line.Append(detail);
        }
        if (result[0] == 'f') app_.log().Error(line.c_str());
        else app_.log().Info(line.c_str());
    }

    void Render() {
        home_controls_.SetVisible(home_);
        page_controls_.SetVisible(!home_);
        if (home_) {
            for (auto& button : menu_buttons_) button.Sync();
            RenderHome();
        } else {
            back_button_.Sync();
            for (auto& button : action_buttons_) button.Sync();
            RenderPage();
        }
    }

    void RenderHome() {
        for (uint32_t index = 0U; index < kTestCount; ++index) {
            mp::FixedString<96U> label;
            label.Append(kTests[index].label);
            label.Append(" · ");
            label.Append(VerdictText(verdicts_[index]));
            mp::Assert(menu_buttons_[index].SetText(label.c_str()).has_value(),
                       "diagnostics: menu label update failed");
        }
        view_.Update([this](View& view) {
            view.Text({layout_.screen.center_x(), 20}, "设备硬件与软件诊断", mp::Color::White(),
                      mp::SystemFont::kTitle, true);
            view.Text({layout_.screen.center_x(), 62}, "分项验证 · 持久结果 · 可观测日志", kMuted,
                      mp::SystemFont::kMedium, true);
        });
    }

    void RenderPage() {
        UpdateActionControls();
        view_.Update([this](View& view) {
            view.Text({layout_.screen.center_x(), 24}, kTests[Index(active_)].label, mp::Color::White(),
                      mp::SystemFont::kTitle, true);
            view.Text({layout_.screen.center_x(), 65}, VerdictText(verdicts_[Index(active_)]),
                      VerdictColor(verdicts_[Index(active_)]), mp::SystemFont::kMedium, true);
            if (active_ == TestId::kTouch) RenderTouch(view);
            else RenderMetrics(view);
            view.Text({layout_.screen.center_x(), layout_.content.y + layout_.content.height - 42}, status_.c_str(),
                      VerdictColor(verdicts_[Index(active_)]), mp::SystemFont::kMedium, true);
        });
    }

    void UpdateActionControls() {
        const char* primary = "开始测试";
        if (active_ == TestId::kSpeaker) primary = "播放提示音";
        if (active_ == TestId::kModel || active_ == TestId::kVoiceFlow) primary = "开始对话";
        if (active_ == TestId::kNetwork) primary = "刷新状态";
        if (active_ == TestId::kTouch) primary = "重置区域";
        if (active_ == TestId::kStorage) primary = "运行测试";
        if (active_ == TestId::kSoak) primary = "开始 5 分钟";
        mp::Assert(action_buttons_[0].SetText(primary).has_value(), "diagnostics: primary label failed");
        mp::Assert(action_buttons_[1].SetText(active_ == TestId::kNetwork ? "打开系统设置" : "停止测试").has_value(),
                   "diagnostics: secondary label failed");
        const bool manual = active_ == TestId::kSpeaker || active_ == TestId::kTouch;
        action_buttons_[2].SetVisible(manual);
        action_buttons_[3].SetVisible(manual);
        for (auto& button : action_buttons_) button.Sync();
    }

    void RenderTouch(View& view) {
        const int32_t gap = 10;
        const int32_t width = (layout_.content.width - gap) / 2;
        const int32_t height = (layout_.content.height - gap) / 2;
        const mp::Color colors[4]{kBlue, kWarning, kAccent, kDanger};
        for (uint32_t index = 0U; index < 4U; ++index) {
            const int32_t x = layout_.content.x + static_cast<int32_t>(index % 2U) * (width + gap);
            const int32_t y = layout_.content.y + static_cast<int32_t>(index / 2U) * (height + gap);
            view.Panel({x, y, width, height}, colors[index], (touch_mask_ & (1U << index)) != 0U ? 255U : 100U);
        }
    }

    void RenderMetrics(View& view) {
        const int32_t center = layout_.content.center_x();
        if (active_ == TestId::kMicrophone) {
            mp::FixedString<128U> line;
            line.Append("采样块 "); line.AppendUint(mic_blocks_); line.Append("   峰值 "); line.AppendUint(mic_peak_);
            view.Text({center, layout_.content.y + 70}, line.c_str(), kMuted, mp::SystemFont::kLarge, true);
        } else if (active_ == TestId::kModel || active_ == TestId::kVoiceFlow) {
            mp::FixedString<160U> line;
            line.Append("Listening "); line.Append(voice_listening_ ? "✓" : "·");
            line.Append("   转写 "); line.Append(voice_transcript_ ? "✓" : "·");
            line.Append("   回复 "); line.Append(voice_response_ ? "✓" : "·");
            line.Append("   音频 "); line.Append(voice_audio_ ? "✓" : "·");
            view.Text({center, layout_.content.y + 70}, line.c_str(), kMuted, mp::SystemFont::kLarge, true);
        } else if (active_ == TestId::kNetwork) {
            view.Text({center, layout_.content.y + 52}, network_ready_ ? "系统网络路由：在线" : "系统网络路由：离线",
                      network_ready_ ? kAccent : kDanger, mp::SystemFont::kLarge, true);
            view.Text({center, layout_.content.y + 104}, voice_route_ready_ ? "语音网关路由：在线" : "语音网关路由：离线",
                      voice_route_ready_ ? kAccent : kDanger, mp::SystemFont::kLarge, true);
        } else if (active_ == TestId::kSoak) {
            mp::FixedString<128U> line;
            line.Append("已运行 "); line.AppendUint(elapsed_ticks_ / 10U); line.Append(" 秒   接口错误 ");
            line.AppendUint(soak_failures_); line.Append("   路由掉线 "); line.AppendUint(soak_route_losses_);
            view.Text({center, layout_.content.y + 70}, line.c_str(), kMuted, mp::SystemFont::kLarge, true);
        } else {
            view.Text({center, layout_.content.y + 70}, kTests[Index(active_)].subtitle, kMuted,
                      mp::SystemFont::kLarge, true);
        }
    }

    [[nodiscard]] static constexpr uint32_t Index(TestId id) { return static_cast<uint32_t>(id); }

    [[nodiscard]] static const char* TestName(TestId id) {
        constexpr const char* names[kTestCount]{"microphone", "speaker", "model", "voice-flow",
                                                "network", "touch", "storage", "soak"};
        return names[Index(id)];
    }

    mp::Application app_;
    mp::Renderer renderer_;
    mp::RendererInfo display_;
    Layout layout_{};
    mp::Scene scene_;
    mp::ContainerNode view_root_{};
    mp::ContainerNode controls_{};
    View view_;
    mp::ContainerNode home_controls_{};
    mp::ContainerNode page_controls_{};
    mp::Timer ticker_;
    std::array<mp::ui::TextButton, kTestCount> menu_buttons_{};
    std::array<mp::ui::TextButton, 4U> action_buttons_{};
    mp::ui::TextButton back_button_{};
    std::array<Verdict, kTestCount> verdicts_{};
    mp::FixedString<192U> status_{};
    TestId active_{TestId::kMicrophone};
    bool home_{true};
    uint32_t pass_mask_{};
    uint32_t fail_mask_{};
    uint32_t elapsed_ticks_{};
    uint32_t mic_blocks_{};
    int16_t mic_min_{INT16_MAX};
    int16_t mic_max_{INT16_MIN};
    uint32_t mic_peak_{};
    uint8_t touch_mask_{};
    bool network_ready_{};
    bool voice_route_ready_{};
    bool voice_transcript_{};
    bool voice_response_{};
    bool voice_audio_{};
    bool voice_listening_{};
    bool voice_speaking_{};
    bool voice_ended_{};
    bool voice_error_{};
    uint32_t soak_failures_{};
    uint32_t soak_route_losses_{};
};

}  // namespace

int main() {
    DiagnosticApp app;
    return app.Run();
}
