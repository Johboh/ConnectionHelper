#include "OtaHelper.h"
#include "MD5Builder.h"
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_tls_crypto.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
#include <spi_flash_mmap.h>
#endif

// Arduino OTA specific
#define UDP_CMD_WRITE_FIRMWARE 0
#define UDP_CMD_WRITE_SPIFFS 100
#define UDP_CMD_AUTH 200
#define ESPOTA_SUCCESSFUL "OK"

// Web OTA/HTTP local OTA specific
#define AUTHORIZATION_HDR_KEY "Authorization"
#define FLASH_MODE_HDR_KEY "X-Flash-Mode"
#define FLASH_MODE_FIRMWARE_STR "firmware"
#define FLASH_MODE_SPIFFS_STR "spiffs"
#define HTTPD_401 "401 UNAUTHORIZED"

// HTTP remote OTA specifc
#define HTTP_REMOTE_TIMEOUT_MS 15000

// generic partition
#define ENCRYPTED_BLOCK_SIZE 16
#define SPI_SECTORS_PER_BLOCK 16 // usually large erase block is 32k/64k
#define SPI_FLASH_BLOCK_SIZE (SPI_SECTORS_PER_BLOCK * SPI_FLASH_SEC_SIZE)

// Rollback related
#define ARDINO_OTA_STARTED_BIT BIT0
#define WEB_OTA_STARTED_BIT BIT1

// #########################################################################
// Public API
// #########################################################################

OtaHelper::OtaHelper(Configuration configuration, CrtBundleAttach crt_bundle_attach)
    : _configuration(configuration), _crt_bundle_attach(crt_bundle_attach) {
  _rollback_event_group = xEventGroupCreate();
}

bool OtaHelper::start() {

  _rollback_bits_to_wait_for = 0;
  xEventGroupClearBits(_rollback_event_group, 0xFF);

  // Username cleanup
  _configuration.web_ota.credentials.username = trim(_configuration.web_ota.credentials.username);
  _configuration.arduino_ota.credentials.username = trim(_configuration.arduino_ota.credentials.username);

  ESP_LOGI(OtaHelperLog::TAG, "Starting OtaHelper with the following configuration");
  ESP_LOGI(OtaHelperLog::TAG, "  - Rollback Strategy: %s",
           _configuration.rollback_strategy == RollbackStrategy::AUTO ? "AUTO" : "MANUAL");

  ESP_LOGI(OtaHelperLog::TAG, "  - Web UI: %s", _configuration.web_ota.enabled ? "enabled" : "disabled");
  if (_configuration.web_ota.enabled) {
    ESP_LOGI(OtaHelperLog::TAG, "    - http port : %d", _configuration.web_ota.http_port);
    ESP_LOGI(OtaHelperLog::TAG, "    - id : %s", _configuration.web_ota.id.c_str());
    auto username = _configuration.web_ota.credentials.username;
    if (!username.empty()) {
      ESP_LOGI(OtaHelperLog::TAG, "    - username: %s", _configuration.web_ota.credentials.username.c_str());
    }
    _rollback_bits_to_wait_for += WEB_OTA_STARTED_BIT;
  }

  ESP_LOGI(OtaHelperLog::TAG, "  - Arduino OTA: %s", _configuration.arduino_ota.enabled ? "enabled" : "disabled");
  if (_configuration.arduino_ota.enabled) {
    ESP_LOGI(OtaHelperLog::TAG, "    - udp listenting port : %d", _configuration.arduino_ota.udp_listenting_port);
    auto username = _configuration.arduino_ota.credentials.username;
    if (!username.empty()) {
      ESP_LOGI(OtaHelperLog::TAG, "    - username: %s", _configuration.arduino_ota.credentials.username.c_str());
    }

    _rollback_bits_to_wait_for += ARDINO_OTA_STARTED_BIT;
  }

  ESP_LOGI(OtaHelperLog::TAG, "  - Remote URI download: enabled (always)");

  if (_configuration.rollback_strategy == RollbackStrategy::AUTO) {
    auto can_rollback = esp_ota_check_rollback_is_possible();
    if (can_rollback) {
      xTaskCreate(rollbackWatcherTask, "rollback", 4096, this, 5, NULL);
    } else {
      ESP_LOGI(OtaHelperLog::TAG, "Not starting rollback watcher (either this is the only app, or other error)");
    }
  }

  if (_configuration.arduino_ota.enabled) {
    xTaskCreate(arduinoOtaUdpServerTask, "arduino_udp", 4096, this, 5, NULL);
  }

  if (_configuration.web_ota.enabled) {
    return startWebserver();
  } else {
    return true;
  }
}

