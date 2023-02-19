#ifndef __WIFI_HELPER_H__
#define __WIFI_HELPER_H__

#include <Arduino.h>

/**
 * Helper for setting up and reconnecting to WiFi.
 */
class WiFiHelper {
public:
  /**
   * @brief Construct a new WiFi Helper.
   *
   * @param ssid the SSID to connect to.
   * @param password the password to use.
   * @param device_hostname the name of this device. Will be used as hostname. From https://www.ietf.org/rfc/rfc1123.txt
   * "Each element of the hostname must be from 1 to 63 characters long
       and the entire hostname, including the dots, can be at most 253
       characters long.  Valid characters for hostnames are ASCII(7)
       letters from a to z, the digits from 0 to 9, and the hyphen (-).
       A hostname may not start with a hyphen."
   */
  WiFiHelper(const char *ssid, const char *password, const char *device_hostname);

public:
  /**
   * @brief Call from main Arduino setup() function to connect to WiFi.
   *
   * @param restart_on_failure if true, will restart if connection failed.
   */
  bool connect(bool restart_on_failure = true);

  /**
   * @brief Call from main Arduino loop() function.
   */
  void handle();

private:
  const char *_ssid;
  const char *_password;
  const char *_device_hostname;
  unsigned long _last_wifi_check_timestamp_ms = 0;
};

#endif // __WIFI_HELPER_H__