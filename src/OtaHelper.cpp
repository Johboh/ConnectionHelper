#include "OtaHelper.h"
#include <ArduinoOTA.h>
#include <ElegantOTA.h>

OtaHelper::OtaHelper(const char *id, uint16_t port) : _id(id), _elegant_ota_server(port) {}

void OtaHelper::setup() {
  ArduinoOTA.onStart([]() { Serial.println("OTA: Start"); });
  ArduinoOTA.onEnd([]() { Serial.println("\nOTA: End"); });
  ArduinoOTA.onProgress([](unsigned int progress, unsigned int total) {
    Serial.printf("OTA: Progress: %u%%\r", (progress / (total / 100)));
  });
  ArduinoOTA.onError([](ota_error_t error) {
    Serial.printf("OTA: Error[%u]: ", error);
    if (error == OTA_AUTH_ERROR)
      Serial.println("Auth Failed");
    else if (error == OTA_BEGIN_ERROR)
      Serial.println("Begin Failed");
    else if (error == OTA_CONNECT_ERROR)
      Serial.println("Connect Failed");
    else if (error == OTA_RECEIVE_ERROR)
      Serial.println("Receive Failed");
    else if (error == OTA_END_ERROR)
      Serial.println("End Failed");
  });
  ArduinoOTA.begin();

  ElegantOTA.begin(&_elegant_ota_server);
  ElegantOTA.setID(_id);
  _elegant_ota_server.begin();
}

void OtaHelper::handle() {
  ArduinoOTA.handle();
  _elegant_ota_server.handleClient();
}