void OtaHelper::cancelRollback() {
  if (!esp_ota_check_rollback_is_possible()) {
    ESP_LOGW(OtaHelperLog::TAG,
             "Rollback is not possible so no rollback to cancel (either this is the only app, or other error)");
  } else {
    ESP_LOGI(OtaHelperLog::TAG, "Canceling rollback and accepting the new firmware (if firmware where written)");
    esp_ota_mark_app_valid_cancel_rollback();
  }
}

bool OtaHelper::updateFrom(std::string &url, FlashMode flash_mode, std::string md5_hash) {
  auto *partition = findPartition(flash_mode);
  if (partition == nullptr) {
    ESP_LOGE(OtaHelperLog::TAG, "Unable to find partition suitable partition");
    return ESP_FAIL;
  }

  if (!md5_hash.empty() && md5_hash.length() != 32) {
    ESP_LOGE(OtaHelperLog::TAG, "MD5 is not correct length. Expected length: 32, got %zu", md5_hash.length());
    return false;
  }

  ESP_LOGI(OtaHelperLog::TAG, "OTA started via remoteHTTP with target partition: %s", partition->label);

  return downloadAndWriteToPartition(partition, flash_mode, url, md5_hash);
}

// #########################################################################
// OTA via local HTTP webserver / web UI
// #########################################################################

void OtaHelper::setNotAuthenticatedResonse(httpd_req_t *req) {
  httpd_resp_set_status(req, HTTPD_401);
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"OtaHelper\"");
  httpd_resp_send(req, "Not authenticated", HTTPD_RESP_USE_STRLEN);
}

bool OtaHelper::handleAuthentication(httpd_req_t *req) {
  if (_configuration.web_ota.credentials.username.empty()) {
    return true; // early return, nothing to authenticate.
  }

  size_t authorization_len = httpd_req_get_hdr_value_len(req, AUTHORIZATION_HDR_KEY) + 1;
  if (authorization_len > 1) {
    char *authorization = (char *)malloc(authorization_len);
    esp_err_t err = httpd_req_get_hdr_value_str(req, AUTHORIZATION_HDR_KEY, authorization, authorization_len);
    if (err != ESP_OK) {
      ESP_LOGE(OtaHelperLog::TAG, "Unable to get authorization header: %s", esp_err_to_name(err));
      setNotAuthenticatedResonse(req);
      return false;
    }

    std::string user_info =
        _configuration.web_ota.credentials.username + ":" + _configuration.web_ota.credentials.password;
    size_t crypto_buffer_size = 0;
    esp_crypto_base64_encode(NULL, 0, &crypto_buffer_size, (const unsigned char *)user_info.c_str(),
                             user_info.length());
    char buff[crypto_buffer_size];
    esp_crypto_base64_encode((unsigned char *)buff, crypto_buffer_size, &crypto_buffer_size,
                             (const unsigned char *)user_info.c_str(), user_info.length());
    std::string expected_authorization = "Basic " + std::string(buff);

    if (expected_authorization != std::string(authorization)) {
      ESP_LOGW(OtaHelperLog::TAG, "Credentials does not match");
      setNotAuthenticatedResonse(req);
      return false;
    }

  } else {
    ESP_LOGI(OtaHelperLog::TAG, "No credentials provided");
    setNotAuthenticatedResonse(req);
    return false;
  }

  return true; // All good.
}

