#pragma once

#include <cstdint>

namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_1024x600 {

struct Layout final {
    static constexpr int32_t kWidth = 1024;
    static constexpr int32_t kHeight = 600;
    static constexpr int32_t kHallLeft = 40;
    static constexpr int32_t kHallTop = 165;
    static constexpr int32_t kHallViewportWidth = 984;
    static constexpr int32_t kHallCardWidth = 250;
    static constexpr int32_t kHallCardHeight = 300;
    static constexpr int32_t kHallCardGap = 24;
    static constexpr int32_t kHallScrollTrackWidth = 240;
    static constexpr int32_t kHallScrollTrackHeight = 5;
    static constexpr int32_t kStatusDialogVisibleY = 18;
};

}  // namespace micropixel::host_ui::lvgl::square_common::profiles::landscape_1024x600
