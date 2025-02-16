#ifndef __OTA_HELPER_H__
#define __OTA_HELPER_H__

#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_client.h>
#include <esp_http_server.h>
#include <esp_log.h>
#include <esp_netif.h>
#include <esp_partition.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <functional>
#include <inttypes.h>
#include <optional>
#include <stdbool.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace OtaHelperLog {
const char TAG[] = "OtaHelper";
};

/**
 * @brief Create a OTA (Over The Air) helper
 *
 * Supports upload of firmware and spiffs via:
 * - ArduinoOTA (called espota in PlatformIO), using PlatformIO, Arduino IDE or stand alone tools.
 *   - supports authentication
 * - HTTP web interface via http://<device-ip>:<port-number>/
 *   - supports authentication (Basic, not secure)
 *   - Can also be used as in script to upload firmware and spiffs directly by doing a file upload using POST to the
 * same URI and setting the "X-Flash-Mode" header to either "firmware" or "spiffs". Like this:
 *   curl -X POST -H "X-Flash-Mode: firmware" -H "Content-Type: application/octet-stream" \
 *        --data-binary "@/path/to/firmware.bin" http://<device-ip>:<port-number>/
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

  /**
   * @brief Configuration for Arduino OTA (called espota in PlatformIO).
   */
  struct ArduinoOta {
    bool enabled = true;
    uint16_t udp_listenting_port = 3232;
    /**
     * Set password to non empty string to enable authentication.
     */
    std::string password = "";

    /**
     * The priority of the UDP update task.
     * Setting this too low will starve the update task and you might not be able to update. Usually you want to keep
     * this high, but if high it will also potentially also starve your other tasks.
     */
    UBaseType_t task_priority = 25;
  };

  struct Credentials {
    std::string username = "";
    std::string password = "";
  };

  /**
   * @brief Configuration for HTTP web interface.
   *
   */
  struct WebOta {
    std::string id = "";
    bool enabled = true;
    uint16_t http_port = 81;
    /**
     * Set username to non empty string to enable authentication.
     * Note: Only Basic authentication is supported.
     * Note: You might need to set
     * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-httpd-max-req-hdr-len
     * to 1024 or higher in menuconfig in case of "431 Request Header Fields Too Large - Header fields are too long for
     * server to interpret" errors.
     */
    Credentials credentials = {};
  };

  enum class RollbackStrategy {
    /**
     * @brief The OtaHelper will automatically mark the new firmware as OK once all OTA services are up and
     * rollback_timeout_ms has passed.
     */
    AUTO,
    /**
     * @brief User of the OtaHelper must manually call confirmRollback() to approve the new firmware. Otherwise it will
     * rollback to the previous version on reboot.
     */
    MANUAL,
  };

  struct Configuration {
    WebOta web_ota = {};
    ArduinoOta arduino_ota = {};
    /**
     * @brief Rollback must be enabled in menuconfig where
     * https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/kconfig.html#config-bootloader-app-rollback-enable
     * must be set. Without this, there will be no rollback.
     *
     * Rollback only apply when writing firmware, not spiffs. There is no automatic invalidation, but instead the app
     * will be reverted on reboot if the rollback is not canceled (manually or automatically).
     *
     */
    RollbackStrategy rollback_strategy = RollbackStrategy::AUTO;
    /**
     * If roolback strategy is AUTO, this is the timeout to wait for the new firmware to be marked as valid, in
     * milliseconds.
     */
    uint32_t rollback_timeout_ms = 5000;
  };

  enum class OtaStatus {
    UPDATE_STARTED,   // Firmware update has started.
    UPDATE_FAILED,    // Firmare update has failed.
    UPDATE_COMPLETED, // Firmware update has completed.
  };

  using OtaStatusCallback = std::function<void(OtaStatus)>;

  /**
   * @brief Construct a new Ota Helper.
   *
   * If not using log callback (see addOnLog()), set log level for this object using:
   * esp_log_level_set(OtaHelperLog::TAG, ESP_LOG_*);
   *
   * @param configuration configuration for the OTA services and rollback strategy.
   * @param crt_bundle_attach CRT Bundle Attach for Ardunio or ESP-IDF from MDTLS, to support TLS/HTTPS. See definition
   * of CrtBundleAttach.
   * @param ota_status_callback optional callback for reciving OTA status. Do not do anything critical/blocking in these
   * callbacks as they will delay/block the OTA process.
   *
   */
  OtaHelper(Configuration configuration, CrtBundleAttach crt_bundle_attach = nullptr,
            OtaStatusCallback ota_status_callback = {});

