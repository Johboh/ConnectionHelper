#include "WiFiHelper.h"
#ifdef ESP32
#include <WiFi.h>
#elif ESP8266
#include <ESP8266WiFi.h>
#else
#error "Unsupported hardware. Sorry!"
#endif

#define CHECK_WIFI_RECONNECT_MS 30000

WiFiHelper::WiFiHelper(const char *ssid, const char *password, const char *device_hostname)
    : _ssid(ssid), _password(password), _device_hostname(device_hostname) {}

bool WiFiHelper::connect(bool restart_on_failure) {
#ifdef ESP32
  WiFi.setHostname(_device_hostname); // For ESP32, must call for Wifi.mode()
#endif
  WiFi.mode(WIFI_STA);
#ifdef ESP8266
  WiFi.setHostname(_device_hostname); // For ESP8266, must call after Wifi.mode()
#endif
  WiFi.begin(_ssid, _password);
  while (WiFi.waitForConnectResult() != WL_CONNECTED) {
    Serial.println("Connection Failed! Rebooting...");
    if (restart_on_failure) {
      delay(1000);
      ESP.restart();
    } else {
      return false;
    }
  }

  Serial.println("have wifi");
  Serial.print("IP number: ");
  Serial.println(WiFi.localIP());
  Serial.print("MAC Address:  ");
  Serial.println(WiFi.macAddress());
  return true;
}

void WiFiHelper::handle() {
  auto now = millis();
  if (now - _last_wifi_check_timestamp_ms >= CHECK_WIFI_RECONNECT_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("WiFiHelper: Not connected. Reconnecting to WiFi...");
      WiFi.reconnect();
    } else {
      // We are connected.
      _last_known_connection_at_ms = now;
    }
    _last_wifi_check_timestamp_ms = now;
  }

  if (_restart_after_ms > 0 && _last_known_connection_at_ms > 0 &&
      now - _last_known_connection_at_ms >= _restart_after_ms) {
    // No connection for a while and user has requested to restart.
    ESP.restart();
  }
}

void WiFiHelper::restartOnConnectionLossAfter(unsigned long timeout) { _restart_after_ms = timeout; }