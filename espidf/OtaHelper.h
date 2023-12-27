#ifndef __OTA_HELPER_H__
#define __OTA_HELPER_H__

#include <cstdint>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_partition.h>
#include <functional>
#include <optional>
#include <string>

namespace OtaHelperLog {
const char TAG[] = "OtaHelper";
};

/**
 * @brief Create a OTA (Over The Air) helper
 *
 * Supports upload of firmware and spiffs via:
 * - ArduinoOTA (called espota in Platform I/O), using Platform I/O, Arduino IDE or stand alone tools.
 *   - supports authentication
 * - HTTP web interface via http://<device-ip>:<port-number>/
 *   - supports authentication
 * - Using URI via remote HTTP server, invoked by the device itself.
 */
class OtaHelper {
public:
  /**
   * @brief CRT Bundle Attach for Ardunio or ESP-IDF from MDTLS, to support TLS/HTTPS.
   *
   * Include esp_crt_bundle.h and pass the following when using respective framework:
   * for Arduino: arduino_esp_crt_bundle_attach
   * for ESP-IDF: esp_crt_bundle_attach
   *
   * C style function.
   */
  typedef esp_err_t (*CrtBundleAttach)(void *conf);

  struct Credentials {
    std::string username = "";
    std::string password = "";
  };

  /**
   * @brief Configuration for Arduino OTA (called espota in Platform I/O).
   */
  struct ArduinoOta {
    bool enabled = true;
    uint16_t udp_listenting_port = 3232;
    Credentials credentials; // Set username to non empty string to enable authentication.
  };

  /**
   * @brief Configuration for HTTP web interface.
   *
   */
  struct WebOta {
    std::string id = "";
    bool enabled = true;
    uint16_t http_port = 81;
    Credentials credentials; // Set username to non empty string to enable authentication.
  };

  enum class RollbackStrategy {
    /**
     * @brief The OtaHelper will automatically mark the new firmware as OK once all OTA services are up and running in a
     * steady state.
     */
    AUTO,
    /**
     * @brief User of the OtaHelper must manually call confirmRollback() to approve the new firmware. Otherwise it will
     * rollback to the previous version on reboot.
     */
    MANUAL,
  };

  struct Configuration {
    WebOta web_ota;
    ArduinoOta arduino_ota;
    /**
     * @brief Rollback must be enabled in menuconfig where
     * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-bootloader-app-rollback-enable
     * must be set. Without this, there will be no rollback.
     *
     * Rollback only apply when writing firmware, not spiffs.
     *
     */
    RollbackStrategy rollback_strategy = RollbackStrategy::AUTO;
  };

  /**
   * @brief Construct a new Ota Helper.
   *
   * To set log level for this object, use: esp_log_level_set(OtaHelperLog::TAG, ESP_LOG_*);
   *
   * @param configuration configuration for the OTA services and rollback strategy.
   */
  OtaHelper(Configuration configuration, CrtBundleAttach crt_bundle_attach = nullptr);

public:
  /**
   * @brief Call to start. Can only be called once there is a WiFi connection.
   *
   * @return true if successfully started.
   */
  bool start();
  void setup() { start(); }

  enum class FlashMode {
    FIRMWARE,
    SPIFFS,
  };

  /**
   * @brief Try to update firmware/spiffs from the given URL.
   * WiFi needs to be established first.
   *
   * @param url url to update from. This should be the bin file to update with.
   * @param flash_mode flash mode to use.
   * @param md5_hash 32 string character MD5 hash to validate written firmware/spiffs against. Empty to not validate.
   * @return true if successful (but will also reboot), so only the false case is useful.
   */
  bool updateFrom(std::string &url, FlashMode flash_mode, std::string md5_hash = "");

private: // OTA (generic)
  bool
  writeStreamToPartition(const esp_partition_t *partition, FlashMode flash_mode, size_t content_length,
                         std::string &md5hash,
                         std::function<int(char *buffer, size_t buffer_size, size_t total_bytes_left)> fill_buffer);
  bool writeBufferToPartition(const esp_partition_t *partition, size_t bytes_written, char *buffer, size_t buffer_size,
                              uint8_t skip);

  esp_err_t partitionIsBootable(const esp_partition_t *partition);
  bool checkDataInBlock(const uint8_t *data, size_t len);

  const esp_partition_t *findPartition(FlashMode flash_mode);

private: // OTA via local HTTP webserver / web UI
  bool startWebserver();
  int fillBuffer(httpd_req_t *req, char *buffer, size_t buffer_size);

  static esp_err_t httpGetHandler(httpd_req_t *req);
  static esp_err_t httpPostHandler(httpd_req_t *req);

private: // OTA via remote URI
  bool downloadAndWriteToPartition(const esp_partition_t *partition, FlashMode flash_mode, std::string &url,
                                   std::string &md5hash);
  static esp_err_t httpEventHandler(esp_http_client_event_t *evt);
  int fillBuffer(esp_http_client_handle_t client, char *buffer, size_t buffer_size);

private: // OTA via ArduinoOTA
  static void udpServerTask(void *pvParameters);

  struct ArduinoOtaUpdate {
    FlashMode flash_mode;
    uint16_t host_port;
    uint32_t size;
    std::string md5;
  };

  std::optional<ArduinoOtaUpdate> parseUdpPacket(char *buffer, size_t buffer_size);
  bool connectToHostForArduino(ArduinoOtaUpdate &update, char *host_ip);

  int fillBuffer(int socket, char *buffer, size_t buffer_size, size_t total_bytes_left);

private: // Generic utils
  bool reportOnError(esp_err_t err, const char *msg);
  void replaceAll(std::string &s, const std::string &search, const std::string &replace);

private:
  Configuration _configuration;
  CrtBundleAttach _crt_bundle_attach;
};

#endif // __OTA_HELPER_H__