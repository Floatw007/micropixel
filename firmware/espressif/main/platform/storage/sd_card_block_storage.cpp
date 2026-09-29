#include "platform/storage/sd_card_block_storage.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstring>
#include <mutex>

#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

namespace micropixel::platform::storage {
namespace {

constexpr char kTag[] = "sd_storage";

bool IsPowerOfTwo(uint32_t value) { return value != 0U && (value & (value - 1U)) == 0U; }

}  // namespace

SdCardBlockStorage::~SdCardBlockStorage() { Shutdown(); }

esp_err_t SdCardBlockStorage::Initialize(const Config& config) {
    if (present_ || host_initialized_ || slot_initialized_) {
        return ESP_ERR_INVALID_STATE;
    }
    if (config.slot < SDMMC_HOST_SLOT_0 || config.slot > SDMMC_HOST_SLOT_1 || config.width != 4 ||
        config.clock == GPIO_NUM_NC || config.command == GPIO_NUM_NC || config.data0 == GPIO_NUM_NC ||
        config.data1 == GPIO_NUM_NC || config.data2 == GPIO_NUM_NC || config.data3 == GPIO_NUM_NC ||
        config.power_ldo_channel < 0 || config.max_frequency_khz == 0U) {
        return ESP_ERR_INVALID_ARG;
    }

    const sd_pwr_ctrl_ldo_config_t power_config{
        .ldo_chan_id = config.power_ldo_channel,
    };
    esp_err_t status = sd_pwr_ctrl_new_on_chip_ldo(&power_config, &power_);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "enable SDMMC LDO%d failed: %s", config.power_ldo_channel, esp_err_to_name(status));
        return status;
    }

    status = sdmmc_host_init();
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "initialize SDMMC host failed: %s", esp_err_to_name(status));
        return status;
    }
    host_initialized_ = true;
    slot_ = config.slot;

    sdmmc_slot_config_t slot_config = SDMMC_SLOT_CONFIG_DEFAULT();
    slot_config.width = config.width;
    slot_config.clk = config.clock;
    slot_config.cmd = config.command;
    slot_config.d0 = config.data0;
    slot_config.d1 = config.data1;
    slot_config.d2 = config.data2;
    slot_config.d3 = config.data3;
    slot_config.cd = SDMMC_SLOT_NO_CD;
    slot_config.wp = SDMMC_SLOT_NO_WP;
    status = sdmmc_host_init_slot(config.slot, &slot_config);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "initialize SDMMC slot %d failed: %s", config.slot, esp_err_to_name(status));
        Shutdown();
        return status;
    }
    slot_initialized_ = true;

    sdmmc_host_t card_host = SDMMC_HOST_DEFAULT();
    card_host.slot = config.slot;
    card_host.max_freq_khz = config.max_frequency_khz;
    card_host.pwr_ctrl_handle = power_;
    status = sdmmc_card_init(&card_host, &card_);
    if (status != ESP_OK) {
        ESP_LOGW(kTag, "SD card probe failed: %s", esp_err_to_name(status));
        Shutdown();
        return status;
    }
    if (!card_.is_mem || card_.csd.capacity <= 0 || card_.csd.sector_size <= 0 ||
        !IsPowerOfTwo(static_cast<uint32_t>(card_.csd.sector_size))) {
        ESP_LOGE(kTag, "SD card reported unsupported geometry: sectors=%d bytes=%d", card_.csd.capacity,
                 card_.csd.sector_size);
        Shutdown();
        return ESP_ERR_INVALID_SIZE;
    }

    sector_size_ = static_cast<uint32_t>(card_.csd.sector_size);
    transfer_buffer_size_ = std::max(kPreferredTransferBytes, sector_size_);
    transfer_buffer_size_ -= transfer_buffer_size_ % sector_size_;
    transfer_sector_capacity_ = transfer_buffer_size_ / sector_size_;
    transfer_buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(transfer_buffer_size_, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (transfer_buffer_ == nullptr) {
        ESP_LOGE(kTag, "SD transfer buffer allocation failed: %" PRIu32 " bytes", transfer_buffer_size_);
        Shutdown();
        return ESP_ERR_NO_MEM;
    }

    geometry_ = device::BlockStorageGeometry{
        .size_bytes = static_cast<uint64_t>(card_.csd.capacity) * sector_size_,
        .erase_size = sector_size_,
        .program_size = 1U,
        .mappable = false,
        .map_alignment = 0U,
    };
    present_ = true;
    ESP_LOGI(kTag, "SD card ready: %.6s, %" PRIu64 " MiB, sectors=%d x %" PRIu32 " B, SDMMC=%d kHz width=%d",
             card_.cid.name, geometry_.size_bytes / (1024U * 1024U), card_.csd.capacity, sector_size_,
             card_.real_freq_khz, config.width);
    return ESP_OK;
}

void SdCardBlockStorage::Shutdown() {
    present_ = false;
    geometry_ = {};
    sector_size_ = 0U;
    transfer_sector_capacity_ = 0U;
    transfer_buffer_size_ = 0U;
    heap_caps_free(transfer_buffer_);
    transfer_buffer_ = nullptr;
    if (slot_initialized_) {
        (void)sdmmc_host_deinit_slot(slot_);
        slot_initialized_ = false;
    }
    // ESP-Hosted uses the same controller through SDMMC slot 1. Keep the
    // controller alive even if slot 0 probing fails or shuts down.
    host_initialized_ = false;
    if (power_ != nullptr) {
        (void)sd_pwr_ctrl_del_on_chip_ldo(power_);
        power_ = nullptr;
    }
    card_ = {};
}

