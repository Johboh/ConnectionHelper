#include <Arduino.h>
#include <OtaHelper.h>
#include <WiFiHelper.h>

// For this example only.
// Suggest to not store credentials in version controlled files.
const char hostname[] = "my-hostname";
const char wifi_ssid[] = "my-ssid";
const char wifi_password[] = "my-password";

// Configure OTA and set hostname for identifying this device.
// Otherwise use defaults.
OtaHelper::Configuration ota_configuration = {
    .web_ota =
        {
            .id = hostname,
        },
};
OtaHelper _ota_helper(ota_configuration);
WiFiHelper _wifi_helper(hostname);

void setup() {
  Serial.begin(115200);

  // Add logging callbacks when using Arduino framework. When using ESP-IDF, use set_log_level() instead. See
  // constructor and addOnLog().
  _ota_helper.addOnLog([](const std::string message, const esp_log_level_t log_level) {
    Serial.println("OtaHelper: " + String(message.c_str())); // ignoring log_level, logs everything. Noisy.
  });
  _wifi_helper.addOnLog([](const std::string message, const esp_log_level_t log_level) {
    Serial.println("WifiHelper: " + String(message.c_str())); // ignoring log_level, logs everything. Noisy.
  });

  bool initialize_nvs = true;
  int timeout_ms = 10000;
  auto connected = _wifi_helper.connectToAp(wifi_ssid, wifi_password, initialize_nvs, timeout_ms);
  if (connected) {
    _ota_helper.start();
  }
}

void loop() {}
