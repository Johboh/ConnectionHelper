#ifndef __OTA_HELPER_H__
#define __OTA_HELPER_H__

#include <Arduino.h>
#ifdef ESP32
#include <WebServer.h>
#elif ESP8266
#include <ESP8266Webserver.h>
#else
#error "Unsupported hardware. Sorry!"
#endif

/**
 * @brief Create a OTA (Over The Air) helper
 *
 * Will setup ArduinoOTA and ElegantOTA.
 * ElegantOTA will be available on http://<device-ip>:<port-number/update
 */
class OtaHelper {
public:
  /**
   * @brief Construct a new Ota Helper.
   *
   * @param id the ID to set to ElegantOTA to this device can be identified when using the web interface to upload new
   * firmware.
   * @param port the port number to run ElegantOTA.
   */
  OtaHelper(const char *id, uint16_t port = 81);

public:
  /**
   * @brief Call from main Arduino setup() function.
   */
  void setup();

  /**
   * @brief Call from main Arduino loop() function.
   */
  void handle();

private:
  const char *_id;
#ifdef ESP32
  WebServer _elegant_ota_server;
#elif ESP8266
  ESP8266WebServer _elegant_ota_server;
#else
#error "Unsupported hardware. Sorry!"
#endif
};

#endif // __OTA_HELPER_H__