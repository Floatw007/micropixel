// SPDX-License-Identifier: Apache-2.0
#include <array>
#include <cstddef>
#include <new>
#include <optional>
#include <string_view>
#include <utility>

#include "sdk/micropixel.hpp"
#include "sdk/scene.hpp"
#include "sdk/ui/text_button.hpp"

namespace canteen {
using namespace micropixel;
using namespace micropixel::literals;

constexpr Color kBackground = Color::Rgb(242, 245, 243);
constexpr Color kSidebar = Color::Rgb(16, 62, 56);
constexpr Color kSidebarRaised = Color::Rgb(25, 76, 69);
constexpr Color kPanel = Color::Rgb(255, 255, 255);
constexpr Color kInk = Color::Rgb(24, 40, 36);
constexpr Color kMuted = Color::Rgb(107, 129, 123);
constexpr Color kAccent = Color::Rgb(80, 185, 129);
constexpr Color kGold = Color::Rgb(242, 189, 88);
constexpr Color kMint = Color::Rgb(222, 238, 232);
constexpr Color kSelection = Color::Rgb(232, 245, 240);
constexpr Color kSoft = Color::Rgb(237, 243, 241);
constexpr Color kWarning = Color::Rgb(223, 129, 80);
constexpr Color kDanger = Color::Rgb(207, 113, 100);
constexpr uint16_t kPageCount = 6U;
constexpr uint16_t kVisibleRows = 6U;
constexpr uint32_t kResponseCapacity = 65536U;
constexpr uint16_t kOrderCapacity = 64U;
constexpr uint16_t kProductCapacity = 32U;
constexpr uint16_t kReservationCapacity = 16U;

enum class Page : uint8_t { kOverview, kOrders, kCatalog, kTelemetry, kReservations, kSettings };
enum class Feed : uint8_t { kTelemetry, kOrders, kHealth, kCatalog, kReservations, kCount };
enum class NumericInput : uint8_t { kNone, kPickupCode, kStock };

struct FeedState final {
    uint64_t next_due_ms{};
    uint8_t failures{};
    bool loaded{};
};

struct TelemetryItem final {
    int64_t product_id{};
    std::array<char, 49U> product{};
    std::array<char, 65U> device{};
    std::array<char, 16U> state{};
    int64_t percent_hundredths{};
    int64_t seq{};
    int64_t pixels{};
    int64_t fps_hundredths{};
    bool has_percent{};
};

struct OrderSummary final {
    int64_t id{};
    std::array<char, 33U> number{};
    std::array<char, 24U> status{};
    std::array<char, 16U> pickup{};
    int64_t total_cents{};
};

struct ProductSummary final {
    int64_t id{};
    std::array<char, 49U> name{};
    int64_t stock{};
    bool on_shelf{};
    bool sold_out{};
};

struct ReservationSummary final {
    int64_t id{};
    int64_t capacity{};
    int64_t remaining{};
    bool enabled{};
};

struct Model final {
    std::array<TelemetryItem, 16U> telemetry{};
    std::array<OrderSummary, kOrderCapacity> orders{};
    std::array<ProductSummary, kProductCapacity> products{};
    std::array<ReservationSummary, kReservationCapacity> reservations{};
    uint16_t telemetry_count{};
    uint16_t order_count{};
    uint16_t catalog_count{};
    uint16_t reservation_count{};
    uint16_t waiting_count{};
    uint16_t preparing_count{};
    uint16_t ready_count{};
    bool telemetry_truncated{};
    bool orders_truncated{};
    bool catalog_truncated{};
    bool reservations_truncated{};
    bool java_online{};
    bool authorization_required{};
    bool stale{};
    uint64_t last_sync_ms{};
};

std::array<uint8_t, kResponseCapacity + 1U> response_buffer{};

template <size_t Capacity>
bool CopyJsonString(const json::Value& value, std::array<char, Capacity>& output) {
    const auto text = json::Reader::String(value);
    if (text.empty() || text.size() >= Capacity) return false;
    for (size_t index = 0U; index < text.size(); ++index) {
        // Business identifiers and labels are deliberately not decoded in the
        // firmware. Jackson emits ordinary UTF-8; escaped content is rejected.
        if (text[index] == '\\') return false;
        output[index] = text[index];
    }
    output[text.size()] = '\0';
    return true;
}

bool Field(const json::Reader& reader, const json::Value& object, const char* name, json::Value& output) {
    return reader.Find(object, name, output);
}

bool ParseTelemetry(const char* bytes, uint32_t size, Model& candidate) {
    json::Reader reader(bytes, size, {.max_depth = 12U, .max_items_per_container = 128U});
    json::Value root{}, items{};
    uint16_t count = 0U;
    if (!reader.Parse(root) || !Field(reader, root, "items", items) || !reader.Count(items, count)) return false;
    candidate.telemetry_count = count > candidate.telemetry.size() ? candidate.telemetry.size() : count;
    candidate.telemetry_truncated = count > candidate.telemetry.size();
    for (uint16_t index = 0U; index < candidate.telemetry_count; ++index) {
        json::Value item{}, value{};
        TelemetryItem parsed{};
        if (!reader.At(items, index, item) || !Field(reader, item, "productName", value) ||
            !CopyJsonString(value, parsed.product) || !Field(reader, item, "deviceId", value) ||
            !CopyJsonString(value, parsed.device) || !Field(reader, item, "state", value) ||
            !CopyJsonString(value, parsed.state)) return false;
        if (!Field(reader, item, "productId", value) || !json::Reader::Integer(value, parsed.product_id)) return false;
        if (Field(reader, item, "percent", value) && value.type != json::Type::kNull) {
            parsed.has_percent = json::Reader::Fixed(value, 2U, parsed.percent_hundredths);
            if (!parsed.has_percent || parsed.percent_hundredths < 0 || parsed.percent_hundredths > 10000) return false;
        }
        if (Field(reader, item, "seq", value) && value.type != json::Type::kNull &&
            !json::Reader::Integer(value, parsed.seq)) return false;
        if (Field(reader, item, "pixels", value) && value.type != json::Type::kNull &&
            !json::Reader::Integer(value, parsed.pixels)) return false;
        if (Field(reader, item, "fps", value) && value.type != json::Type::kNull &&
            !json::Reader::Fixed(value, 2U, parsed.fps_hundredths)) return false;
        candidate.telemetry[index] = parsed;
    }
    return true;
}

bool ParseOrders(const char* bytes, uint32_t size, Model& candidate) {
    json::Reader reader(bytes, size, {.max_depth = 12U, .max_items_per_container = 128U});
    json::Value root{};
    uint16_t count = 0U;
    if (!reader.Parse(root) || !reader.Count(root, count)) return false;
    candidate.order_count = count > candidate.orders.size() ? candidate.orders.size() : count;
    candidate.orders_truncated = count > candidate.orders.size();
    candidate.waiting_count = candidate.preparing_count = candidate.ready_count = 0U;
    for (uint16_t index = 0U; index < count; ++index) {
        json::Value item{}, value{};
        if (!reader.At(root, index, item) || !Field(reader, item, "status", value)) return false;
        const auto status = json::Reader::String(value);
        if (status == "PAID_PENDING_ACCEPT") ++candidate.waiting_count;
        else if (status == "PREPARING") ++candidate.preparing_count;
        else if (status == "READY") ++candidate.ready_count;
        if (index >= candidate.orders.size()) continue;
        OrderSummary parsed{};
        if (!Field(reader, item, "id", value) || !json::Reader::Integer(value, parsed.id) ||
            !Field(reader, item, "orderNo", value) || !CopyJsonString(value, parsed.number) ||
            !Field(reader, item, "status", value) || !CopyJsonString(value, parsed.status) ||
            !Field(reader, item, "pickupCode", value) || !CopyJsonString(value, parsed.pickup) ||
            !Field(reader, item, "totalAmount", value) || !json::Reader::Fixed(value, 2U, parsed.total_cents)) return false;
        candidate.orders[index] = parsed;
    }
    return true;
}

bool ParseCatalog(const char* bytes, uint32_t size, Model& candidate) {
    json::Reader reader(bytes, size, {.max_depth = 12U, .max_items_per_container = 128U});
    json::Value root{}, categories{};
    uint16_t category_count = 0U;
    if (!reader.Parse(root) || !Field(reader, root, "categories", categories) ||
        !reader.Count(categories, category_count)) return false;
    uint16_t products = 0U;
    uint16_t stored = 0U;
    for (uint16_t index = 0U; index < category_count; ++index) {
        json::Value category{}, list{};
        uint16_t count = 0U;
        if (!reader.At(categories, index, category) || !Field(reader, category, "products", list) ||
            !reader.Count(list, count)) return false;
        for (uint16_t product_index = 0U; product_index < count; ++product_index) {
            json::Value product{}, value{};
            ProductSummary parsed{};
            bool boolean = false;
            if (!reader.At(list, product_index, product) || !Field(reader, product, "id", value) ||
                !json::Reader::Integer(value, parsed.id) || !Field(reader, product, "name", value) ||
                !CopyJsonString(value, parsed.name) || !Field(reader, product, "stock", value) ||
                !json::Reader::Integer(value, parsed.stock) || !Field(reader, product, "onShelf", value) ||
                !json::Reader::Boolean(value, boolean)) return false;
            parsed.on_shelf = boolean;
            if (!Field(reader, product, "soldOut", value) || !json::Reader::Boolean(value, boolean)) return false;
            parsed.sold_out = boolean;
            if (stored < candidate.products.size()) candidate.products[stored++] = parsed;
        }
        products = static_cast<uint16_t>(products + count);
    }
    candidate.catalog_count = products > candidate.products.size() ? candidate.products.size() : products;
    candidate.catalog_truncated = category_count > 8U || products > candidate.products.size();
    return true;
}

bool ParseReservations(const char* bytes, uint32_t size, Model& candidate) {
    json::Reader reader(bytes, size, {.max_depth = 8U, .max_items_per_container = 32U});
    json::Value root{};
    uint16_t count = 0U;
    if (!reader.Parse(root) || !reader.Count(root, count)) return false;
    candidate.reservation_count = count > candidate.reservations.size() ? candidate.reservations.size() : count;
    candidate.reservations_truncated = count > candidate.reservations.size();
    const uint16_t stored = count > candidate.reservations.size() ? candidate.reservations.size() : count;
    for (uint16_t index = 0U; index < stored; ++index) {
        json::Value item{}, value{};
        ReservationSummary parsed{};
        if (!reader.At(root, index, item) || !Field(reader, item, "id", value) ||
            !json::Reader::Integer(value, parsed.id) || !Field(reader, item, "capacity", value) ||
            !json::Reader::Integer(value, parsed.capacity) || !Field(reader, item, "remaining", value) ||
            !json::Reader::Integer(value, parsed.remaining) || !Field(reader, item, "enabled", value) ||
            !json::Reader::Boolean(value, parsed.enabled)) return false;
        candidate.reservations[index] = parsed;
    }
    return true;
}

bool ParseHealth(const char* bytes, uint32_t size) {
    json::Reader reader(bytes, size, {.max_depth = 4U, .max_items_per_container = 8U});
    json::Value root{}, status{};
    return reader.Parse(root) && Field(reader, root, "status", status) && json::Reader::String(status) == "UP";
}

class MerchantTerminal final {
   public:
    explicit MerchantTerminal(Application& app)
        : app_(app),
          renderer_(app.renderer()),
          scene_(CreateScene()),
          header_(scene_.CreateContainer({.z_order = 2}).value()),
          sidebar_(scene_.CreateContainer({.z_order = 1}).value()),
          keypad_(scene_.CreateContainer({.z_order = 3}).value()),
          device_keyboard_(scene_.CreateContainer({.z_order = 3}).value()) {
        BuildChrome();
        BuildPages();
        BuildKeypad();
        BuildDeviceKeyboard();
        RefreshNetworkInfo();
        model_.authorization_required = !configured_;
        ShowPage(Page::kOverview);
        Render(true);
    }

