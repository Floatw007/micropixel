#pragma once

#include "host/ui/gesture_thresholds.hpp"
#include "host/ui/lvgl/square_common/profiles/landscape_1024x600_layout.hpp"
#include "host/ui/lvgl/square_common/square_ui_state.hpp"

namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_1024x600 {

inline constexpr SquareLayout kSquareLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .hall_card_width = Layout::kHallCardWidth,
    .hall_card_height = Layout::kHallCardHeight,
    .transition_intermediate_width = 512U,
    .guest_transition_duration_ms = 0U,
    .fullscreen_to_intermediate_scale = 0.5F,
    .intermediate_to_card_scale = 0.5F,
};

inline constexpr HallCardLayout kHallCardLayout{
    .width = Layout::kHallCardWidth,
    .height = Layout::kHallCardHeight,
    .radius = 24,
    .border_width = 1,
    .label_y = 253,
    .label_x = 14,
    .label_width = 222,
    .label_height = 30,
    .running_x = 14,
    .running_y = 14,
    .running_width = 112,
    .running_height = 44,
    .stop_x = 190,
    .stop_y = 14,
    .stop_size = 44,
    .stop_hit_padding = 10,
    .stop_icon_size = 18,
    .label_font = platform::lvgl::SystemFontRole::kLarge,
    .badge_font = platform::lvgl::SystemFontRole::kSmall,
};

inline constexpr HallSceneLayout kHallSceneLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .brand = {.x = 40, .y = 63, .width = 18, .height = 44},
    .brand_radius = 9,
    .title = {.x = 76, .y = 59},
    .section = {.x = 40, .y = 132},
    .settings_button = {.x = 800, .y = 62, .width = 184, .height = 64},
    .update_button = {.x = 600, .y = 62, .width = 184, .height = 64},
    .header_button_radius = 22,
    .header_button_icon_x = 18,
    .header_button_label_x = 50,
    .header_button_font = platform::lvgl::SystemFontRole::kLarge,
    .header_button_pad_horizontal = 18,
    .carousel = {.x = Layout::kHallLeft,
                 .y = Layout::kHallTop,
                 .width = Layout::kHallViewportWidth,
                 .height = Layout::kHallCardHeight},
    .card_width = Layout::kHallCardWidth,
    .card_gap = Layout::kHallCardGap,
    .content_trailing_width = Layout::kHallLeft,
    .fully_visible_cards = 3U,
    .scroll_track = {.x = 392,
                     .y = 500,
                     .width = Layout::kHallScrollTrackWidth,
                     .height = Layout::kHallScrollTrackHeight},
    .scroll_min_thumb_width = 32,
    .empty_message = {.x = 40, .y = 275},
    .simple_status = {.x = 40, .y = 524},
    .simple_app_id = {.x = 40, .y = 558},
    .status_text_width = 944,
    .status_bar =
        {
            .height = 46,
            .padding_left = 40,
            .padding_right = 23,
            .item_gap = 12,
            .battery_gap = 3,
            .time_width = 72,
            .cellular = {.width = 22, .height = 16},
            .wifi_scale = 256U,
            .battery_width = 30,
            .battery_percent_width = 48,
        },
};

static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kTopReservedEdge) >=
              kHallSceneLayout.status_bar.height);
// The square profiles reserve exactly enough height for their gesture hint.
// On the 1024x600 panel the width-scaled hint is one pixel taller than the
// height-scaled reserved edge, which is harmless because the hint is clipped
// at the display boundary.
static_assert(gesture_thresholds::ScaleExtent(Layout::kHeight, gesture_thresholds::kBottomReservedEdge) + 1 >=
              gesture_thresholds::GestureHintBottomExtent(Layout::kWidth));

inline constexpr SystemMenuLayout kSystemMenuLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 108,
    .header_padding_horizontal = 40,
    .header_padding_top = 26,
    .header_gap = 20,
    .back_button_size = 56,
    .back_button_radius = 18,
    .back_button_hit_padding = 12,
    .header_text_gap = 1,
    .content_padding_horizontal = 40,
    .content_padding_top = 8,
    .content_padding_bottom = 12,
    .row_height = 88,
    .row_gap = 12,
    .row_radius = 22,
    .row_padding_horizontal = 26,
    .row_icon_width = 56,
    .row_content_gap = 22,
    .row_text_gap = 1,
    .row_chevron_width = 24,
    .firmware_dot_size = 10,
    .scrollbar_width = 5,
    .scrollbar_radius = 3,
    .row_icon_font = platform::lvgl::SystemFontRole::kLarge,
    .row_name_font = platform::lvgl::SystemFontRole::kLarge,
    .row_detail_font = platform::lvgl::SystemFontRole::kMedium,
};

inline constexpr SystemPageLayout kSystemPageLayout{
    .width = Layout::kWidth,
    .height = Layout::kHeight,
    .header_height = 108,
    .safe_horizontal = 40,
    .header_padding_top = 26,
    .header_gap = 20,
    .header_text_gap = 1,
    .back_button_size = 56,
    .back_button_radius = 18,
    .back_button_hit_padding = 12,
    .content_padding_top = 12,
    .content_padding_bottom = 24,
    .section_gap = 18,
    .panel_gap = 12,
    .panel_padding = 22,
    .panel_radius = 22,
    .row_height = 64,
    .control_height = 64,
    .scrollbar_width = 5,
    .scrollbar_radius = 3,
    .title_font = platform::lvgl::SystemFontRole::kTitle,
    .subtitle_font = platform::lvgl::SystemFontRole::kMedium,
    .heading_font = platform::lvgl::SystemFontRole::kLarge,
    .body_font = platform::lvgl::SystemFontRole::kMedium,
    .detail_font = platform::lvgl::SystemFontRole::kMedium,
};

inline constexpr SquareSystemUiProfile kSystemUiProfile{
    .square = kSquareLayout,
    .hall_card = kHallCardLayout,
    .hall_scene = kHallSceneLayout,
    .system_menu = kSystemMenuLayout,
    .system_page = kSystemPageLayout,
    .launch_label_bottom_offset = 42,
    .scale_oversized_launch_bitmap = true,
    .derive_launch_background = false,
    .allow_software_status_animation = false,
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_1024x600