public:
  /**
   * @brief Call to start. Can only be called once there is a WiFi connection.
   *
   * @return true if successfully started.
   */
  bool start();

  /**
   * @brief If the rollback strategy is MANUAL, call this to confirm that the new firmware is OK. Otherwise the previous
   * image will be rolled back on reboot (if rollback is enabled in menuconfig, see RollbackStrategy).
   */
  void cancelRollback();

  enum class FlashMode {
    FIRMWARE,
    SPIFFS,
  };

  /**
   * @brief Try to update firmware/spiffs from the given URL.
   * WiFi needs to be established first.
   * Will not restart/reboot on success or failure. Caller is responsible to reboot on sucess (or at a convinient time).
   *
   * @param url url to update from. This should be the bin file to update with.
   * @param flash_mode flash mode to use.
   * @param md5_hash 32 string character MD5 hash to validate written firmware/spiffs against. Empty to not validate.
   * @return true if successful.
   */
  bool updateFrom(std::string &url, FlashMode flash_mode, std::string md5_hash = "");

  /**
   * @brief Callback when this object want to log something.
   *
   * @param message the log message to log.
   * @param log_level the severity of the log.
   */
  using OnLog = std::function<void(const std::string message, const esp_log_level_t log_level)>;

  /**
   * @brief Register log callback. Normally you can use esp_log_level_set(OtaHelperLog::TAG,
   * ESP_LOG_*); to set the log level for this object, but if that is not possible or you want more control over
   * logging, you can add a log callback. When set, the log level set using esp_log_level_set() is ignored.
   */
  void addOnLog(OnLog on_log) { _on_log.push_back(on_log); }

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

  void setNotAuthenticatedResonse(httpd_req_t *req);

  // Return true if user is authenticated, or if no authentication is required.
  // Will set proper headers. If this returns false, caller should not continue or set anything.
  bool handleAuthentication(httpd_req_t *req);

  static esp_err_t httpGetHandler(httpd_req_t *req);
  static esp_err_t httpPostHandler(httpd_req_t *req);

private: // OTA via remote URI
  bool downloadAndWriteToPartition(const esp_partition_t *partition, FlashMode flash_mode, std::string &url,
                                   std::string &md5hash);
  static esp_err_t httpEventHandler(esp_http_client_event_t *evt);
  int fillBuffer(esp_http_client_handle_t client, char *buffer, size_t buffer_size);

private: // OTA via ArduinoOTA
  static void arduinoOtaUdpServerTask(void *pvParameters);

  struct ArduinoAuthUpdate {
    std::string cnonce;
    std::string response;
  };

  struct ArduinoOtaHandshake {
    FlashMode flash_mode;
    uint16_t host_port;
    uint32_t size;
    std::string md5;
  };

  std::optional<ArduinoAuthUpdate> parseAuthUdpPacket(char *buffer, size_t buffer_size);
  std::optional<ArduinoOtaHandshake> parseHandshakeUdpPacket(char *buffer, size_t buffer_size);
  bool connectToHostForArduino(ArduinoOtaHandshake &update, char *host_ip);

  int fillBuffer(int socket, char *buffer, size_t buffer_size, size_t total_bytes_left);

private: // Rollback
  static void rollbackWatcherTask(void *pvParameters);

private: // Generic utils
  void reportStatus(OtaStatus status);
  bool reportOnError(esp_err_t err, const char *msg);
  void replaceAll(std::string &s, const std::string &search, const std::string &replace);
  std::string trim(const std::string &str);
  void log(const esp_log_level_t log_level, std::string message);

private:
  std::vector<OnLog> _on_log;
  Configuration _configuration;
  CrtBundleAttach _crt_bundle_attach;
  uint8_t _rollback_bits_to_wait_for;
  EventGroupHandle_t _rollback_event_group;
  OtaStatusCallback _ota_status_callback;
};

#endif // __OTA_HELPER_H__