    void Run() {
        ticker_.emplace(app_.timers().Every(250_ms).value());
        ScheduleAll(true);
        app_.Run([&](const Event& event) {
            bool redraw = false;
            if (event.type() == EventType::kStop) {
                if (active_.has_value()) (void)active_->Cancel();
                return;
            }
            if (ticker_.has_value() && event.TimerFrom(*ticker_) != nullptr) Poll();
            if (active_.has_value()) {
                if (const NetworkCompleteEvent* completed = event.NetworkFrom(*active_)) redraw = Complete(*completed);
            }
            if (const TouchEvent* touch = event.touch()) redraw = HandleTouch(*touch) || redraw;
            if (event.type() == EventType::kResume) {
                RefreshNetworkInfo();
                ScheduleAll(true);
                redraw = true;
            }
            if (redraw) Render(false);
        });
    }

   private:
    Scene CreateScene() {
        renderer_.ConfigureDisplay({.logical_size = {1024U, 600U}, .scale_mode = DisplayScaleMode::kExpand}).value();
        return renderer_.CreateScene(kBackground).value();
    }

    void BuildChrome() {
        (void)header_.CreateShape({170, 0, 854, 64}, kPanel).value();
        (void)header_.CreateLabel({194, 18}, "食堂商家端", kMuted, SystemFont::kSmall).value();
        header_title_ = header_.CreateLabel({306, 12}, "经营概览", kInk, SystemFont::kLarge).value();
        network_pill_ = header_.CreateRoundedRect({830, 14, 174, 36}, {.fill = kMint, .radius = 18U}).value();
        network_label_ = header_.CreateLabel({917, 22}, "正在初始化", kMuted, SystemFont::kSmall, true).value();

        (void)sidebar_.CreateShape({0, 0, 170, 600}, kSidebar).value();
        (void)sidebar_.CreateRoundedRect({16, 18, 40, 40}, {.fill = kGold, .radius = 12U}).value();
        (void)sidebar_.CreateLabel({36, 25}, "食", kSidebar, SystemFont::kMedium, true).value();
        (void)sidebar_.CreateLabel({68, 18}, "食堂终端", Color::White(), SystemFont::kMedium).value();
        (void)sidebar_.CreateLabel({68, 43}, "轻食窗口", Color::Rgb(168, 193, 187), SystemFont::kSmall).value();
        (void)sidebar_.CreateRoundedRect({12, 76, 146, 50}, {.fill = kSidebarRaised, .radius = 10U}).value();
        (void)sidebar_.CreateLabel({24, 85}, "一食堂 · 轻食", Color::White(), SystemFont::kSmall).value();
        (void)sidebar_.CreateLabel({24, 105}, "终端已连接", Color::Rgb(181, 209, 202), SystemFont::kSmall).value();
        (void)sidebar_.CreateLabel({16, 558}, "P4-TERMINAL-01", Color::White(), SystemFont::kSmall).value();
        (void)sidebar_.CreateLabel({16, 580}, "1024×600", Color::Rgb(168, 193, 187), SystemFont::kSmall).value();
        constexpr std::array<const char*, kPageCount> names{"概览", "订单", "菜品", "余量", "预约", "设置"};
        for (uint16_t index = 0U; index < kPageCount; ++index) {
            nav_[index] = sidebar_.CreateTextButton(
                {.bounds = {12, 138 + static_cast<int32_t>(index) * 58, 146, 50},
                 .text = names[index],
                 .style = {.background = kSidebar, .text = Color::Rgb(190, 209, 204),
                           .feedback = kSidebarRaised, .font = SystemFont::kMedium, .corner_radius = 12U}});
        }
    }