esp_err_t OtaHelper::httpGetHandler(httpd_req_t *req) {
  OtaHelper *_this = (OtaHelper *)req->user_ctx;

  if (!_this->handleAuthentication(req)) {
    return ESP_FAIL;
  }

  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_set_hdr(req, "Connection", "keep-alive");
  extern const unsigned char html_start[] asm("_binary_ota_html_start");
  extern const unsigned char html_end[] asm("_binary_ota_html_end");
  const size_t html_size = (html_end - html_start);

  std::string html;
  html.assign((char *)html_start, html_size);
  _this->replaceAll(html, "$id", _this->_configuration.web_ota.id);

  httpd_resp_send(req, html.c_str(), HTTPD_RESP_USE_STRLEN);
  return ESP_OK;
}
esp_err_t OtaHelper::httpPostHandler(httpd_req_t *req) {
  OtaHelper *_this = (OtaHelper *)req->user_ctx;

  if (!_this->handleAuthentication(req)) {
    return ESP_FAIL;
  }

  httpd_resp_set_status(req, HTTPD_500); // Assume failure, change later on success.
  httpd_resp_set_hdr(req, "Connection", "keep-alive");

  // Firmware or spiffs
  char hdr_value[255] = {0};
  esp_err_t err = httpd_req_get_hdr_value_str(req, FLASH_MODE_HDR_KEY, hdr_value, 255);
  if (err != ESP_OK) {
    ESP_LOGE(OtaHelperLog::TAG, "Unable to get flash mode (firmware or spiffs): %s", esp_err_to_name(err));
    httpd_resp_send(req, "Unable to get flash mode (firmware or spiffs)", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  FlashMode flash_mode;
  if (strcmp(hdr_value, FLASH_MODE_FIRMWARE_STR) == 0) {
    flash_mode = FlashMode::FIRMWARE;
  } else if (strcmp(hdr_value, FLASH_MODE_SPIFFS_STR) == 0) {
    flash_mode = FlashMode::SPIFFS;
  } else {
    ESP_LOGE(OtaHelperLog::TAG, "Invalid flash mode: %s", hdr_value);
    httpd_resp_send(req, "Invalid flash mode", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  const esp_partition_t *partition = _this->findPartition(flash_mode);
  if (partition == nullptr) {
    ESP_LOGE(OtaHelperLog::TAG, "Unable to find partition suitable partition");
    httpd_resp_send(req, "Unable to find suitable partition", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }
  ESP_LOGI(OtaHelperLog::TAG, "OTA started via HTTP with target partition: %s", partition->label);

  std::string md5hash = ""; // No hash
  if (!_this->writeStreamToPartition(partition, flash_mode, req->content_len, md5hash,
                                     [&_this, req](char *buffer, size_t buffer_size, size_t total_bytes_left) {
                                       return _this->fillBuffer(req, buffer, buffer_size);
                                     })) {
    ESP_LOGE(OtaHelperLog::TAG, "Failed to write stream to partition");
    httpd_resp_send(req, "Failed to write stream to partition", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  ESP_LOGI(OtaHelperLog::TAG, "HTTP OTA complete, rebooting...");

  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_send(req, NULL, 0);
  vTaskDelay(2000 / portTICK_RATE_MS);
  esp_restart();
  return ESP_OK;
}

bool OtaHelper::startWebserver() {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = _configuration.web_ota.http_port;
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

  xEventGroupSetBits(_rollback_event_group, WEB_OTA_STARTED_BIT);
  return true;
}

/**
 * @brief Fill buffer with data from HTTP local webserver
 */
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

// #########################################################################
// OTA via remote URI
// #########################################################################

esp_err_t OtaHelper::httpEventHandler(esp_http_client_event_t *evt) {

  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    ESP_LOGE(OtaHelperLog::TAG, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    ESP_LOGI(OtaHelperLog::TAG, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    ESP_LOGV(OtaHelperLog::TAG, "HTTP_EVENT_HEADER_SENT");
    break;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
  case HTTP_EVENT_REDIRECT:
    ESP_LOGV(OtaHelperLog::TAG, "HTTP_EVENT_REDIRECT");
    break;
#endif
  case HTTP_EVENT_ON_HEADER:
    ESP_LOGV(OtaHelperLog::TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
    break;
  case HTTP_EVENT_ON_DATA:
    ESP_LOGV(OtaHelperLog::TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
    break;
  case HTTP_EVENT_ON_FINISH:
    ESP_LOGI(OtaHelperLog::TAG, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    ESP_LOGI(OtaHelperLog::TAG, "HTTP_EVENT_DISCONNECTED");
    break;
  }

  return ESP_OK;
}

bool OtaHelper::downloadAndWriteToPartition(const esp_partition_t *partition, FlashMode flash_mode, std::string &url,
                                            std::string &md5hash) {

  char *buffer = (char *)malloc(SPI_FLASH_SEC_SIZE);

  esp_http_client_config_t config = {};
  config.url = url.c_str();
  config.user_data = this;
  config.event_handler = httpEventHandler;
  config.buffer_size = SPI_FLASH_SEC_SIZE;
  if (_crt_bundle_attach) {
    config.crt_bundle_attach = _crt_bundle_attach;
    ESP_LOGI(OtaHelperLog::TAG, "With TLS/HTTPS support");
  } else {
    ESP_LOGI(OtaHelperLog::TAG, "Without TLS/HTTPS support");
  }
  esp_http_client_handle_t client = esp_http_client_init(&config);

  ESP_LOGI(OtaHelperLog::TAG, "Using URL %s", url.c_str());
  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_http_client_set_header(client, "Accept", "*/*");
  esp_http_client_set_timeout_ms(client, HTTP_REMOTE_TIMEOUT_MS);

  bool success = false;
  esp_err_t r = esp_http_client_open(client, 0);
  if (r == ESP_OK) {
    esp_http_client_fetch_headers(client);
    auto status_code = esp_http_client_get_status_code(client);
    auto content_length = esp_http_client_get_content_length(client);
    ESP_LOGI(OtaHelperLog::TAG, "HTTP status code: %d, content length: %d", status_code, content_length);

    if (status_code == 200) {
      if (content_length > partition->size) {
        ESP_LOGE(OtaHelperLog::TAG, "Content length %d is larger than partition size %d", content_length,
                 partition->size);
      } else {
        success = writeStreamToPartition(partition, flash_mode, content_length, md5hash,
                                         [&](char *buffer, size_t buffer_size, size_t total_bytes_left) {
                                           return fillBuffer(client, buffer, buffer_size);
                                         });
      }
    } else {
      ESP_LOGE(OtaHelperLog::TAG, "Got non 200 status code: %d", status_code);
    }

  } else {
    const char *errstr = esp_err_to_name(r);
    ESP_LOGE(OtaHelperLog::TAG, "Failed to open HTTP connection: %s", errstr);
  }

  esp_http_client_close(client);
  esp_http_client_cleanup(client);

  if (buffer != nullptr) {
    free(buffer);
  }
  return success;
}

/**
 * @brief Fill buffer with data from URI/remote HTTP server.
 */
int OtaHelper::fillBuffer(esp_http_client_handle_t client, char *buffer, size_t buffer_size) {
  int total_read = 0;
  while (total_read < buffer_size) {
    int read = esp_http_client_read(client, buffer + total_read, buffer_size - total_read);
    if (read <= 0) {
      if (esp_http_client_is_complete_data_received(client)) {
        return total_read;
      } else {
        ESP_LOGE(OtaHelperLog::TAG, "Failed to fill buffer, read zero and not complete.");
        return -1;
      }
    }
    total_read += read;
  }
  return total_read;
}

// #########################################################################
// OTA via ArduinoOTA
// #########################################################################

void OtaHelper::arduinoOtaUdpServerTask(void *pvParameters) {
  OtaHelper *_this = (OtaHelper *)pvParameters;

  char rx_buffer[512];
  char addr_str[128];
  auto port = _this->_configuration.arduino_ota.udp_listenting_port;

  while (1) {

    struct sockaddr_in dest_addr_ip4;
    dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4.sin_family = AF_INET;
    dest_addr_ip4.sin_port = htons(port);
    int ip_protocol = IPPROTO_IP;

    int sock = socket(AF_INET, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
      ESP_LOGE(OtaHelperLog::TAG, "Unable to create UDP socket: errno %d", errno);
      break;
    }
    ESP_LOGI(OtaHelperLog::TAG, "UDP socket created");

    int err = bind(sock, (struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
    if (err < 0) {
      ESP_LOGE(OtaHelperLog::TAG, "UDP socket unable to bind: errno %d", errno);
      break;
    }
    ESP_LOGI(OtaHelperLog::TAG, "UDP socket bound, port %d", port);
    xEventGroupSetBits(_this->_rollback_event_group, ARDINO_OTA_STARTED_BIT);

    struct sockaddr_storage source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (1) {
      ESP_LOGI(OtaHelperLog::TAG, "waiting UDP packet...");
      int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
      ESP_LOGI(OtaHelperLog::TAG, "Got UDP packet with length %d", len);

      // Error occurred during receiving?
      if (len < 0) {
        ESP_LOGE(OtaHelperLog::TAG, "UDP recvfrom failed: errno %d", errno);
        break;
      } else {
        // Parse packet.
        auto udp_packet = _this->parseUdpPacket(rx_buffer, len);
        if (!udp_packet) {
          ESP_LOGE(OtaHelperLog::TAG, "Failed to parse UDP packet");
          break;
        }

        // Send response
        // Get the sender's ip address as string
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
        std::string ok = "OK";
        int err = sendto(sock, ok.c_str(), ok.size(), 0, (struct sockaddr *)&source_addr, sizeof(source_addr));
        if (err < 0) {
          ESP_LOGE(OtaHelperLog::TAG, "error occurred during sending UDP: errno %d", errno);
          break;
        }

        // Handle OTA.
        if (udp_packet) {
          _this->connectToHostForArduino(*udp_packet, addr_str);
        }
      }
    }

    if (sock != -1) {
      ESP_LOGE(OtaHelperLog::TAG, "Shutting down UDP socket and restarting...");
      shutdown(sock, 0);
      close(sock);
    }
  }
  vTaskDelete(NULL);
}

/**
 * @brief Parses the buffer received from the Arduino upload server.
 * The buffer contains the received packet.
 * The packet first contains an integer as a string (this is the flash_mode, uint8_t), followed by a space,
 * then another integer as a string (this is the size of the host port number, uint16_t), followed by a space,
 * then another integer as a string (this is the size of the firmware, uint16_t), followed by a space,
 * then a MD5 string in hex (32 characters) which ends with a new line.
 *
 * @param buffer the input buffer.
 * @param buffer_size the length of the buffer.
 */
std::optional<OtaHelper::ArduinoOtaUpdate> OtaHelper::parseUdpPacket(char *buffer, size_t buffer_size) {
  ArduinoOtaUpdate update;

  // Tokenize the buffer using strtok function
  char *token = strtok(buffer, " ");
  if (token == nullptr) {
    return std::nullopt;
  }

  // Parse flash_mode integer
  uint8_t flash_mode = static_cast<uint8_t>(std::atoi(token));
  if (flash_mode == UDP_CMD_WRITE_FIRMWARE) {
    update.flash_mode = FlashMode::FIRMWARE;
  } else if (flash_mode == UDP_CMD_WRITE_SPIFFS) {
    update.flash_mode = FlashMode::SPIFFS;
  } else {
    return std::nullopt;
  }

  // Parse host port number size
  token = strtok(nullptr, " ");
  if (token == nullptr) {
    return std::nullopt;
  }
  update.host_port = static_cast<uint16_t>(std::atoi(token));

  // Parse firmware size
  token = strtok(nullptr, " ");
  if (token == nullptr) {
    return std::nullopt;
  }
  update.size = static_cast<uint32_t>(std::atoi(token));

  // Parse MD5 string
  token = strtok(nullptr, "\n");
  if (token == nullptr) {
    return std::nullopt;
  }
  update.md5 = token;

  return update;
}

bool OtaHelper::connectToHostForArduino(ArduinoOtaUpdate &update, char *host_ip) {
  ESP_LOGV(OtaHelperLog::TAG, "Connecting to host %s", host_ip);
  ESP_LOGV(OtaHelperLog::TAG, "host_port: %d", update.host_port);
  ESP_LOGV(OtaHelperLog::TAG, "flash_mode: %d", update.flash_mode);
  ESP_LOGV(OtaHelperLog::TAG, "size: %d", update.size);
  ESP_LOGV(OtaHelperLog::TAG, "md5: %s", update.md5.c_str());

  const esp_partition_t *partition = findPartition(update.flash_mode);
  if (partition == nullptr) {
    ESP_LOGE(OtaHelperLog::TAG, "Unable to find suitable partition");
    return ESP_FAIL;
  }
  ESP_LOGI(OtaHelperLog::TAG, "OTA started via TCP with target partition: %s", partition->label);

  struct sockaddr_in dest_addr;
  inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(update.host_port);

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock < 0) {
    ESP_LOGE(OtaHelperLog::TAG, "Unable to create TCP client socket: errno %d", errno);
    return false;
  }
  ESP_LOGI(OtaHelperLog::TAG, "TCP client socket created, connecting to %s:%d", host_ip, update.host_port);

  int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0) {
    ESP_LOGE(OtaHelperLog::TAG, "TCP client socket unable to connect: errno %d", errno);
    shutdown(sock, 0);
    close(sock);
    return false;
  }
  ESP_LOGI(OtaHelperLog::TAG, "Successfully connected to host");

  auto ok = writeStreamToPartition(partition, update.flash_mode, update.size, update.md5,
                                   [&](char *buffer, size_t buffer_size, size_t total_bytes_left) {
                                     return fillBuffer(sock, buffer, buffer_size, total_bytes_left);
                                   });
  if (!ok) {
    ESP_LOGE(OtaHelperLog::TAG, "Failed to write stream to partition");
    shutdown(sock, 0);
    close(sock);
    return false;
  }

  ESP_LOGI(OtaHelperLog::TAG, "TCP OTA complete, rebooting...");

  err = send(sock, ESPOTA_SUCCESSFUL, strlen(ESPOTA_SUCCESSFUL), 0);
  if (err < 0) {
    ESP_LOGE(OtaHelperLog::TAG, "Failed to ack TCP update, its fine.");
  }
  return true;
}

/**
 * @brief Fill buffer with data from socket
 */
int OtaHelper::fillBuffer(int socket, char *buffer, size_t buffer_size, size_t total_bytes_left) {
  int total_read = 0;
  while (total_read < buffer_size) {
    int read = recv(socket, buffer + total_read, buffer_size - total_read, 0);
    if (read < 0) {
      ESP_LOGE(OtaHelperLog::TAG, "Failed to fill buffer, read error.");
      return -1;
    } else if (read == 0) {
      ESP_LOGW(OtaHelperLog::TAG, "Connection closed by remote end.");
      return total_read;
    } else {
      total_read += read;

      auto bytes_filled = std::to_string(read);
      int err = send(socket, bytes_filled.c_str(), bytes_filled.size(), 0);
      if (err < 0) {
        ESP_LOGE(OtaHelperLog::TAG, "Failed to ack when filling buffer.");
        return -1;
      }
      // Are we at the end?
      ESP_LOGV(OtaHelperLog::TAG, "Read %s bytes from socket, total_read: %d, total_bytes_left: %d",
               bytes_filled.c_str(), total_read, total_bytes_left);
      if (total_read >= total_bytes_left) {
        return total_read;
      }
    }
  }
  return total_read;
}

// #########################################################################
// ESP-IDF OTA generic
// #########################################################################

bool OtaHelper::writeStreamToPartition(
    const esp_partition_t *partition, FlashMode flash_mode, size_t content_length, std::string &md5hash,
    std::function<int(char *buffer, size_t buffer_size, size_t total_bytes_left)> fill_buffer) {
  char *buffer = (char *)malloc(SPI_FLASH_SEC_SIZE);
  if (buffer == nullptr) {
    ESP_LOGE(OtaHelperLog::TAG, "Failed to allocate buffer of size %d", SPI_FLASH_SEC_SIZE);
    return false;
  }

  uint8_t skip_buffer[ENCRYPTED_BLOCK_SIZE];

  EspNowMD5Builder md5;
  md5.begin();

  int bytes_read = 0;
  while (bytes_read < content_length) {
    int bytes_filled = fill_buffer(buffer, SPI_FLASH_SEC_SIZE, content_length - bytes_read);
    if (bytes_filled < 0) {
      ESP_LOGE(OtaHelperLog::TAG, "Unable to fill buffer");
      free(buffer);
      return false;
    }

    ESP_LOGV(OtaHelperLog::TAG, "Filled buffer with: %d", bytes_filled);

    // Special start case
    // Check start if contains the magic byte.
    uint8_t skip = 0;
    if (bytes_read == 0 && flash_mode == FlashMode::FIRMWARE) {
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

    md5.add((uint8_t *)buffer, (uint16_t)bytes_filled);
    bytes_read += bytes_filled;

    // If this is the end, finish up.
    if (bytes_read == content_length) {
      ESP_LOGI(OtaHelperLog::TAG, "End of stream, writing data to partition");

      if (!md5hash.empty()) {
        md5.calculate();
        if (md5hash != md5.toString()) {
          ESP_LOGE(OtaHelperLog::TAG, "MD5 checksum verification failed.");
          free(buffer);
          return false;
        } else {
          ESP_LOGI(OtaHelperLog::TAG, "MD5 checksum correct.");
        }
      }

      if (flash_mode == FlashMode::FIRMWARE) {
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
    }

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

const esp_partition_t *OtaHelper::findPartition(FlashMode flash_mode) {

  if (flash_mode == FlashMode::FIRMWARE) {
    const esp_partition_t *partition = esp_ota_get_next_update_partition(NULL);
    if (partition == nullptr) {
      ESP_LOGE(OtaHelperLog::TAG, "No firmware OTA partition found");
    }
    return partition;
  } else if (flash_mode == FlashMode::SPIFFS) {
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (partition == nullptr) {
      ESP_LOGE(OtaHelperLog::TAG, "No SPIIFS partition found");
    }
    return partition;
  }
  return nullptr;
}

// #########################################################################
// Rollback
// #########################################################################

void OtaHelper::rollbackWatcherTask(void *pvParameters) {
  OtaHelper *_this = (OtaHelper *)pvParameters;

  // Wait just slightly to give things time.
  vTaskDelay(5000 / portTICK_RATE_MS);

  auto wait_bits = _this->_rollback_bits_to_wait_for;
  if (wait_bits > 0) {
    xEventGroupWaitBits(_this->_rollback_event_group, wait_bits, pdFALSE, pdFALSE, portMAX_DELAY);
  }
  // We got all bits, or no bits to wait for. Canceling rollback.
  _this->cancelRollback();

  vTaskDelete(NULL);
}

// #########################################################################
// Generic utils
// #########################################################################

bool OtaHelper::reportOnError(esp_err_t err, const char *msg) {
  if (err != ESP_OK) {
    ESP_LOGE(OtaHelperLog::TAG, "%s: %s", msg, esp_err_to_name(err));
    return false;
  }
  return true;
}

void OtaHelper::replaceAll(std::string &s, const std::string &search, const std::string &replace) {
  for (size_t pos = 0;; pos += replace.length()) {
    // Locate the substring to replace
    pos = s.find(search, pos);
    if (pos == std::string::npos)
      break;
    // Replace by erasing and inserting
    s.erase(pos, search.length());
    s.insert(pos, replace);
  }
}

std::string OtaHelper::trim(const std::string &str) {
  auto first = std::find_if_not(str.begin(), str.end(), [](int c) { return std::isspace(c); });
  auto last = std::find_if_not(str.rbegin(), str.rend(), [](int c) { return std::isspace(c); }).base();
  return (first == last ? std::string() : std::string(first, last));
}