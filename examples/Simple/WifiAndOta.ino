#include <Ardunio.h>
#include <OtaHelper.h>
#include <WiFiHelper.h>

const char hostname[] = "my-hostname";
const char wifi_ssid[] = "my-ssid";
const char wifi_password[] = "my-password";

OtaHelper _ota_helper(hostname);
WiFiHelper _wifi_helper(wifi_ssid, wifi_password, hostname);

void setup() {
  Serial.begin(115200);
  _wifi_helper.connect();
  _ota_helper.setup();
}

void loop() {
  _wifi_helper.handle();
  _ota_helper.handle();
}