    void BuildPages() {
        constexpr std::array<const char*, kPageCount> titles{"经营概览", "订单", "菜品", "实时余量", "预约", "终端状态"};
        constexpr std::array<const char*, kPageCount> hints{
            "订单、余量和连接状态", "选择订单后，只显示当前可执行的操作", "上架状态和人工库存",
            "K230 识别结果仅作展示", "调整已有预约时段", "网络、云端服务与设备信息"};
        for (uint16_t page = 0U; page < kPageCount; ++page) {
            pages_[page] = scene_.CreateContainer({.z_order = 0}).value();
            if (page == static_cast<uint16_t>(Page::kOverview)) continue;
            (void)pages_[page].CreateLabel({190, 82}, titles[page], kInk, SystemFont::kLarge).value();
            (void)pages_[page].CreateLabel({190, 116}, hints[page], kMuted, SystemFont::kSmall).value();
            (void)pages_[page].CreateRoundedRect({186, 148, 818, 426}, {.fill = kPanel, .radius = 15U}).value();
            for (uint16_t row = 0U; row < kVisibleRows; ++row) {
                selection_marks_[page][row] =
                    pages_[page]
                        .CreateRoundedRect({198, 160 + static_cast<int32_t>(row) * 54, 794, 48},
                                           {.fill = kSelection, .radius = 10U})
                        .value();
                selection_marks_[page][row].SetVisible(false);
                row_buttons_[page][row] =
                    ui::Button({198, 160 + static_cast<int32_t>(row) * 54, 794, 48});
                row_buttons_[page][row].SetEnabled(page >= static_cast<uint16_t>(Page::kOrders) &&
                                                   page <= static_cast<uint16_t>(Page::kReservations));
                rows_[page][row] = pages_[page]
                    .CreateLabel({212, 172 + static_cast<int32_t>(row) * 54}, "—", kInk, SystemFont::kMedium)
                    .value();
            }
        }
        BuildOverview();
        order_actions_[0] = CreateAction(pages_[static_cast<uint8_t>(Page::kOrders)], 198, "接单");
        order_actions_[1] = CreateDangerAction(pages_[static_cast<uint8_t>(Page::kOrders)], 340, "拒单");
        order_actions_[2] = CreateGoldAction(pages_[static_cast<uint8_t>(Page::kOrders)], 198, "确认出餐");
        order_actions_[3] = CreateGoldAction(pages_[static_cast<uint8_t>(Page::kOrders)], 198, "取餐核销");
        catalog_actions_[0] = CreateAction(pages_[static_cast<uint8_t>(Page::kCatalog)], 198, "切换上架");
        catalog_actions_[1] = CreateDangerAction(pages_[static_cast<uint8_t>(Page::kCatalog)], 340, "切换售罄");
        catalog_actions_[2] = CreateGoldAction(pages_[static_cast<uint8_t>(Page::kCatalog)], 340, "调整库存");
        telemetry_actions_[0] = CreateGoldAction(pages_[static_cast<uint8_t>(Page::kTelemetry)], 198, "绑定 / 换绑");
        telemetry_actions_[1] = CreateAction(pages_[static_cast<uint8_t>(Page::kTelemetry)], 340, "换绑");
        telemetry_actions_[2] = CreateDangerAction(pages_[static_cast<uint8_t>(Page::kTelemetry)], 340, "解除绑定");
        catalog_actions_[1].SetVisible(false);
        telemetry_actions_[1].SetVisible(false);
        reservation_action_ = CreateGoldAction(pages_[static_cast<uint8_t>(Page::kReservations)], 198, "切换启停");
        for (uint16_t page = static_cast<uint16_t>(Page::kOrders);
             page <= static_cast<uint16_t>(Page::kReservations); ++page) {
            page_actions_[page][0] = CreatePager(pages_[page], 790, "上一页");
            page_actions_[page][1] = CreatePager(pages_[page], 898, "下一页");
        }
    }

    void BuildOverview() {
        auto& page = pages_[static_cast<uint8_t>(Page::kOverview)];
        (void)page.CreateRoundedRect({186, 78, 818, 88}, {.fill = kMint, .radius = 16U}).value();
        (void)page.CreateLabel({206, 92}, "上午好，轻食窗口", kInk, SystemFont::kLarge).value();
        (void)page.CreateLabel({206, 126}, "订单、菜品与 K230 视觉余量正在实时同步", Color::Rgb(76, 135, 115),
                               SystemFont::kSmall).value();
        constexpr std::array<const char*, 4U> titles{"待接单", "制作中", "待取餐", "余量设备"};
        for (uint16_t index = 0U; index < titles.size(); ++index) {
            const int32_t x = 186 + static_cast<int32_t>(index) * 207;
            (void)page.CreateRoundedRect({x, 178, 197, 88},
                                         {.fill = index == 0U ? kSidebar : kPanel, .radius = 14U}).value();
            overview_metric_titles_[index] =
                page.CreateLabel({x + 14, 190}, titles[index], index == 0U ? Color::Rgb(190, 209, 204) : kMuted,
                                 SystemFont::kSmall).value();
            overview_metric_values_[index] =
                page.CreateLabel({x + 14, 214}, "0", index == 0U ? Color::White() : kInk, SystemFont::kTitle).value();
        }
        (void)page.CreateRoundedRect({186, 278, 404, 286}, {.fill = kPanel, .radius = 15U}).value();
        (void)page.CreateRoundedRect({600, 278, 404, 286}, {.fill = kPanel, .radius = 15U}).value();
        (void)page.CreateLabel({204, 294}, "连接状态", kInk, SystemFont::kMedium).value();
        (void)page.CreateLabel({618, 294}, "业务摘要", kInk, SystemFont::kMedium).value();
        constexpr std::array<Point, kVisibleRows> positions{{{204, 338}, {204, 382}, {204, 426},
                                                             {618, 338}, {618, 382}, {618, 426}}};
        for (uint16_t row = 0U; row < kVisibleRows; ++row)
            rows_[static_cast<uint8_t>(Page::kOverview)][row] =
                page.CreateLabel(positions[row], "—", row == 0U ? kAccent : kMuted, SystemFont::kMedium).value();
    }

    void BuildKeypad() {
        (void)keypad_.CreateShape({170, 64, 854, 536}, kInk, 210U).value();
        (void)keypad_.CreateRoundedRect({326, 86, 520, 482}, {.fill = kPanel, .radius = 18U}).value();
        keypad_prompt_ = keypad_.CreateLabel({576, 116}, "请输入六位取餐码", kInk,
                                             SystemFont::kLarge, true).value();
        keypad_value_ = keypad_.CreateLabel({576, 166}, "______", kAccent, SystemFont::kTitle, true).value();
        constexpr std::array<const char*, 12U> keys{"1", "2", "3", "4", "5", "6", "7", "8", "9", "取消", "0", "确认"};
        for (uint16_t index = 0U; index < keys.size(); ++index) {
            const int32_t column = index % 3U;
            const int32_t row = index / 3U;
            keypad_buttons_[index] = keypad_.CreateTextButton(
                {.bounds = {364 + column * 142, 220 + row * 78, 118, 58},
                 .text = keys[index],
                 .style = {.background = index == 11U ? kGold : kSoft,
                           .text = index == 11U ? kSidebar : kInk, .feedback = kMuted,
                           .font = SystemFont::kLarge, .corner_radius = 12U}});
        }
        keypad_.SetVisible(false);
    }

