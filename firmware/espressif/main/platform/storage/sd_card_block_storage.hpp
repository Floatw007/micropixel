#ifndef MICROPIXEL_PLATFORM_STORAGE_SD_CARD_BLOCK_STORAGE_HPP
#define MICROPIXEL_PLATFORM_STORAGE_SD_CARD_BLOCK_STORAGE_HPP

#include <cstddef>
#include <cstdint>
#include <expected>
#include <mutex>
#include <span>

#include "device/contracts/block_storage.hpp"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_err.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"
#include "sdmmc_cmd.h"

namespace micropixel::platform::storage {

// Removable SD card connected to an SDMMC host slot. BundleFS owns the raw card rather
// than mounting FAT: sector writes emulate the erase/program contract and the
// Host never formats user media until System Settings receives confirmation.
class SdCardBlockStorage final : public device::BlockStorage {
   public:
    struct Config final {
        int slot{SDMMC_HOST_SLOT_0};
        int width{4};
        gpio_num_t clock{GPIO_NUM_NC};
        gpio_num_t command{GPIO_NUM_NC};
        gpio_num_t data0{GPIO_NUM_NC};
        gpio_num_t data1{GPIO_NUM_NC};
        gpio_num_t data2{GPIO_NUM_NC};
        gpio_num_t data3{GPIO_NUM_NC};
        int power_ldo_channel{-1};
        uint32_t max_frequency_khz{SDMMC_FREQ_DEFAULT};
    };

    SdCardBlockStorage() = default;
    ~SdCardBlockStorage() override;

    // Powers no board rails. The board must enable the card supply first.
    // Failure leaves present() false so boot can continue without a card.
    [[nodiscard]] esp_err_t Initialize(const Config& config);
    void Shutdown();

    [[nodiscard]] bool present() const { return present_; }  // NOLINT(readability-identifier-naming)
    [[nodiscard]] const device::BlockStorageGeometry& geometry() const override { return geometry_; }

    [[nodiscard]] std::expected<void, device::BlockStorageError> Read(uint64_t offset,
                                                                      std::span<uint8_t> destination) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Program(uint64_t offset,
                                                                         std::span<const uint8_t> source) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Erase(uint64_t offset, uint64_t size) override;
    [[nodiscard]] std::expected<void, device::BlockStorageError> Sync() override;
    [[nodiscard]] std::expected<device::BlockStorageMapping, device::BlockStorageError> Map(
        std::span<const uint64_t> block_offsets, uint32_t block_size) override;
    void Unmap(device::BlockStorageMapping& mapping) override;

   private:
    [[nodiscard]] bool InRange(uint64_t offset, uint64_t size) const;
    [[nodiscard]] bool ReadSectors(size_t sector, size_t count);
    [[nodiscard]] bool WriteSectors(size_t sector, size_t count);

    static constexpr uint32_t kPreferredTransferBytes = 4096U;

    std::mutex mutex_;
    int slot_{SDMMC_HOST_SLOT_0};
    bool host_initialized_{};
    bool slot_initialized_{};
    sd_pwr_ctrl_handle_t power_{};
    sdmmc_card_t card_{};
    uint8_t* transfer_buffer_{};
    uint32_t transfer_buffer_size_{};
    uint32_t transfer_sector_capacity_{};
    uint32_t sector_size_{};
    bool present_{};
    device::BlockStorageGeometry geometry_{};
};

}  // namespace micropixel::platform::storage

#endif
