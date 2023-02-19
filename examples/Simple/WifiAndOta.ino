#include <Ardunio.h>
#include <OtaHelper.h>
#include <WiFiHelper.h>

const char user_hostname[] = "my-hostname";
const char user_wifi_ssid[] = "my-ssid";
const char user_wifi_password[] = "my-password";

OtaHelper _ota_helper(user_hostname);
WiFiHelper _wifi_helper(user_wifi_ssid, user_wifi_password, user_hostname);

void setup() {
  Serial.begin(115200);
  _wifi_helper.connect();
  _ota_helper.setup();
}

void loop() {
  _wifi_helper.handle();
  _ota_helper.handle();
}