    void BuildDeviceKeyboard() {
        (void)device_keyboard_.CreateShape({170, 64, 854, 536}, kInk, 224U).value();
        (void)device_keyboard_.CreateRoundedRect({182, 78, 830, 500}, {.fill = kPanel, .radius = 18U}).value();
        (void)device_keyboard_.CreateLabel({597, 96}, "输入 K230 设备号", kInk, SystemFont::kLarge, true).value();
        device_value_ = device_keyboard_.CreateLabel({597, 134}, "_", kAccent, SystemFont::kMedium, true).value();
        constexpr std::array<const char*, 40U> keys{
            "q", "w", "e", "r", "t", "y", "u", "i", "o", "p",
            "a", "s", "d", "f", "g", "h", "j", "k", "l", "_",
            "z", "x", "c", "v", "b", "n", "m", ".", "-", "退格",
            "0", "1", "2", "3", "4", "5", "6", "7", "8", "9"};
        for (uint16_t index = 0U; index < keys.size(); ++index) {
            const int32_t column = index % 10U;
            const int32_t row = index / 10U;
            device_keys_[index] = device_keyboard_.CreateTextButton(
                {.bounds = {198 + column * 79, 176 + row * 70, 68, 52},
                 .text = keys[index],
                 .style = {.background = kSoft, .text = kInk, .feedback = kMuted,
                           .font = SystemFont::kMedium, .corner_radius = 9U}});
        }
        device_controls_[0] = device_keyboard_.CreateTextButton(
            {.bounds = {218, 472, 160, 54}, .text = "大写",
             .style = {.background = kSoft, .text = kInk, .feedback = kMuted,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
        device_controls_[1] = device_keyboard_.CreateTextButton(
            {.bounds = {398, 472, 160, 54}, .text = "取消",
             .style = {.background = kSoft, .text = kInk, .feedback = kMuted,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
        device_controls_[2] = device_keyboard_.CreateTextButton(
            {.bounds = {578, 472, 160, 54}, .text = "清空",
             .style = {.background = kSoft, .text = kInk, .feedback = kMuted,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
        device_controls_[3] = device_keyboard_.CreateTextButton(
            {.bounds = {758, 472, 160, 54}, .text = "确认绑定",
             .style = {.background = kGold, .text = kSidebar, .feedback = kWarning,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
        device_keyboard_.SetVisible(false);
    }

    static ui::TextButton CreateAction(ContainerNode& parent, int32_t x, const char* text) {
        return parent.CreateTextButton(
            {.bounds = {x, 512, 130, 50}, .text = text,
             .style = {.background = kSidebar, .text = Color::White(), .feedback = kSidebarRaised,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
    }

    static ui::TextButton CreateGoldAction(ContainerNode& parent, int32_t x, const char* text) {
        return parent.CreateTextButton(
            {.bounds = {x, 512, 130, 50}, .text = text,
             .style = {.background = kGold, .text = kSidebar, .feedback = kWarning,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
    }

    static ui::TextButton CreateDangerAction(ContainerNode& parent, int32_t x, const char* text) {
        return parent.CreateTextButton(
            {.bounds = {x, 512, 130, 50}, .text = text,
             .style = {.background = Color::Rgb(253, 234, 231), .text = Color::Rgb(167, 80, 67),
                       .feedback = kDanger, .font = SystemFont::kMedium, .corner_radius = 10U}});
    }

    static ui::TextButton CreatePager(ContainerNode& parent, int32_t x, const char* text) {
        return parent.CreateTextButton(
            {.bounds = {x, 512, 100, 50}, .text = text,
             .style = {.background = kSoft, .text = kSidebar, .feedback = kMuted,
                       .font = SystemFont::kMedium, .corner_radius = 10U}});
    }

    void ShowPage(Page page) {
        active_page_ = page;
        constexpr std::array<const char*, kPageCount> titles{"经营概览", "订单处理", "菜品管理", "实时余量", "预约管理", "终端状态"};
        header_title_.SetText(titles[static_cast<uint16_t>(page)]);
        for (uint16_t index = 0U; index < kPageCount; ++index) {
            const bool selected = index == static_cast<uint16_t>(page);
            pages_[index].SetVisible(selected);
            (void)nav_[index].SetStyle({.background = selected ? kGold : kSidebar,
                                        .text = selected ? kSidebar : Color::Rgb(190, 209, 204),
                                        .feedback = kSidebarRaised, .font = SystemFont::kMedium,
                                        .corner_radius = 12U});
        }
    }

    bool HandleTouch(const TouchEvent& touch) {
        if (device_keyboard_open_) {
            for (uint16_t index = 0U; index < device_keys_.size(); ++index) {
                const auto update = device_keys_[index].OnTouch(touch);
                if (update.clicked) {
                    HandleDeviceKey(index);
                    return true;
                }
                if (update.redraw()) return true;
            }
            for (uint16_t index = 0U; index < device_controls_.size(); ++index) {
                const auto update = device_controls_[index].OnTouch(touch);
                if (update.clicked) {
                    HandleDeviceControl(index);
                    return true;
                }
                if (update.redraw()) return true;
            }
            return false;
        }
        if (numeric_input_ != NumericInput::kNone) {
            for (uint16_t index = 0U; index < keypad_buttons_.size(); ++index) {
                const auto update = keypad_buttons_[index].OnTouch(touch);
                if (update.clicked) {
                    if (index <= 8U) AppendNumericDigit(static_cast<char>('1' + index));
                    else if (index == 9U) CloseNumericInput();
                    else if (index == 10U) AppendNumericDigit('0');
                    else ConfirmNumericInput();
                    return true;
                }
                if (update.redraw()) return true;
            }
            return false;
        }
        for (uint16_t index = 0U; index < kPageCount; ++index) {
            const ui::ButtonUpdate update = nav_[index].OnTouch(touch);
            if (update.clicked) {
                ShowPage(static_cast<Page>(index));
                for (auto& feed : feeds_) feed.next_due_ms = 0U;
                if (active_page_ == Page::kCatalog) feeds_[static_cast<uint8_t>(Feed::kCatalog)].next_due_ms = 0U;
                if (active_page_ == Page::kReservations) feeds_[static_cast<uint8_t>(Feed::kReservations)].next_due_ms = 0U;
                return true;
            }
            if (update.redraw()) return true;
        }
        const uint16_t page = static_cast<uint16_t>(active_page_);
        if (active_page_ >= Page::kOrders && active_page_ <= Page::kReservations) {
            for (uint16_t row = 0U; row < kVisibleRows; ++row) {
                const auto update = row_buttons_[page][row].OnTouch(touch);
                if (update.clicked) {
                    const uint16_t index = static_cast<uint16_t>(page_offsets_[page] + row);
                    if (index < ItemCount(active_page_)) selected_indices_[page] = index;
                    return true;
                }
                if (update.redraw()) return true;
            }
            for (uint16_t direction = 0U; direction < 2U; ++direction) {
                const auto update = page_actions_[page][direction].OnTouch(touch);
                if (update.clicked) {
                    ChangePage(active_page_, direction != 0U);
                    return true;
                }
                if (update.redraw()) return true;
            }
        }
        if (active_page_ == Page::kOrders) {
            const std::string_view status = model_.order_count == 0U
                                                ? std::string_view{}
                                                : std::string_view(model_.orders[Selected(Page::kOrders)].status.data());
            for (uint16_t index = 0U; index < order_actions_.size(); ++index) {
                const bool visible = (index <= 1U && status == "PAID_PENDING_ACCEPT") ||
                                     (index == 2U && status == "PREPARING") ||
                                     (index == 3U && status == "READY");
                if (!visible) continue;
                const auto update = order_actions_[index].OnTouch(touch);
                if (update.clicked) { HandleOrderAction(index); return true; }
                if (update.redraw()) return true;
            }
        } else if (active_page_ == Page::kCatalog) {
            for (uint16_t index = 0U; index < catalog_actions_.size(); ++index) {
                if (index == 1U) continue;
                const auto update = catalog_actions_[index].OnTouch(touch);
                if (update.clicked) { HandleCatalogAction(index); return true; }
                if (update.redraw()) return true;
            }
        } else if (active_page_ == Page::kTelemetry) {
            for (uint16_t index = 0U; index < telemetry_actions_.size(); ++index) {
                if (index == 1U) continue;
                const auto update = telemetry_actions_[index].OnTouch(touch);
                if (update.clicked) { HandleTelemetryAction(index); return true; }
                if (update.redraw()) return true;
            }
        } else if (active_page_ == Page::kReservations) {
            const auto update = reservation_action_.OnTouch(touch);
            if (update.clicked) { HandleReservationAction(); return true; }
            if (update.redraw()) return true;
        }
        return false;
    }

    uint16_t ItemCount(Page page) const {
        switch (page) {
            case Page::kOrders: return model_.order_count;
            case Page::kCatalog: return model_.catalog_count;
            case Page::kTelemetry: return model_.telemetry_count;
            case Page::kReservations: return model_.reservation_count;
            default: return 0U;
        }
    }

    void ChangePage(Page page, bool forward) {
        const uint16_t slot = static_cast<uint16_t>(page);
        const uint16_t count = ItemCount(page);
        if (forward) {
            if (page_offsets_[slot] + kVisibleRows < count) page_offsets_[slot] += kVisibleRows;
        } else if (page_offsets_[slot] >= kVisibleRows) {
            page_offsets_[slot] -= kVisibleRows;
        }
        if (count != 0U) selected_indices_[slot] = page_offsets_[slot];
    }

    void NormalizeSelection(Page page) {
        const uint16_t slot = static_cast<uint16_t>(page);
        const uint16_t count = ItemCount(page);
        if (count == 0U) {
            page_offsets_[slot] = 0U;
            selected_indices_[slot] = 0U;
            return;
        }
        if (selected_indices_[slot] >= count) selected_indices_[slot] = count - 1U;
        if (page_offsets_[slot] >= count) page_offsets_[slot] = static_cast<uint16_t>(((count - 1U) / kVisibleRows) * kVisibleRows);
    }

    uint16_t Selected(Page page) const { return selected_indices_[static_cast<uint16_t>(page)]; }

    void HandleOrderAction(uint16_t action) {
        if (Selected(Page::kOrders) >= model_.order_count) return;
        const OrderSummary& order = model_.orders[Selected(Page::kOrders)];
        const std::string_view status(order.status.data());
        if ((action <= 1U && status != "PAID_PENDING_ACCEPT") || (action == 2U && status != "PREPARING") ||
            (action == 3U && status != "READY")) return;
        if (action == 3U) {
            numeric_target_id_ = order.id;
            OpenNumericInput(NumericInput::kPickupCode);
            return;
        }
        FixedString<128> path;
        path.Append("/api/merchant/orders/"); path.AppendInt(order.id);
        path.Append(action == 0U ? "/accept" : action == 1U ? "/reject" : "/ready");
        constexpr const char* kRejectBody = "{\"reason\":\"商家暂时无法制作\"}";
        uint32_t reject_size = 0U;
        while (kRejectBody[reject_size] != '\0') ++reject_size;
        StartWrite(HttpMethod::kPost, path.c_str(), action == 1U ? kRejectBody : nullptr,
                   action == 1U ? reject_size : 0U, Feed::kOrders);
    }

    void OpenNumericInput(NumericInput mode) {
        numeric_entry_.Clear();
        numeric_input_ = mode;
        keypad_prompt_.SetText(mode == NumericInput::kPickupCode ? "请输入六位取餐码"
                                                                    : "请输入新库存（0-999999）");
        keypad_value_.SetText(mode == NumericInput::kPickupCode ? "______" : "_");
        keypad_.SetVisible(true);
    }

    void AppendNumericDigit(char digit) {
        if (numeric_entry_.size() >= 6U) return;
        char text[2]{digit, '\0'};
        numeric_entry_.Append(text);
        FixedString<16> masked;
        masked.Append(numeric_entry_.c_str());
        if (numeric_input_ == NumericInput::kPickupCode) {
            for (uint32_t index = numeric_entry_.size(); index < 6U; ++index) masked.Append("_");
        }
        keypad_value_.SetText(masked.c_str());
    }

    void CloseNumericInput() {
        numeric_input_ = NumericInput::kNone;
        keypad_.SetVisible(false);
        numeric_target_id_ = 0;
        numeric_entry_.Clear();
    }

    void ConfirmNumericInput() {
        if (numeric_target_id_ <= 0 || numeric_entry_.empty()) return;
        if (numeric_input_ == NumericInput::kPickupCode && numeric_entry_.size() != 6U) return;
        if (numeric_input_ == NumericInput::kStock) {
            int64_t stock = 0;
            for (uint32_t index = 0U; index < numeric_entry_.size(); ++index) {
                stock = stock * 10 + static_cast<int64_t>(numeric_entry_.c_str()[index] - '0');
            }
            if (Selected(Page::kCatalog) >= model_.catalog_count) return;
            const ProductSummary& product = model_.products[Selected(Page::kCatalog)];
            FixedString<128> path;
            path.Append("/api/merchant/products/"); path.AppendInt(numeric_target_id_); path.Append("/availability");
            FixedString<160> body;
            body.Append("{\"onShelf\":"); body.Append(product.on_shelf ? "true" : "false");
            body.Append(",\"soldOut\":"); body.Append(stock == 0 ? "true" : "false");
            body.Append(",\"stock\":"); body.AppendInt(stock); body.Append("}");
            keypad_.SetVisible(false);
            numeric_input_ = NumericInput::kNone;
            numeric_target_id_ = 0;
            numeric_entry_.Clear();
            StartWrite(HttpMethod::kPatch, path.c_str(), body.c_str(), body.size(), Feed::kCatalog);
            return;
        }
        FixedString<128> path;
        path.Append("/api/merchant/orders/"); path.AppendInt(numeric_target_id_); path.Append("/complete");
        FixedString<64> body;
        body.Append("{\"pickupCode\":\""); body.Append(numeric_entry_.c_str()); body.Append("\"}");
        keypad_.SetVisible(false);
        numeric_input_ = NumericInput::kNone;
        StartWrite(HttpMethod::kPost, path.c_str(), body.c_str(), body.size(), Feed::kOrders);
        numeric_target_id_ = 0;
        numeric_entry_.Clear();
    }

    void HandleCatalogAction(uint16_t action) {
        if (Selected(Page::kCatalog) >= model_.catalog_count) return;
        const ProductSummary& product = model_.products[Selected(Page::kCatalog)];
        if (action == 2U) {
            numeric_target_id_ = product.id;
            OpenNumericInput(NumericInput::kStock);
            return;
        }
        FixedString<128> path;
        path.Append("/api/merchant/products/"); path.AppendInt(product.id); path.Append("/availability");
        FixedString<160> body;
        body.Append("{\"onShelf\":"); body.Append(action == 0U ? (!product.on_shelf ? "true" : "false")
                                                               : (product.on_shelf ? "true" : "false"));
        body.Append(",\"soldOut\":"); body.Append(action == 1U ? (!product.sold_out ? "true" : "false")
                                                              : (product.sold_out ? "true" : "false"));
        body.Append(",\"stock\":"); body.AppendInt(product.stock); body.Append("}");
        StartWrite(HttpMethod::kPatch, path.c_str(), body.c_str(), body.size(), Feed::kCatalog);
    }

    void HandleTelemetryAction(uint16_t action) {
        if (action == 0U) {
            if (model_.telemetry_count != 0U && Selected(Page::kTelemetry) < model_.telemetry_count)
                bind_product_id_ = model_.telemetry[Selected(Page::kTelemetry)].product_id;
            else if (model_.catalog_count != 0U && Selected(Page::kCatalog) < model_.catalog_count)
                bind_product_id_ = model_.products[Selected(Page::kCatalog)].id;
            else return;
            OpenDeviceKeyboard();
            return;
        }
        if (Selected(Page::kTelemetry) >= model_.telemetry_count) return;
        const TelemetryItem& item = model_.telemetry[Selected(Page::kTelemetry)];
        if (action == 1U) {
            bind_product_id_ = item.product_id;
            OpenDeviceKeyboard();
            return;
        }
        FixedString<128> path;
        path.Append("/api/merchant/products/"); path.AppendInt(item.product_id);
        path.Append("/telemetry-binding");
        StartWrite(HttpMethod::kDelete, path.c_str(), nullptr, 0U, Feed::kTelemetry);
    }

    void HandleReservationAction() {
        if (Selected(Page::kReservations) >= model_.reservation_count) return;
        const ReservationSummary& reservation = model_.reservations[Selected(Page::kReservations)];
        FixedString<128> path;
        path.Append("/api/merchant/reservation-slots/"); path.AppendInt(reservation.id); path.Append("/enabled");
        const char* body = reservation.enabled ? "{\"enabled\":false}" : "{\"enabled\":true}";
        uint32_t size = 0U; while (body[size] != '\0') ++size;
        StartWrite(HttpMethod::kPatch, path.c_str(), body, size, Feed::kReservations);
    }

    void OpenDeviceKeyboard() {
        device_entry_.Clear();
        device_uppercase_ = false;
        (void)device_controls_[0].SetText("大写");
        device_value_.SetText("_");
        device_keyboard_open_ = true;
        device_keyboard_.SetVisible(true);
    }

    void RefreshDeviceValue() { device_value_.SetText(device_entry_.empty() ? "_" : device_entry_.c_str()); }

    void PopDeviceCharacter() {
        if (device_entry_.empty()) return;
        FixedString<65> shortened;
        for (uint32_t index = 0U; index + 1U < device_entry_.size(); ++index) {
            char text[2]{device_entry_.c_str()[index], '\0'};
            shortened.Append(text);
        }
        device_entry_ = shortened;
        RefreshDeviceValue();
    }

    void HandleDeviceKey(uint16_t index) {
        if (index == 29U) {
            PopDeviceCharacter();
            return;
        }
        if (device_entry_.size() >= 64U) return;
        constexpr char keys[] = "qwertyuiopasdfghjkl_zxcvbnm.-#0123456789";
        char value = keys[index];
        if (device_uppercase_ && value >= 'a' && value <= 'z') value = static_cast<char>(value - 'a' + 'A');
        char text[2]{value, '\0'};
        device_entry_.Append(text);
        RefreshDeviceValue();
    }

    void HandleDeviceControl(uint16_t control) {
        if (control == 0U) {
            device_uppercase_ = !device_uppercase_;
            (void)device_controls_[0].SetText(device_uppercase_ ? "小写" : "大写");
            return;
        }
        if (control == 1U) {
            device_keyboard_open_ = false;
            device_keyboard_.SetVisible(false);
            bind_product_id_ = 0;
            device_entry_.Clear();
            return;
        }
        if (control == 2U) {
            device_entry_.Clear();
            RefreshDeviceValue();
            return;
        }
        if (device_entry_.empty() || bind_product_id_ <= 0) return;
        FixedString<128> path;
        path.Append("/api/merchant/products/"); path.AppendInt(bind_product_id_); path.Append("/telemetry-binding");
        FixedString<128> body;
        body.Append("{\"deviceId\":\""); body.Append(device_entry_.c_str()); body.Append("\"}");
        device_keyboard_open_ = false;
        device_keyboard_.SetVisible(false);
        bind_product_id_ = 0;
        device_entry_.Clear();
        StartWrite(HttpMethod::kPut, path.c_str(), body.c_str(), body.size(), Feed::kTelemetry);
    }

    void StartWrite(HttpMethod method, const char* path, const char* body, uint32_t body_length, Feed refresh) {
        if (active_.has_value() || !model_.java_online || model_.authorization_required) return;
        FixedString<64> key;
        key.Append("p4-"); key.AppendUint(NowMs()); key.Append("-"); key.AppendUint(++write_sequence_);
        auto started = app_.network().Start({.method = method, .cache_mode = NetworkCacheMode::kNone,
                                             .path = path, .body = reinterpret_cast<const uint8_t*>(body),
                                             .body_length = body_length, .idempotency_key = key.c_str()});
        if (!started.has_value()) {
            Failed(refresh);
            return;
        }
        active_.emplace(std::move(*started));
        active_feed_ = refresh;
        write_active_ = true;
    }

    uint64_t NowMs() const { return app_.clock().Now().microseconds() / 1000U; }

    uint32_t BasePeriodMs(Feed feed) const {
        switch (feed) {
            case Feed::kTelemetry:
                return active_page_ == Page::kOverview || active_page_ == Page::kTelemetry ? 2000U : 10000U;
            case Feed::kOrders:
                return active_page_ == Page::kOverview || active_page_ == Page::kOrders ? 2000U : 5000U;
            case Feed::kHealth:
                return active_page_ == Page::kOverview || active_page_ == Page::kSettings ? 10000U : 30000U;
            case Feed::kCatalog:
            case Feed::kReservations: return 60000U;
            default: return 60000U;
        }
    }

    static const char* Path(Feed feed) {
        switch (feed) {
            case Feed::kTelemetry: return "/api/merchant/telemetry-bindings";
            case Feed::kOrders: return "/api/merchant/orders?limit=64";
            case Feed::kHealth: return "/api/health";
            case Feed::kCatalog: return "/api/merchant/catalog";
            case Feed::kReservations: return "/api/merchant/reservation-slots";
            default: return "/api/health";
        }
    }

    static NetworkCacheMode Cache(Feed feed) {
        if (feed == Feed::kTelemetry || feed == Feed::kHealth) return NetworkCacheMode::kMemoryFallback;
        return NetworkCacheMode::kPersistentFallback;
    }

    void ScheduleAll(bool immediate) {
        const uint64_t now = NowMs();
        for (auto& feed : feeds_) feed.next_due_ms = immediate ? now : feed.next_due_ms;
    }

    void Poll() {
        if (active_.has_value()) return;
        const uint64_t now = NowMs();
        if (now >= next_info_ms_) {
            RefreshNetworkInfo();
            next_info_ms_ = now + 2000U;
        }
        if (!configured_) return;
        for (uint8_t offset = 0U; offset < static_cast<uint8_t>(Feed::kCount); ++offset) {
            const uint8_t index = static_cast<uint8_t>((next_feed_ + offset) % static_cast<uint8_t>(Feed::kCount));
            if (feeds_[index].next_due_ms > now) continue;
            const Feed feed = static_cast<Feed>(index);
            auto started = app_.network().Start({.method = HttpMethod::kGet, .cache_mode = Cache(feed), .path = Path(feed)});
            if (!started.has_value()) {
                Failed(feed);
                return;
            }
            active_.emplace(std::move(*started));
            active_feed_ = feed;
            next_feed_ = static_cast<uint8_t>((index + 1U) % static_cast<uint8_t>(Feed::kCount));
            return;
        }
    }

    void Failed(Feed feed) {
        static constexpr std::array<uint32_t, 5U> backoff{1000U, 2000U, 5000U, 10000U, 30000U};
        FeedState& state = feeds_[static_cast<uint8_t>(feed)];
        const uint8_t slot = state.failures < backoff.size() ? state.failures : backoff.size() - 1U;
        if (state.failures < 255U) ++state.failures;
        jitter_ = jitter_ * 1664525U + 1013904223U;
        state.next_due_ms = NowMs() + backoff[slot] + jitter_ % 251U;
        model_.java_online = false;
    }

    bool Complete(const NetworkCompleteEvent& event) {
        const Feed feed = active_feed_;
        const uint16_t http = event.http_status();
        bool parsed = false;
        if (write_active_) {
            const bool succeeded = event.transport_succeeded() && http >= 200U && http < 300U;
            if (http == 401U || http == 403U) {
                model_.authorization_required = true;
                model_.java_online = false;
            } else {
                // A timeout may hide a successful server commit. Never replay
                // the mutation here; immediately read the authoritative state.
                feeds_[static_cast<uint8_t>(feed)].next_due_ms = 0U;
                model_.java_online = succeeded;
            }
            (void)active_->Close();
            active_.reset();
            write_active_ = false;
            return true;
        }
        if (event.transport_succeeded() && event.body_length() <= kResponseCapacity && http >= 200U && http < 300U) {
            uint32_t offset = 0U;
            bool read_ok = true;
            while (offset < event.body_length()) {
                const uint32_t wanted = (event.body_length() - offset) > 4096U ? 4096U : event.body_length() - offset;
                auto chunk = active_->Read(offset, response_buffer.data() + offset, wanted);
                if (!chunk.has_value() || *chunk == 0U) { read_ok = false; break; }
                offset += *chunk;
            }
            response_buffer[event.body_length()] = 0U;
            Model candidate = model_;
            if (read_ok) {
                const char* bytes = reinterpret_cast<const char*>(response_buffer.data());
                if (feed == Feed::kTelemetry) parsed = ParseTelemetry(bytes, event.body_length(), candidate);
                else if (feed == Feed::kOrders) parsed = ParseOrders(bytes, event.body_length(), candidate);
                else if (feed == Feed::kCatalog) parsed = ParseCatalog(bytes, event.body_length(), candidate);
                else if (feed == Feed::kReservations) parsed = ParseReservations(bytes, event.body_length(), candidate);
                else parsed = event.body_length() <= 4096U && ParseHealth(bytes, event.body_length());
            }
            if (parsed) {
                if (feed == Feed::kHealth) RefreshNetworkInfo();
                candidate.java_online = true;
                candidate.authorization_required = !configured_;
                candidate.stale = event.source() != static_cast<uint8_t>(NetworkResponseSource::kLive);
                candidate.last_sync_ms = NowMs();
                model_ = candidate;
                FeedState& state = feeds_[static_cast<uint8_t>(feed)];
                state.loaded = true;
                state.failures = 0U;
                state.next_due_ms = NowMs() + BasePeriodMs(feed);
            }
        } else if (http == 401U || http == 403U) {
            model_.authorization_required = true;
            model_.java_online = false;
        }
        (void)active_->Close();
        active_.reset();
        if (!parsed && http != 401U && http != 403U) Failed(feed);
        return true;
    }

    void RefreshNetworkInfo() {
        const auto info = app_.network().info();
        configured_ = info.has_value() && info->configured;
        route_online_ = info.has_value() && info->route_online;
        time_synchronized_ = info.has_value() && info->time_synchronized;
        if (!configured_) model_.authorization_required = true;
    }

    template <uint32_t Capacity>
    static void AppendMoney(FixedString<Capacity>& line, int64_t cents) {
        if (cents < 0) { line.Append("-"); cents = -cents; }
        line.AppendUint(static_cast<uint64_t>(cents / 100));
        line.Append(".");
        if (cents % 100 < 10) line.Append("0");
        line.AppendUint(static_cast<uint64_t>(cents % 100));
    }

    void SetRow(Page page, uint16_t row, const char* text, Color color = kInk) {
        rows_[static_cast<uint8_t>(page)][row].SetText(text);
        rows_[static_cast<uint8_t>(page)][row].SetColor(color);
    }

    static const char* OrderStatusText(std::string_view status) {
        if (status == "PAID_PENDING_ACCEPT") return "待接单";
        if (status == "PREPARING") return "制作中";
        if (status == "READY") return "待取餐";
        if (status == "COMPLETED") return "已完成";
        if (status == "REJECTED") return "已拒单";
        return "状态更新中";
    }

    void RenderRows() {
        NormalizeSelection(Page::kOrders);
        NormalizeSelection(Page::kCatalog);
        NormalizeSelection(Page::kTelemetry);
        NormalizeSelection(Page::kReservations);
        FixedString<160> line;
        const std::array<uint16_t, 4U> metrics{model_.waiting_count, model_.preparing_count, model_.ready_count,
                                               model_.telemetry_count};
        for (uint16_t index = 0U; index < metrics.size(); ++index) {
            FixedString<16> value;
            value.AppendUint(metrics[index]);
            overview_metric_values_[index].SetText(value.c_str());
        }
        SetRow(Page::kOverview, 0U, model_.java_online ? "云端服务  正常" : "云端服务  离线",
               model_.java_online ? kAccent : kDanger);
        SetRow(Page::kOverview, 1U, model_.stale ? "数据来源  本地缓存" : "数据来源  实时同步",
               model_.stale ? kWarning : kMuted);
        SetRow(Page::kOverview, 2U, model_.authorization_required ? "终端配置  需要更新" : "终端配置  已完成",
               model_.authorization_required ? kDanger : kMuted);
        line.Clear(); line.Append("菜品 "); line.AppendUint(model_.catalog_count); line.Append("  ·  预约 ");
        line.AppendUint(model_.reservation_count);
        SetRow(Page::kOverview, 3U, line.c_str(), kInk);
        line.Clear();
        if (model_.telemetry_count != 0U) {
            const auto& item = model_.telemetry[0];
            line.Append(item.product.data()); line.Append("  ");
            if (item.has_percent) { line.AppendInt(item.percent_hundredths / 100); line.Append("%"); }
            else line.Append("--%");
        } else line.Append("暂无实时余量");
        SetRow(Page::kOverview, 4U, line.c_str(), model_.telemetry_count != 0U ? kAccent : kMuted);
        SetRow(Page::kOverview, 5U, "余量仅展示，不自动修改库存", kMuted);

        for (uint16_t index = 0U; index < kVisibleRows; ++index) {
            const uint16_t absolute = static_cast<uint16_t>(page_offsets_[static_cast<uint16_t>(Page::kOrders)] + index);
            line.Clear();
            if (absolute < model_.order_count) {
                const auto& order = model_.orders[absolute];
                line.Append(order.number.data()); line.Append("    ");
                line.Append(OrderStatusText(order.status.data())); line.Append("    ¥"); AppendMoney(line, order.total_cents);
            } else if (index == 0U) line.Append("暂无订单；联网后自动同步");
            else line.Append(" ");
            SetRow(Page::kOrders, index, line.c_str());
            selection_marks_[static_cast<uint16_t>(Page::kOrders)][index].SetVisible(
                absolute < model_.order_count && absolute == Selected(Page::kOrders));
            row_buttons_[static_cast<uint16_t>(Page::kOrders)][index].SetEnabled(absolute < model_.order_count);
        }

        for (uint16_t index = 0U; index < kVisibleRows; ++index) {
            const uint16_t absolute = static_cast<uint16_t>(page_offsets_[static_cast<uint16_t>(Page::kCatalog)] + index);
            line.Clear();
            if (absolute < model_.catalog_count) {
                const auto& product = model_.products[absolute];
                line.Append(product.name.data()); line.Append("  库存 "); line.AppendInt(product.stock);
                line.Append(product.on_shelf ? "  已上架" : "  已下架");
                if (product.sold_out) line.Append("  售罄");
                if (absolute == 0U && model_.catalog_truncated) line.Append("  [数据截断]");
            } else if (index == 0U) line.Append("暂无菜品；新增菜品请使用网页端");
            else line.Append(" ");
            SetRow(Page::kCatalog, index, line.c_str(), model_.catalog_truncated && absolute == 0U ? kWarning : kInk);
            selection_marks_[static_cast<uint16_t>(Page::kCatalog)][index].SetVisible(
                absolute < model_.catalog_count && absolute == Selected(Page::kCatalog));
            row_buttons_[static_cast<uint16_t>(Page::kCatalog)][index].SetEnabled(absolute < model_.catalog_count);
        }

        for (uint16_t index = 0U; index < kVisibleRows; ++index) {
            const uint16_t absolute = static_cast<uint16_t>(page_offsets_[static_cast<uint16_t>(Page::kTelemetry)] + index);
            line.Clear();
            if (absolute < model_.telemetry_count) {
                const auto& item = model_.telemetry[absolute];
                line.Append(item.product.data()); line.Append("    "); line.Append(item.device.data()); line.Append("    ");
                if (item.has_percent) { line.AppendInt(item.percent_hundredths / 100); line.Append("%"); }
                else line.Append("--%");
                line.Append("    "); line.Append(item.state.data());
            } else if (index == 0U) line.Append("暂无实时余量");
            else line.Append(" ");
            SetRow(Page::kTelemetry, index, line.c_str(), absolute < model_.telemetry_count && std::string_view(model_.telemetry[absolute].state.data()) == "LIVE" ? kAccent : kInk);
            selection_marks_[static_cast<uint16_t>(Page::kTelemetry)][index].SetVisible(
                absolute < model_.telemetry_count && absolute == Selected(Page::kTelemetry));
            row_buttons_[static_cast<uint16_t>(Page::kTelemetry)][index].SetEnabled(absolute < model_.telemetry_count);
        }

        for (uint16_t index = 0U; index < kVisibleRows; ++index) {
            const uint16_t absolute = static_cast<uint16_t>(page_offsets_[static_cast<uint16_t>(Page::kReservations)] + index);
            line.Clear();
            if (absolute < model_.reservation_count) {
                const auto& reservation = model_.reservations[absolute];
                line.Append("时段 #"); line.AppendInt(reservation.id); line.Append("  容量 ");
                line.AppendInt(reservation.capacity); line.Append("  剩余 "); line.AppendInt(reservation.remaining);
                line.Append(reservation.enabled ? "  已启用" : "  已停用");
                if (absolute == 0U && model_.reservations_truncated) line.Append("  [数据截断]");
            } else if (index == 0U) line.Append("暂无预约时段");
            else line.Append(" ");
            SetRow(Page::kReservations, index, line.c_str());
            selection_marks_[static_cast<uint16_t>(Page::kReservations)][index].SetVisible(
                absolute < model_.reservation_count && absolute == Selected(Page::kReservations));
            row_buttons_[static_cast<uint16_t>(Page::kReservations)][index].SetEnabled(absolute < model_.reservation_count);
        }

        SetRow(Page::kSettings, 0U, configured_ ? "终端配置    已配置" : "终端配置    未配置", configured_ ? kAccent : kDanger);
        SetRow(Page::kSettings, 1U, route_online_ ? "Wi-Fi 网络    在线" : "Wi-Fi 网络    离线", route_online_ ? kAccent : kDanger);
        SetRow(Page::kSettings, 2U, time_synchronized_ ? "系统时间    已校准" : "系统时间    未校准", time_synchronized_ ? kAccent : kWarning);
        SetRow(Page::kSettings, 3U, model_.java_online ? "云端服务    正常" : "云端服务    不可达",
               model_.java_online ? kAccent : kDanger);
        SetRow(Page::kSettings, 4U, "终端编号    P4-TERMINAL-01");
        SetRow(Page::kSettings, 5U, "系统版本    MicroPixel 0.9");

        const bool writes_enabled = model_.java_online && !model_.authorization_required && !write_active_;
        const std::string_view order_status = model_.order_count == 0U
                                                  ? std::string_view{}
                                                  : std::string_view(model_.orders[Selected(Page::kOrders)].status.data());
        for (uint16_t index = 0U; index < order_actions_.size(); ++index) {
            const bool visible = (index <= 1U && order_status == "PAID_PENDING_ACCEPT") ||
                                 (index == 2U && order_status == "PREPARING") ||
                                 (index == 3U && order_status == "READY");
            order_actions_[index].SetVisible(visible);
            order_actions_[index].SetEnabled(visible && writes_enabled);
        }
        catalog_actions_[0].SetEnabled(writes_enabled && model_.catalog_count != 0U);
        catalog_actions_[1].SetEnabled(false);
        catalog_actions_[1].SetVisible(false);
        catalog_actions_[2].SetEnabled(writes_enabled && model_.catalog_count != 0U);
        telemetry_actions_[0].SetEnabled(writes_enabled && (model_.telemetry_count != 0U || model_.catalog_count != 0U));
        telemetry_actions_[1].SetEnabled(false);
        telemetry_actions_[1].SetVisible(false);
        telemetry_actions_[2].SetEnabled(writes_enabled && model_.telemetry_count != 0U);
        reservation_action_.SetEnabled(writes_enabled && model_.reservation_count != 0U);
        for (uint16_t page = static_cast<uint16_t>(Page::kOrders);
             page <= static_cast<uint16_t>(Page::kReservations); ++page) {
            const uint16_t count = ItemCount(static_cast<Page>(page));
            page_actions_[page][0].SetEnabled(page_offsets_[page] != 0U);
            page_actions_[page][1].SetEnabled(page_offsets_[page] + kVisibleRows < count);
        }
    }

    void Render(bool initial) {
        RenderRows();
        FixedString<96> status;
        if (!configured_) status.Append("未配置");
        else if (model_.authorization_required) status.Append("需要重新配置");
        else if (!model_.java_online) status.Append("离线");
        else if (model_.stale) status.Append("缓存");
        else status.Append("在线");
        if (model_.last_sync_ms != 0U) { status.Append(" · "); status.AppendUint((NowMs() - model_.last_sync_ms) / 1000U); status.Append("秒前"); }
        network_label_.SetText(status.c_str());
        network_label_.SetColor(model_.java_online ? kAccent : (configured_ ? kWarning : kDanger));
        network_pill_.SetFillColor(model_.java_online ? kMint : Color::Rgb(253, 234, 231));
        if (!renderer_.Present(scene_).has_value() && initial) Panic("merchant: initial scene present failed");
    }

    Application& app_;
    Renderer renderer_;
    Scene scene_;
    ContainerNode header_{};
    ContainerNode sidebar_{};
    ContainerNode keypad_{};
    ContainerNode device_keyboard_{};
    RoundedRectNode network_pill_{};
    LabelNode header_title_{};
    LabelNode network_label_{};
    LabelNode keypad_prompt_{};
    LabelNode keypad_value_{};
    LabelNode device_value_{};
    std::array<LabelNode, 4U> overview_metric_titles_{};
    std::array<LabelNode, 4U> overview_metric_values_{};
    std::array<ui::TextButton, kPageCount> nav_{};
    std::array<ui::TextButton, 4U> order_actions_{};
    std::array<ui::TextButton, 3U> catalog_actions_{};
    std::array<ui::TextButton, 3U> telemetry_actions_{};
    ui::TextButton reservation_action_{};
    std::array<ui::TextButton, 12U> keypad_buttons_{};
    std::array<ui::TextButton, 40U> device_keys_{};
    std::array<ui::TextButton, 4U> device_controls_{};
    std::array<ContainerNode, kPageCount> pages_{};
    std::array<std::array<LabelNode, kVisibleRows>, kPageCount> rows_{};
    std::array<std::array<RoundedRectNode, kVisibleRows>, kPageCount> selection_marks_{};
    std::array<std::array<ui::Button, kVisibleRows>, kPageCount> row_buttons_{};
    std::array<std::array<ui::TextButton, 2U>, kPageCount> page_actions_{};
    std::array<uint16_t, kPageCount> page_offsets_{};
    std::array<uint16_t, kPageCount> selected_indices_{};
    std::array<FeedState, static_cast<uint8_t>(Feed::kCount)> feeds_{};
    std::optional<Timer> ticker_{};
    std::optional<NetworkRequest> active_{};
    Model model_{};
    Feed active_feed_{Feed::kHealth};
    Page active_page_{Page::kOverview};
    uint8_t next_feed_{};
    uint32_t jitter_{0x4d504958U};
    uint32_t write_sequence_{};
    uint64_t next_info_ms_{};
    int64_t numeric_target_id_{};
    int64_t bind_product_id_{};
    FixedString<7> numeric_entry_{};
    FixedString<65> device_entry_{};
    NumericInput numeric_input_{NumericInput::kNone};
    bool write_active_{};
    bool device_keyboard_open_{};
    bool device_uppercase_{};
    bool configured_{};
    bool route_online_{};
    bool time_synchronized_{};
};

alignas(MerchantTerminal) std::array<std::byte, sizeof(MerchantTerminal)> terminal_storage{};

}  // namespace canteen

int main() {
    // The retained six-page scene owns several bounded tables and node arrays.
    // Construct it in linear-memory static storage instead of consuming the
    // Guest's deliberately small 16 KiB C stack at entry. Placement storage
    // also avoids registering a process-exit destructor in the freestanding
    // Guest runtime.
    micropixel::Application app;
    auto* terminal = ::new (canteen::terminal_storage.data()) canteen::MerchantTerminal(app);
    terminal->Run();
    return 0;
}
