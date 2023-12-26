#ifndef __OTA_HELPER_H__
#define __OTA_HELPER_H__

#include <cstdint>
#include <esp_err.h>
#include <esp_event.h>
#include <esp_http_server.h>
#include <esp_netif.h>
#include <esp_partition.h>
#include <functional>
#include <string>

namespace OtaHelperLog {
const char TAG[] = "OtaHelper";
};

/**
 * @brief Create a OTA (Over The Air) helper
 *
 * Supports the espOTA and ArduinoOTA, and upload via HTTP interace at http://<device-ip>:<port-number/update
 */
class OtaHelper {
public:
  /**
   * @brief Construct a new Ota Helper.
   *
   * To set log level for this object, use: esp_log_level_set(OtaHelperLog::TAG, ESP_LOG_*);
   *
   * @param id the ID for identifying this device when using the web interface to upload new
   * firmware.
   * @param port the port number to run the HTTP webserver.
   */
  OtaHelper(const char *id, uint16_t port = 81);

public:
  /**
   * @brief Call to start. Can only be called once there is a WiFi connection.
   *
   * @return true if successfully started.
   */
  bool start();
  void setup() { start(); }

private: // Flow
  bool startWebserver();

private: // OTA
  int fillBuffer(httpd_req_t *req, char *buffer, size_t buffer_size);
  bool writeStreamToPartition(const esp_partition_t *partition, httpd_req_t *req);
  bool writeBufferToPartition(const esp_partition_t *partition, size_t bytes_written, char *buffer, size_t buffer_size,
                              uint8_t skip);

  esp_err_t partitionIsBootable(const esp_partition_t *partition);
  bool checkDataInBlock(const uint8_t *data, size_t len);

private: // Utils
  bool reportOnError(esp_err_t err, const char *msg);
  void replaceAll(std::string &s, const std::string &search, const std::string &replace);

private: // Static callbacks
  static esp_err_t httpGetHandler(httpd_req_t *req);
  static esp_err_t httpPostHandler(httpd_req_t *req);

private:
  uint16_t _port;
  const std::string _id;
};

#endif // __OTA_HELPER_H__