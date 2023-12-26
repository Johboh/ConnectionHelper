#include "OtaHelper.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
#include <spi_flash_mmap.h>
#endif

#define ENCRYPTED_BLOCK_SIZE 16
#define SPI_SECTORS_PER_BLOCK 16 // usually large erase block is 32k/64k
#define SPI_FLASH_BLOCK_SIZE (SPI_SECTORS_PER_BLOCK * SPI_FLASH_SEC_SIZE)

OtaHelper::OtaHelper(const char *id, uint16_t port) : _id(id), _port(port) {}

bool OtaHelper::start() {
  auto *partition = esp_ota_get_next_update_partition(NULL);
  if (partition == NULL) {
    ESP_LOGE(OtaHelperLog::TAG, "No OTA partition found");
    return false;
  }

  return startWebserver();
}

esp_err_t OtaHelper::httpGetHandler(httpd_req_t *req) {
  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  extern const unsigned char html_start[] asm("_binary_ota_html_start");
  extern const unsigned char html_end[] asm("_binary_ota_html_end");
  httpd_resp_send(req, (const char *)html_start, html_end - html_start);
  return ESP_OK;
}
esp_err_t OtaHelper::httpPostHandler(httpd_req_t *req) {
  OtaHelper *_this = (OtaHelper *)req->user_ctx;

  httpd_resp_set_status(req, HTTPD_500); // Assume failure, change later on success.

  auto *partition = esp_ota_get_next_update_partition(NULL);
  if (partition == NULL) {
    ESP_LOGE(OtaHelperLog::TAG, "No OTA partition found");
    httpd_resp_send(req, "No OTA partition found", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  ESP_LOGI(OtaHelperLog::TAG, "OTA started via HTTP with target partition: %s", partition->label);
  if (!_this->writeStreamToPartition(partition, req)) {
    ESP_LOGE(OtaHelperLog::TAG, "Failed to write stream to partition");
    httpd_resp_send(req, "Failed to write stream to partition", HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
  }

  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_send(req, NULL, 0);
  vTaskDelay(2000 / portTICK_RATE_MS);
  esp_restart();
  return ESP_OK;
}

int OtaHelper::fillBuffer(httpd_req_t *req, char *buffer, size_t buffer_size) {
  int total_read = 0;
  while (total_read < buffer_size) {
    int read = httpd_req_recv(req, buffer + total_read, buffer_size - total_read);
    if (read <= 0) {
      if (read == HTTPD_SOCK_ERR_TIMEOUT || read == HTTPD_SOCK_ERR_FAIL) {
        ESP_LOGE(OtaHelperLog::TAG, "Failed to fill buffer, read zero and not complete.");
        return -1;
      } else {
        return total_read;
      }
    }
    total_read += read;
  }
  return total_read;
}

bool OtaHelper::writeStreamToPartition(const esp_partition_t *partition, httpd_req_t *req) {
  char *buffer = (char *)malloc(SPI_FLASH_SEC_SIZE);
  if (buffer == nullptr) {
    ESP_LOGE(OtaHelperLog::TAG, "Failed to allocate buffer of size %d", SPI_FLASH_SEC_SIZE);
    return false;
  }

  uint8_t skip_buffer[ENCRYPTED_BLOCK_SIZE];

  int bytes_read = 0;
  while (bytes_read < req->content_len) {
    int bytes_filled = fillBuffer(req, buffer, SPI_FLASH_SEC_SIZE);
    if (bytes_filled < 0) {
      ESP_LOGE(OtaHelperLog::TAG, "Unable to fill buffer");
      free(buffer);
      return false;
    }

    ESP_LOGV(OtaHelperLog::TAG, "Filled buffer with: %d", bytes_filled);

    // Special start case
    // Check start if contains the magic byte.
    uint8_t skip = 0;
    if (bytes_read == 0) {
      if (buffer[0] != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(OtaHelperLog::TAG, "Start of firwmare does not contain magic byte");
        free(buffer);
        return false;
      }

      // Stash the first 16/ENCRYPTED_BLOCK_SIZE bytes of data and set the offset so they are
      // not written at this point so that partially written firmware
      // will not be bootable
      memcpy(skip_buffer, buffer, sizeof(skip_buffer));
      skip += sizeof(skip_buffer);
    }

    // Normal case - write buffer
    if (!writeBufferToPartition(partition, bytes_read, buffer, bytes_filled, skip)) {
      ESP_LOGE(OtaHelperLog::TAG, "Failed to write buffer to partition");
      free(buffer);
      return false;
    }

    // If this is the end, finish up.
    if (bytes_filled != SPI_FLASH_SEC_SIZE) {
      ESP_LOGI(OtaHelperLog::TAG, "End of buffer");

      auto r = esp_partition_write(partition, 0, (uint32_t *)skip_buffer, ENCRYPTED_BLOCK_SIZE);
      if (!reportOnError(r, "Failed to enable partition")) {
        free(buffer);
        return false;
      }

      r = partitionIsBootable(partition);
      if (!reportOnError(r, "Partition is not bootable")) {
        free(buffer);
        return false;
      }

      r = esp_ota_set_boot_partition(partition);
      if (!reportOnError(r, "Failed to set partition as bootable")) {
        free(buffer);
        return false;
      }
    }

    bytes_read += bytes_filled;
    vTaskDelay(0); // Yield/reschedule
  }

  free(buffer);
  return true;
}

bool OtaHelper::writeBufferToPartition(const esp_partition_t *partition, size_t bytes_written, char *buffer,
                                       size_t buffer_size, uint8_t skip) {

  size_t offset = partition->address + bytes_written;
  bool block_erase =
      (buffer_size - bytes_written >= SPI_FLASH_BLOCK_SIZE) &&
      (offset % SPI_FLASH_BLOCK_SIZE == 0); // if it's the block boundary, than erase the whole block from here
  bool part_head_sectors = partition->address % SPI_FLASH_BLOCK_SIZE &&
                           offset < (partition->address / SPI_FLASH_BLOCK_SIZE + 1) *
                                        SPI_FLASH_BLOCK_SIZE; // sector belong to unaligned partition heading block
  bool part_tail_sectors = offset >= (partition->address + buffer_size) / SPI_FLASH_BLOCK_SIZE *
                                         SPI_FLASH_BLOCK_SIZE; // sector belong to unaligned partition tailing block
  if (block_erase || part_head_sectors || part_tail_sectors) {
    esp_err_t r =
        esp_partition_erase_range(partition, bytes_written, block_erase ? SPI_FLASH_BLOCK_SIZE : SPI_FLASH_SEC_SIZE);
    if (!reportOnError(r, "Failed to erase range")) {
      return false;
    }
  }

  // try to skip empty blocks on unecrypted partitions
  if (partition->encrypted || checkDataInBlock((uint8_t *)buffer + skip / sizeof(uint32_t), bytes_written - skip)) {
    auto r = esp_partition_write(partition, bytes_written + skip, (uint32_t *)buffer + skip / sizeof(uint32_t),
                                 buffer_size - skip);
    if (!reportOnError(r, "Failed to write range")) {
      return false;
    }
  }

  return true;
}

esp_err_t OtaHelper::partitionIsBootable(const esp_partition_t *partition) {
  uint8_t buf[ENCRYPTED_BLOCK_SIZE];
  if (!partition) {
    return ESP_ERR_INVALID_ARG;
  }

  esp_err_t r = esp_partition_read(partition, 0, (uint32_t *)buf, ENCRYPTED_BLOCK_SIZE);
  if (r != ESP_OK) {
    return r;
  }

  if (buf[0] != ESP_IMAGE_HEADER_MAGIC) {
    return ESP_ERR_INVALID_CRC;
  }
  return ESP_OK;
}

bool OtaHelper::checkDataInBlock(const uint8_t *data, size_t len) {
  // check 32-bit aligned blocks only
  if (!len || len % sizeof(uint32_t))
    return true;

  size_t dwl = len / sizeof(uint32_t);

  do {
    if (*(uint32_t *)data ^ 0xffffffff) // for SPI NOR flash empty blocks are all one's, i.e. filled with 0xff byte
      return true;

    data += sizeof(uint32_t);
  } while (--dwl);
  return false;
}

bool OtaHelper::startWebserver() {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;

  if (!reportOnError(httpd_start(&server, &config), "failed to start httpd")) {
    return false;
  }

  const httpd_uri_t ota_post = {
      .uri = "/",
      .method = HTTP_POST,
      .handler = httpPostHandler,
      .user_ctx = this,
  };
  if (!reportOnError(httpd_register_uri_handler(server, &ota_post), "failed to register uri handler for OTA post")) {
    return false;
  }

  httpd_uri_t ota_root = {
      .uri = "/",
      .method = HTTP_GET,
      .handler = httpGetHandler,
      .user_ctx = this,
  };
  if (!reportOnError(httpd_register_uri_handler(server, &ota_root), "failed to register uri handler for OTA root")) {
    return false;
  }

  return true;
}

bool OtaHelper::reportOnError(esp_err_t err, const char *msg) {
  if (err != ESP_OK) {
    ESP_LOGE(OtaHelperLog::TAG, "%s: %s", msg, esp_err_to_name(err));
    return false;
  }
  return true;
}