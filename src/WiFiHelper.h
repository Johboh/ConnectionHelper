#ifndef __WIFI_HELPER_H__
#define __WIFI_HELPER_H__

#include <esp_event.h>
#include <esp_netif.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <functional>
#include <string>

#define TIMEOUT_CONNECT_MS 5000

namespace WiFiHelperLog {
const char TAG[] = "WiFiHelper";
};

/**
 * Helper for setting up and reconnecting to WiFi.
 */
class WiFiHelper {
public:
  /**
   * @brief Construct a new WiFi Helper.
   *
   * To set log level for this object, use: esp_log_level_set(WiFiHelperLog::TAG, ESP_LOG_*);
   *
   * @param device_hostname the name of this device. Will be used as hostname. From https://www.ietf.org/rfc/rfc1123.txt
   * "Each element of the hostname must be from 1 to 63 characters long
       and the entire hostname, including the dots, can be at most 253
       characters long.  Valid characters for hostnames are ASCII(7)
       letters from a to z, the digits from 0 to 9, and the hyphen (-).
       A hostname may not start with a hyphen."
   * @param on_connected optional callback on connect.
   * @param on_disconnected optional callback on disconnect.
   */
  WiFiHelper(const char *device_hostname, std::function<void(void)> on_connected = {},
             std::function<void(void)> on_disconnected = {});

public:
  /**
   * @brief Connect to AP.
   *
   * Note that NVS must have been setup before calling this function. Either your application does this, or you can set
   * initializeNVS to do it automatically.
   *
   * @param ssid the SSID to connect to.
   * @param password the password to use.
   * @param initializeNVS true to initialize NVS before connecting.
   * @param timeout_ms timeout in miliseconds to try to connect.
   * @param reconnect true to reconnect on connection loss.
   * @return true if a successful connection was established.
   */
  bool connectToAp(const char *ssid, const char *password, bool initializeNVS = true,
                   int timeout_ms = TIMEOUT_CONNECT_MS, bool reconnect = true);

  /**
   * @brief Disconnect from the AP.
   */
  void disconnect();

  /**
   * Return IP address when connected to AP.
   */
  esp_ip4_addr_t getIpAddress() { return _ip_addr; }

  /**
   * @brief return if connected to AP.
   */
  bool isConnected() { return _is_connected; }

private:
  bool initializeNVS();
  bool reportOnError(esp_err_t err, const char *msg);

private:
  static void eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data);

private:
  const char *_device_hostname;

private:
  bool _reconnect;
  bool _is_connected;
  esp_ip4_addr_t _ip_addr;
  esp_netif_t *_netif_sta;
  EventGroupHandle_t _wifi_event_group;
  std::function<void(void)> _on_connected;
  std::function<void(void)> _on_disconnected;
};

#endif // __WIFI_HELPER_H__