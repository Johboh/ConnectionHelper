#include "OtaHelper.h"
#include "LogHelper.h"
#include "MD5Builder.h"
#include "ota_html.h"
#include <esp_app_format.h>
#include <esp_log.h>
#include <esp_ota_ops.h>
#include <esp_timer.h>
#include <esp_tls_crypto.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
#include <esp_flash_spi_init.h>
#endif
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
#include <spi_flash_mmap.h>
#endif

// Arduino OTA specific
#define UDP_CMD_WRITE_FIRMWARE 0
#define UDP_CMD_WRITE_SPIFFS 100
#define UDP_CMD_AUTH 200
#define ESPOTA_SUCCESSFUL "OK"
#define ARDUINO_OTA_TASK_STACK_SIZE 4096

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
#define ARDUINO_OTA_STARTED_BIT BIT0
#define WEB_OTA_STARTED_BIT BIT1
#define ROLLBACK_TASK_STACK_SIZE 2048
#define ROLLBACK_TASK_PRIORITY 5

// #########################################################################
// Public API
// #########################################################################

OtaHelper::OtaHelper(Configuration configuration, CrtBundleAttach crt_bundle_attach,
                     OtaStatusCallback ota_status_callback)
    : _configuration(configuration), _crt_bundle_attach(crt_bundle_attach), _ota_status_callback(ota_status_callback) {
  _rollback_event_group = xEventGroupCreate();
}

bool OtaHelper::start() {

  _rollback_bits_to_wait_for = 0;
  xEventGroupClearBits(_rollback_event_group, 0xFF);

  // Username cleanup
  _configuration.web_ota.credentials.username = trim(_configuration.web_ota.credentials.username);

  log(ESP_LOG_INFO, "Starting OtaHelper with the following configuration");
  log(ESP_LOG_INFO, "  - Rollback Strategy: " +
                        std::string(_configuration.rollback_strategy == RollbackStrategy::AUTO ? "auto" : "manual"));
  if (_configuration.rollback_strategy == RollbackStrategy::AUTO) {
    log(ESP_LOG_INFO, "  - Rollback Timeout: " + std::to_string(_configuration.rollback_timeout_ms) + "ms");
  }

  log(ESP_LOG_INFO, "  - Web UI/OTA: " + std::string(_configuration.web_ota.enabled ? "enabled" : "disabled"));
  if (_configuration.web_ota.enabled) {
    log(ESP_LOG_INFO, "    - http port: " + std::to_string(_configuration.web_ota.http_port));
    log(ESP_LOG_INFO, "    - id: " + _configuration.web_ota.id);
    auto username = _configuration.web_ota.credentials.username;
    if (!username.empty()) {
      log(ESP_LOG_INFO, "    - username: " + _configuration.web_ota.credentials.username);
    }
    _rollback_bits_to_wait_for += WEB_OTA_STARTED_BIT;
  }

  log(ESP_LOG_INFO, "  - Arduino OTA: " + std::string(_configuration.arduino_ota.enabled ? "enabled" : "disabled"));
  if (_configuration.arduino_ota.enabled) {
    log(ESP_LOG_INFO, "    - UDP listenting port: " + std::to_string(_configuration.arduino_ota.udp_listenting_port));
    auto password = _configuration.arduino_ota.password;
    if (!password.empty()) {
      log(ESP_LOG_INFO, "    - auth: enabled");
    }
    log(ESP_LOG_INFO, "    - UDP task priority: " + std::to_string(_configuration.arduino_ota.task_priority));

    _rollback_bits_to_wait_for += ARDUINO_OTA_STARTED_BIT;
  }

  log(ESP_LOG_INFO, "  - Remote URI download: enabled (always)");

  if (_configuration.rollback_strategy == RollbackStrategy::AUTO) {
    auto can_rollback = esp_ota_check_rollback_is_possible();
    if (can_rollback) {
      log(ESP_LOG_INFO,
          "Starting rollback task with timeout " + std::to_string(_configuration.rollback_timeout_ms) + "ms");
      xTaskCreate(rollbackWatcherTask, "rollback", ROLLBACK_TASK_STACK_SIZE, this, ROLLBACK_TASK_PRIORITY, NULL);
    } else {
      log(ESP_LOG_INFO, "Not starting rollback watcher as there is no other app to rollback to or "
                        "CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE is not enabled in sdkconfig.");
    }
  }

  if (_configuration.arduino_ota.enabled) {
    xTaskCreate(arduinoOtaUdpServerTask, "arduino_udp", ARDUINO_OTA_TASK_STACK_SIZE, this,
                _configuration.arduino_ota.task_priority, NULL);
  }

  bool success = !_configuration.web_ota.enabled || startWebserver();
  return success;
}

