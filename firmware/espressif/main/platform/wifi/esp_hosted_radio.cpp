#include "platform/wifi/esp_hosted_radio.hpp"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_hosted_ota.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

namespace micropixel::platform::wifi {
namespace {

constexpr char kTag[] = "micropixel_radio";
constexpr size_t kOtaChunkSize = 1500U;

struct StagedImage {
    const esp_partition_t* partition{};
    size_t size{};
    char version[32]{};
};

bool SameVersion(const esp_hosted_coprocessor_fwver_t& current, const char* staged) {
    char current_text[32]{};
    const int length = std::snprintf(current_text, sizeof(current_text), "%" PRIu32 ".%" PRIu32 ".%" PRIu32,
                                     current.major1, current.minor1, current.patch1);
    return length > 0 && static_cast<size_t>(length) < sizeof(current_text) && std::strcmp(current_text, staged) == 0;
}

bool SupportsExplicitActivation(const esp_hosted_coprocessor_fwver_t& version) {
    return version.major1 > 2U || (version.major1 == 2U && version.minor1 >= 6U);
}

esp_err_t InspectStagedImage(const char* label, StagedImage& image) {
    image.partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_ANY, label);
    if (image.partition == nullptr) return ESP_ERR_NOT_FOUND;

    esp_image_header_t header{};
    esp_err_t status = esp_partition_read(image.partition, 0, &header, sizeof(header));
    if (status != ESP_OK) return status;
    if (header.magic == 0xffU) return ESP_ERR_NOT_FOUND;
    if (header.magic != ESP_IMAGE_HEADER_MAGIC || header.segment_count == 0U || header.segment_count > 16U) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = sizeof(header);
    for (uint8_t segment = 0; segment < header.segment_count; ++segment) {
        esp_image_segment_header_t segment_header{};
        if (offset > image.partition->size - sizeof(segment_header)) return ESP_ERR_INVALID_SIZE;
        status = esp_partition_read(image.partition, offset, &segment_header, sizeof(segment_header));
        if (status != ESP_OK) return status;
        offset += sizeof(segment_header);
        if (segment_header.data_len > image.partition->size - offset) return ESP_ERR_INVALID_SIZE;

        if (segment == 0U) {
            esp_app_desc_t descriptor{};
            if (segment_header.data_len < sizeof(descriptor)) return ESP_ERR_INVALID_ARG;
            status = esp_partition_read(image.partition, offset, &descriptor, sizeof(descriptor));
            if (status != ESP_OK) return status;
            if (descriptor.magic_word != ESP_APP_DESC_MAGIC_WORD || descriptor.version[0] == '\0') {
                return ESP_ERR_INVALID_ARG;
            }
            std::memcpy(image.version, descriptor.version, sizeof(image.version) - 1U);
            image.version[sizeof(image.version) - 1U] = '\0';
        }
        offset += segment_header.data_len;
    }

    // The checksum byte is part of the final 16-byte-aligned image body;
    // an optional SHA-256 digest follows that boundary.
    offset = (offset + 1U + 15U) & ~size_t{15U};
    if (header.hash_appended != 0U) {
        offset = (offset + 15U) & ~size_t{15U};
        offset += 32U;
    }
    if (offset > image.partition->size) return ESP_ERR_INVALID_SIZE;
    image.size = offset;
    return ESP_OK;
}

esp_err_t TransferStagedImage(const StagedImage& image) {
    auto* chunk = static_cast<uint8_t*>(heap_caps_malloc(kOtaChunkSize, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (chunk == nullptr) return ESP_ERR_NO_MEM;

    esp_err_t status = esp_hosted_slave_ota_begin();
    if (status != ESP_OK) {
        heap_caps_free(chunk);
        return status;
    }
    for (size_t offset = 0; status == ESP_OK && offset < image.size;) {
        const size_t amount = image.size - offset < kOtaChunkSize ? image.size - offset : kOtaChunkSize;
        status = esp_partition_read(image.partition, offset, chunk, amount);
        if (status == ESP_OK) {
            status = esp_hosted_slave_ota_write(chunk, static_cast<uint32_t>(amount));
        }
        offset += amount;
        if (status == ESP_OK && (offset == image.size || (offset / kOtaChunkSize) % 100U == 0U)) {
            ESP_LOGI(kTag, "C6 OTA progress: %u/%u bytes", static_cast<unsigned>(offset),
                     static_cast<unsigned>(image.size));
        }
    }
    const esp_err_t end_status = esp_hosted_slave_ota_end();
    heap_caps_free(chunk);
    return status == ESP_OK ? end_status : status;
}

esp_err_t UpdateCoprocessorIfNeeded(const char* partition_label) {
    StagedImage staged{};
    const esp_err_t inspect_status = InspectStagedImage(partition_label, staged);
    if (inspect_status != ESP_OK) {
        ESP_LOGW(kTag, "C6 staged firmware '%s' is unavailable: %s", partition_label,
                 esp_err_to_name(inspect_status));
        return inspect_status;
    }

    esp_hosted_coprocessor_fwver_t current{};
    const esp_err_t version_status = static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(&current));
    if (version_status == ESP_OK && SameVersion(current, staged.version)) {
        ESP_LOGI(kTag, "C6 firmware %s is current", staged.version);
        return ESP_OK;
    }

    ESP_LOGW(kTag, "updating C6 firmware to %s from staged image (%u bytes)", staged.version,
             static_cast<unsigned>(staged.size));
    const bool activate = version_status == ESP_OK && SupportsExplicitActivation(current);
    esp_err_t status = TransferStagedImage(staged);
    if (status != ESP_OK) return status;
    if (activate) {
        status = esp_hosted_slave_ota_activate();
        if (status != ESP_OK) return status;
    }

    ESP_LOGI(kTag, "C6 firmware update completed; restarting Host to resynchronize SDIO");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

void LogCoprocessorInfo() {
    esp_hosted_coprocessor_fwver_t version{};
    const esp_err_t version_status = static_cast<esp_err_t>(esp_hosted_get_coprocessor_fwversion(&version));
    if (version_status == ESP_OK) {
        ESP_LOGI(kTag, "coprocessor firmware: %" PRIu32 ".%" PRIu32 ".%" PRIu32 " rev=%" PRId32, version.major1,
                 version.minor1, version.patch1, version.revision);
    } else {
        ESP_LOGW(kTag, "could not read coprocessor firmware version: %s", esp_err_to_name(version_status));
    }

    uint32_t chip_id = 0U;
    char target[32]{};
    const esp_err_t info_status = static_cast<esp_err_t>(esp_hosted_get_cp_info(&chip_id, target, sizeof(target)));
    if (info_status == ESP_OK) {
        ESP_LOGI(kTag, "coprocessor target=%s chip_id=0x%08" PRIx32, target[0] == '\0' ? "unknown" : target, chip_id);
    } else {
        ESP_LOGW(kTag, "could not read coprocessor info: %s", esp_err_to_name(info_status));
    }
}

}  // namespace

esp_err_t EspHostedRadio::Initialize() { return static_cast<esp_err_t>(esp_hosted_init()); }

esp_err_t EspHostedRadio::OnStationStarted() {
    if (staged_firmware_partition_ != nullptr) {
        const esp_err_t ota_status = UpdateCoprocessorIfNeeded(staged_firmware_partition_);
        if (ota_status != ESP_OK) {
            ESP_LOGE(kTag, "C6 automatic firmware update failed: %s", esp_err_to_name(ota_status));
        }
    }
    LogCoprocessorInfo();
    return ESP_OK;
}

}  // namespace micropixel::platform::wifi