bool SdCardBlockStorage::InRange(uint64_t offset, uint64_t size) const {
    return present_ && offset <= geometry_.size_bytes && size <= geometry_.size_bytes - offset;
}

bool SdCardBlockStorage::ReadSectors(size_t sector, size_t count) {
    const esp_err_t status = sdmmc_read_sectors(&card_, transfer_buffer_, sector, count);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "SD read failed: sector=%zu count=%zu error=%s", sector, count, esp_err_to_name(status));
        return false;
    }
    return true;
}

bool SdCardBlockStorage::WriteSectors(size_t sector, size_t count) {
    const esp_err_t status = sdmmc_write_sectors(&card_, transfer_buffer_, sector, count);
    if (status != ESP_OK) {
        ESP_LOGE(kTag, "SD write failed: sector=%zu count=%zu error=%s", sector, count, esp_err_to_name(status));
        return false;
    }
    return true;
}

std::expected<void, device::BlockStorageError> SdCardBlockStorage::Read(uint64_t offset,
                                                                        std::span<uint8_t> destination) {
    if (!InRange(offset, destination.size())) {
        return std::unexpected(present_ ? device::BlockStorageError::kInvalidArgument
                                        : device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    size_t consumed = 0U;
    while (consumed < destination.size()) {
        const uint64_t current = offset + consumed;
        const size_t sector = static_cast<size_t>(current / sector_size_);
        const uint32_t within = static_cast<uint32_t>(current % sector_size_);
        const size_t remaining = destination.size() - consumed;
        size_t sectors = 1U;
        if (within == 0U && remaining >= sector_size_) {
            sectors = std::min<size_t>(remaining / sector_size_, transfer_sector_capacity_);
        }
        if (!ReadSectors(sector, sectors)) {
            return std::unexpected(device::BlockStorageError::kIo);
        }
        const size_t available = sectors * sector_size_ - within;
        const size_t chunk = std::min(remaining, available);
        std::memcpy(destination.data() + consumed, transfer_buffer_ + within, chunk);
        consumed += chunk;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SdCardBlockStorage::Program(uint64_t offset,
                                                                           std::span<const uint8_t> source) {
    if (!InRange(offset, source.size())) {
        return std::unexpected(present_ ? device::BlockStorageError::kInvalidArgument
                                        : device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    size_t consumed = 0U;
    while (consumed < source.size()) {
        const uint64_t current = offset + consumed;
        const size_t sector = static_cast<size_t>(current / sector_size_);
        const uint32_t within = static_cast<uint32_t>(current % sector_size_);
        const size_t remaining = source.size() - consumed;
        if (within == 0U && remaining >= sector_size_) {
            const size_t sectors = std::min<size_t>(remaining / sector_size_, transfer_sector_capacity_);
            const size_t chunk = sectors * sector_size_;
            std::memcpy(transfer_buffer_, source.data() + consumed, chunk);
            if (!WriteSectors(sector, sectors)) {
                return std::unexpected(device::BlockStorageError::kIo);
            }
            consumed += chunk;
            continue;
        }
        if (!ReadSectors(sector, 1U)) {
            return std::unexpected(device::BlockStorageError::kIo);
        }
        const size_t chunk = std::min<size_t>(remaining, sector_size_ - within);
        std::memcpy(transfer_buffer_ + within, source.data() + consumed, chunk);
        if (!WriteSectors(sector, 1U)) {
            return std::unexpected(device::BlockStorageError::kIo);
        }
        consumed += chunk;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SdCardBlockStorage::Erase(uint64_t offset, uint64_t size) {
    if (!InRange(offset, size) || (offset % sector_size_) != 0U || (size % sector_size_) != 0U) {
        return std::unexpected(present_ ? device::BlockStorageError::kInvalidArgument
                                        : device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    std::memset(transfer_buffer_, 0xff, transfer_buffer_size_);
    size_t sector = static_cast<size_t>(offset / sector_size_);
    uint64_t remaining = size / sector_size_;
    while (remaining != 0U) {
        const size_t count = static_cast<size_t>(std::min<uint64_t>(remaining, transfer_sector_capacity_));
        if (!WriteSectors(sector, count)) {
            return std::unexpected(device::BlockStorageError::kIo);
        }
        sector += count;
        remaining -= count;
    }
    return {};
}

std::expected<void, device::BlockStorageError> SdCardBlockStorage::Sync() {
    if (!present_) {
        return std::unexpected(device::BlockStorageError::kUnavailable);
    }
    const std::lock_guard lock(mutex_);
    if (const esp_err_t status = sdmmc_get_status(&card_); status != ESP_OK) {
        ESP_LOGE(kTag, "SD sync/status failed: %s", esp_err_to_name(status));
        return std::unexpected(device::BlockStorageError::kIo);
    }
    return {};
}

std::expected<device::BlockStorageMapping, device::BlockStorageError> SdCardBlockStorage::Map(
    std::span<const uint64_t> block_offsets, uint32_t block_size) {
    (void)block_offsets;
    (void)block_size;
    return std::unexpected(device::BlockStorageError::kUnsupported);
}

void SdCardBlockStorage::Unmap(device::BlockStorageMapping& mapping) { mapping = {}; }

}  // namespace micropixel::platform::storage