void OtaHelper::cancelRollback() {
  if (!esp_ota_check_rollback_is_possible()) {
    ESP_LOGI(OtaHelperLog::TAG, "No rollback to cancel.");
  } else {
    log(ESP_LOG_INFO, "Canceling rollback and accepting the new firmware (if firmware where written)");
    esp_ota_mark_app_valid_cancel_rollback();
  }
}

bool OtaHelper::updateFrom(std::string &url, FlashMode flash_mode, std::string md5_hash) {
  auto *partition = findPartition(flash_mode);
  if (partition == nullptr) {
    log(ESP_LOG_ERROR, "Unable to find partition suitable partition");
    return ESP_FAIL;
  }

  if (!md5_hash.empty() && md5_hash.length() != 32) {
    log(ESP_LOG_ERROR, "MD5 is not correct length. Expected length: 32, got " + std::to_string(md5_hash.length()));
    return false;
  }

  reportStatus(OtaStatus::UPDATE_STARTED);
  log(ESP_LOG_INFO, "OTA started via remoteHTTP with target partition: " + std::string(partition->label));

  auto success = downloadAndWriteToPartition(partition, flash_mode, url, md5_hash);
  if (success) {
    reportStatus(OtaStatus::UPDATE_COMPLETED);
  } else {
    reportStatus(OtaStatus::UPDATE_FAILED);
  }
  return success;
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
      log(ESP_LOG_ERROR, "Unable to get authorization header: " + std::string(esp_err_to_name(err)));
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
      log(ESP_LOG_WARN, "Credentials does not match");
      setNotAuthenticatedResonse(req);
      return false;
    }

  } else {
    log(ESP_LOG_INFO, "No credentials provided");
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

  std::string html;
  html.assign(ota_html, sizeof(ota_html));
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
    _this->log(ESP_LOG_ERROR, "Unable to get flash mode (firmware or spiffs): " + std::string(esp_err_to_name(err)));
    httpd_resp_send(req, "Unable to get flash mode (firmware or spiffs)", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  FlashMode flash_mode;
  if (strcmp(hdr_value, FLASH_MODE_FIRMWARE_STR) == 0) {
    flash_mode = FlashMode::FIRMWARE;
  } else if (strcmp(hdr_value, FLASH_MODE_SPIFFS_STR) == 0) {
    flash_mode = FlashMode::SPIFFS;
  } else {
    _this->log(ESP_LOG_ERROR, "Invalid flash mode: " + std::string(hdr_value));
    httpd_resp_send(req, "Invalid flash mode", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  const esp_partition_t *partition = _this->findPartition(flash_mode);
  if (partition == nullptr) {
    _this->log(ESP_LOG_ERROR, "Unable to find partition suitable partition");
    httpd_resp_send(req, "Unable to find suitable partition", HTTPD_RESP_USE_STRLEN);
    return ESP_FAIL;
  }

  _this->reportStatus(OtaStatus::UPDATE_STARTED);
  _this->log(ESP_LOG_INFO, "OTA started via HTTP with target partition: " + std::string(partition->label));

  if (req->content_len == 0) {
    _this->log(ESP_LOG_ERROR, "No content received");
    httpd_resp_send(req, "No content received", HTTPD_RESP_USE_STRLEN);
    _this->reportStatus(OtaStatus::UPDATE_FAILED);
    return ESP_FAIL;
  }

  std::string md5hash = ""; // No hash
  if (!_this->writeStreamToPartition(partition, flash_mode, req->content_len, md5hash,
                                     [&_this, req](char *buffer, size_t buffer_size, size_t total_bytes_left) {
                                       return _this->fillBuffer(req, buffer, buffer_size);
                                     })) {
    _this->log(ESP_LOG_ERROR, "Failed to write stream to partition");
    httpd_resp_send(req, "Failed to write stream to partition", HTTPD_RESP_USE_STRLEN);
    _this->reportStatus(OtaStatus::UPDATE_FAILED);
    return ESP_FAIL;
  }

  _this->reportStatus(OtaStatus::UPDATE_COMPLETED);
  _this->log(ESP_LOG_INFO, "HTTP OTA complete, rebooting...");

  httpd_resp_set_status(req, HTTPD_200);
  httpd_resp_send(req, NULL, 0);
  vTaskDelay(2000 / portTICK_PERIOD_MS);
  esp_restart();
  return ESP_OK;
}

bool OtaHelper::startWebserver() {
  httpd_handle_t server = NULL;
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  // Must use unique unique internal UDP port in case of several HTTP servers on this host. OK to wrap.
  config.ctrl_port = config.ctrl_port + _configuration.web_ota.http_port;
  config.server_port = _configuration.web_ota.http_port;
  config.lru_purge_enable = true;
  config.max_uri_handlers = _configuration.web_ota.ui_enabled ? 2 : 1;
  config.max_open_sockets = 2;

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

  if (_configuration.web_ota.ui_enabled) {
    httpd_uri_t ota_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = httpGetHandler,
        .user_ctx = this,
    };
    if (!reportOnError(httpd_register_uri_handler(server, &ota_root), "failed to register uri handler for OTA root")) {
      return false;
    }
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
        log(ESP_LOG_ERROR, "Failed to fill buffer, read zero and not complete.");
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
  OtaHelper *_this = (OtaHelper *)evt->user_data;

  switch (evt->event_id) {
  case HTTP_EVENT_ERROR:
    _this->log(ESP_LOG_ERROR, "HTTP_EVENT_ERROR");
    break;
  case HTTP_EVENT_ON_CONNECTED:
    _this->log(ESP_LOG_INFO, "HTTP_EVENT_ON_CONNECTED");
    break;
  case HTTP_EVENT_HEADER_SENT:
    _this->log(ESP_LOG_VERBOSE, "HTTP_EVENT_HEADER_SENT");
    break;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 1, 0)
  case HTTP_EVENT_REDIRECT:
    _this->log(ESP_LOG_VERBOSE, "HTTP_EVENT_REDIRECT");
    break;
#endif
  case HTTP_EVENT_ON_HEADER:
    _this->log(ESP_LOG_VERBOSE, "HTTP_EVENT_ON_HEADER, key=" + std::string(evt->header_key) +
                                    ", value=" + std::string(evt->header_value));
    break;
  case HTTP_EVENT_ON_DATA:
    _this->log(ESP_LOG_VERBOSE, "HTTP_EVENT_ON_DATA, len=" + std::to_string(evt->data_len));
    break;
  case HTTP_EVENT_ON_FINISH:
    _this->log(ESP_LOG_INFO, "HTTP_EVENT_ON_FINISH");
    break;
  case HTTP_EVENT_DISCONNECTED:
    _this->log(ESP_LOG_INFO, "HTTP_EVENT_DISCONNECTED");
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
    log(ESP_LOG_INFO, "With TLS/HTTPS support");
  } else {
    log(ESP_LOG_INFO, "Without TLS/HTTPS support");
  }
  esp_http_client_handle_t client = esp_http_client_init(&config);

  log(ESP_LOG_INFO, "Using URL " + url);
  esp_http_client_set_method(client, HTTP_METHOD_GET);
  esp_http_client_set_header(client, "Accept", "*/*");
  esp_http_client_set_timeout_ms(client, HTTP_REMOTE_TIMEOUT_MS);

  bool success = false;
  esp_err_t r = esp_http_client_open(client, 0);
  if (r == ESP_OK) {
    esp_http_client_fetch_headers(client);
    auto status_code = esp_http_client_get_status_code(client);
    auto content_length = esp_http_client_get_content_length(client);
    log(ESP_LOG_INFO,
        "HTTP status code: " + std::to_string(status_code) + ", content length: " + std::to_string(content_length));

    if (status_code == 200) {
      uint32_t partition_size = partition->size;
      if (content_length > partition_size) {
        log(ESP_LOG_ERROR, "Content length " + std::to_string(content_length) + " is larger than partition size " +
                               std::to_string(partition_size));

      } else {
        success = writeStreamToPartition(partition, flash_mode, content_length, md5hash,
                                         [&](char *buffer, size_t buffer_size, size_t total_bytes_left) {
                                           return fillBuffer(client, buffer, buffer_size);
                                         });
      }
    } else {
      log(ESP_LOG_ERROR, "Got non 200 status code: " + std::to_string(status_code));
    }

  } else {
    const char *errstr = esp_err_to_name(r);
    log(ESP_LOG_ERROR, "Failed to open HTTP connection: " + std::string(errstr));
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
        log(ESP_LOG_ERROR, "Failed to fill buffer, read zero and not complete.");
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
    // Two state management: if false we are waiting for the first normal handshake package. But if set to true we are
    // waiting for the second authentication package.
    // This is reset on any failure.
    bool waiting_for_auth = false;
    std::string auth_nonce = "";
    std::optional<ArduinoOtaHandshake> handshake_packet;

    struct sockaddr_in dest_addr_ip4;
    dest_addr_ip4.sin_addr.s_addr = htonl(INADDR_ANY);
    dest_addr_ip4.sin_family = AF_INET;
    dest_addr_ip4.sin_port = htons(port);
    int ip_protocol = IPPROTO_IP;

    int sock = socket(AF_INET, SOCK_DGRAM, ip_protocol);
    if (sock < 0) {
      _this->log(ESP_LOG_ERROR, "Unable to create UDP socket: errno " + std::to_string(errno));
      break;
    }
    _this->log(ESP_LOG_INFO, "UDP socket created");

    int err = bind(sock, (struct sockaddr *)&dest_addr_ip4, sizeof(dest_addr_ip4));
    if (err < 0) {
      _this->log(ESP_LOG_ERROR, "UDP socket unable to bind: errno " + std::to_string(errno));
      break;
    }
    _this->log(ESP_LOG_INFO, "UDP socket bound, port " + std::to_string(port));
    xEventGroupSetBits(_this->_rollback_event_group, ARDUINO_OTA_STARTED_BIT);

    struct sockaddr_storage source_addr;
    socklen_t socklen = sizeof(source_addr);

    while (1) {
      _this->log(ESP_LOG_INFO, "waiting UDP packet...");
      int len = recvfrom(sock, rx_buffer, sizeof(rx_buffer) - 1, 0, (struct sockaddr *)&source_addr, &socklen);
      _this->log(ESP_LOG_VERBOSE, "Got UDP packet with length " + std::to_string(len));

      // Error occurred during receiving?
      if (len < 0) {
        _this->log(ESP_LOG_ERROR, "UDP recvfrom failed: errno " + std::to_string(errno));
        break;
      } else {
        std::string reply_string;

        // Parse packets conditionally.
        std::optional<ArduinoAuthUpdate> auth_packet;

        if (!waiting_for_auth) {
          handshake_packet = _this->parseHandshakeUdpPacket(rx_buffer, len);
          if (!handshake_packet) {
            _this->log(ESP_LOG_ERROR, "Failed to parse handshake UDP packet");
            break;
          }

          // Need auth?
          if (!_this->_configuration.arduino_ota.password.empty()) {
            // Generate nounce.
            ConnectionHelperUtils::MD5Builder nonce_md5;
            nonce_md5.begin();
            nonce_md5.add(std::to_string(esp_timer_get_time()));
            nonce_md5.calculate();
            auth_nonce = nonce_md5.toString();
            reply_string = "AUTH " + auth_nonce;
            waiting_for_auth = true;
          } else {
            // Send OK
            reply_string = "OK";
          }

        } else {
          auth_packet = _this->parseAuthUdpPacket(rx_buffer, len);
          if (!auth_packet) {
            _this->log(ESP_LOG_ERROR, "Failed to parse auth UDP packet");
            break;
          }

          // Verify authentication
          ConnectionHelperUtils::MD5Builder passwordmd5;
          passwordmd5.begin();
          passwordmd5.add(_this->_configuration.arduino_ota.password);
          passwordmd5.calculate();
          auto challenge = passwordmd5.toString() + ":" + auth_nonce + ":" + auth_packet->cnonce;

          ConnectionHelperUtils::MD5Builder challengemd5;
          challengemd5.begin();
          challengemd5.add(challenge);
          challengemd5.calculate();
          auto result = challengemd5.toString();
          if (result == auth_packet->response) {
            reply_string = "OK";
          } else {
            _this->log(ESP_LOG_WARN, "Authentication Failed");
            reply_string = "Authentication Failed";
          }

          waiting_for_auth = false;
        }

        // Get the sender's ip address as string
        inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);

        int err = sendto(sock, reply_string.c_str(), reply_string.size(), 0, (struct sockaddr *)&source_addr,
                         sizeof(source_addr));
        if (err < 0) {
          _this->log(ESP_LOG_ERROR, "error occurred during sending UDP: errno " + std::to_string(errno));
          break;
        } else {
          _this->log(ESP_LOG_VERBOSE, "Sent UDP reply: " + reply_string);
        }

        // Handle OTA (if not waiting for auth)
        if (handshake_packet && !waiting_for_auth) {
          _this->reportStatus(OtaStatus::UPDATE_STARTED);
          auto result = _this->connectToHostForArduino(*handshake_packet, addr_str);
          if (result) {
            _this->reportStatus(OtaStatus::UPDATE_COMPLETED);
            vTaskDelay(2000 / portTICK_PERIOD_MS);
            esp_restart();
          } else {
            _this->reportStatus(OtaStatus::UPDATE_FAILED);
          }
          break; // Fail or OK, restart UDP.
        }
      }
    }

    if (sock != -1) {
      _this->log(ESP_LOG_ERROR, "Shutting down UDP and restarting socket...");
      shutdown(sock, 0);
      close(sock);
    }
  }
  vTaskDelete(NULL);
}

/**
 * @brief Parses the buffer received from the Arduino upload server.
 * The buffer contains the received packet.
 * The packet first contains an integer as a string (this is the command, uint8_t), followed by a space,
 * then another integer as a string (this is the size of the host port number, uint16_t), followed by a space,
 * then another integer as a string (this is the size of the firmware, uint16_t), followed by a space,
 * then a MD5 string in hex (32 characters) which ends with a new line.
 *
 * @param buffer the input buffer.
 * @param buffer_size the length of the buffer.
 */
std::optional<OtaHelper::ArduinoOtaHandshake> OtaHelper::parseHandshakeUdpPacket(char *buffer, size_t buffer_size) {
  ArduinoOtaHandshake update;

  // Tokenize the buffer using strtok function
  char *token = strtok(buffer, " ");
  if (token == nullptr) {
    return std::nullopt;
  }

  // Parse command integer
  uint8_t command = static_cast<uint8_t>(std::atoi(token));
  if (command == UDP_CMD_WRITE_FIRMWARE) {
    update.flash_mode = FlashMode::FIRMWARE;
  } else if (command == UDP_CMD_WRITE_SPIFFS) {
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

std::optional<OtaHelper::ArduinoAuthUpdate> OtaHelper::parseAuthUdpPacket(char *buffer, size_t buffer_size) {
  ArduinoAuthUpdate auth;

  // Tokenize the buffer using strtok function
  char *token = strtok(buffer, " ");
  if (token == nullptr) {
    return std::nullopt;
  }

  // Parse command integer
  uint8_t command = static_cast<uint8_t>(std::atoi(token));
  if (command != UDP_CMD_AUTH) {
    return std::nullopt;
  }

  // Parse cnonce
  token = strtok(nullptr, " ");
  if (token == nullptr) {
    return std::nullopt;
  }
  auth.cnonce = token;
  if (auth.cnonce.length() != 32) {
    return std::nullopt;
  }

  // Parse response
  token = strtok(nullptr, "\n");
  if (token == nullptr) {
    return std::nullopt;
  }
  auth.response = token;
  if (auth.response.length() != 32) {
    return std::nullopt;
  }

  return auth;
}

bool OtaHelper::connectToHostForArduino(ArduinoOtaHandshake &update, char *host_ip) {
  log(ESP_LOG_VERBOSE, "Connecting to host " + std::string(host_ip));
  log(ESP_LOG_VERBOSE, "host_port: " + std::to_string(update.host_port));
  log(ESP_LOG_VERBOSE, "flash_mode: " + std::to_string((int)update.flash_mode));
  log(ESP_LOG_VERBOSE, "size: " + std::to_string(update.size));
  log(ESP_LOG_VERBOSE, "md5: " + update.md5);

  const esp_partition_t *partition = findPartition(update.flash_mode);
  if (partition == nullptr) {
    log(ESP_LOG_ERROR, "Unable to find suitable partition");
    return ESP_FAIL;
  }
  log(ESP_LOG_INFO, "OTA started via TCP with target partition: " + std::string(partition->label));

  struct sockaddr_in dest_addr;
  inet_pton(AF_INET, host_ip, &dest_addr.sin_addr);
  dest_addr.sin_family = AF_INET;
  dest_addr.sin_port = htons(update.host_port);

  int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
  if (sock < 0) {
    log(ESP_LOG_ERROR, "Unable to create TCP client socket: errno " + std::to_string(errno));
    return false;
  }
  log(ESP_LOG_INFO,
      "TCP client socket created, connecting to " + std::string(host_ip) + ":" + std::to_string(update.host_port));

  int err = connect(sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
  if (err != 0) {
    log(ESP_LOG_ERROR, "TCP client socket unable to connect: errno " + std::to_string(errno));
    shutdown(sock, 0);
    close(sock);
    return false;
  }
  log(ESP_LOG_INFO, "Successfully connected to host");

  auto ok = writeStreamToPartition(partition, update.flash_mode, update.size, update.md5,
                                   [&](char *buffer, size_t buffer_size, size_t total_bytes_left) {
                                     return fillBuffer(sock, buffer, buffer_size, total_bytes_left);
                                   });
  if (!ok) {
    log(ESP_LOG_ERROR, "Failed to write stream to partition");
    shutdown(sock, 0);
    close(sock);
    return false;
  }

  log(ESP_LOG_INFO, "TCP OTA complete, rebooting...");

  err = send(sock, ESPOTA_SUCCESSFUL, strlen(ESPOTA_SUCCESSFUL), 0);
  if (err < 0) {
    log(ESP_LOG_ERROR, "Failed to ack TCP update, its fine.");
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
      log(ESP_LOG_ERROR, "Failed to fill buffer, read error.");
      return -1;
    } else if (read == 0) {
      log(ESP_LOG_WARN, "Connection closed by remote end.");
      return total_read;
    } else {
      total_read += read;

      auto bytes_filled = std::to_string(read);
      int err = send(socket, bytes_filled.c_str(), bytes_filled.size(), 0);
      if (err < 0) {
        log(ESP_LOG_ERROR, "Failed to ack when filling buffer.");
        return -1;
      }
      // Are we at the end?
      log(ESP_LOG_VERBOSE, "Read " + bytes_filled + " bytes from socket, total_read: " + std::to_string(total_read) +
                               ", total_bytes_left: " + std::to_string(total_bytes_left));
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
    log(ESP_LOG_ERROR, "Failed to allocate buffer of size " + std::to_string(SPI_FLASH_SEC_SIZE));
    return false;
  }

  uint8_t skip_buffer[ENCRYPTED_BLOCK_SIZE];

  ConnectionHelperUtils::MD5Builder md5;
  md5.begin();

  int bytes_read = 0;
  while (bytes_read < content_length) {
    int bytes_filled = fill_buffer(buffer, SPI_FLASH_SEC_SIZE, content_length - bytes_read);
    if (bytes_filled < 0) {
      log(ESP_LOG_ERROR, "Unable to fill buffer");
      free(buffer);
      return false;
    }

    log(ESP_LOG_VERBOSE, "Filled buffer with: " + std::to_string(bytes_filled));

    // Special start case
    // Check start if contains the magic byte.
    uint8_t skip = 0;
    if (bytes_read == 0 && flash_mode == FlashMode::FIRMWARE) {
      if (buffer[0] != ESP_IMAGE_HEADER_MAGIC) {
        log(ESP_LOG_ERROR, "Start of firwmare does not contain magic byte");
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
      log(ESP_LOG_ERROR, "Failed to write buffer to partition");
      free(buffer);
      return false;
    }

    md5.add((uint8_t *)buffer, (uint16_t)bytes_filled);
    bytes_read += bytes_filled;

    // If this is the end, finish up.
    if (bytes_read == content_length) {
      log(ESP_LOG_INFO, "End of stream, writing data to partition");

      if (!md5hash.empty()) {
        md5.calculate();
        if (md5hash != md5.toString()) {
          log(ESP_LOG_ERROR, "MD5 checksum verification failed.");
          free(buffer);
          return false;
        } else {
          log(ESP_LOG_INFO, "MD5 checksum correct.");
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
      log(ESP_LOG_ERROR, "No firmware OTA partition found");
    }
    return partition;
  } else if (flash_mode == FlashMode::SPIFFS) {
    const esp_partition_t *partition =
        esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_SPIFFS, NULL);
    if (partition == nullptr) {
      log(ESP_LOG_ERROR, "No SPIIFS partition found");
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

  // Wait for rollback_timeout_ms until confirming.
  vTaskDelay(_this->_configuration.rollback_timeout_ms / portTICK_PERIOD_MS);

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

void OtaHelper::reportStatus(OtaStatus status) {
  if (_ota_status_callback) {
    _ota_status_callback(status);
  }
}

bool OtaHelper::reportOnError(esp_err_t err, const char *msg) {
  if (err != ESP_OK) {
    log(ESP_LOG_ERROR, std::string(msg) + ": " + std::string(esp_err_to_name(err)));
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
// Not optimal, need to figure out the ::ranges situation.
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 4, 0)
  auto first = std::ranges::find_if_not(str.begin(), str.end(), [](int c) { return std::isspace(c); });
  auto last = std::ranges::find_if_not(str.rbegin(), str.rend(), [](int c) { return std::isspace(c); }).base();
#else
  auto first = std::find_if_not(str.begin(), str.end(), [](int c) { return std::isspace(c); });
  auto last = std::find_if_not(str.rbegin(), str.rend(), [](int c) { return std::isspace(c); }).base();
#endif
  return (first == last ? std::string() : std::string(first, last));
}

void OtaHelper::log(const esp_log_level_t log_level, std::string message) {
  if (_on_log.size() > 0) {
    for (auto &on_log : _on_log) {
      on_log(message, log_level);
    }
  } else {
    LogHelper::log(OtaHelperLog::TAG, log_level, message);
  }
}