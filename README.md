# ConnectionHelper
[![Test](https://github.com/Johboh/ConnectionHelper/actions/workflows/test.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper/actions/workflows/test.yaml)
[![GitHub release](https://img.shields.io/github/release/Johboh/ConnectionHelper.svg)](https://github.com/Johboh/ConnectionHelper/releases)

A small Arduino utility library for setting up WiFi and OTA (Over The Air)

I found myself repeating the WiFI and OTA setup in all my projects so I made a reusable library for it.

OTA gives several options.
- Using the Arduino IDE, Platform I/O and using `upload_protocol = espota` or using the esp IDF command line options, `espota`.
- Using the [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) web UI provided update tool. See [OtaHelper](src/OtaHelper.h) for more documentation.

### Usage
```C++
#include <Arduino.h>
#include <OtaHelper.h>
#include <WiFiHelper.h>

OtaHelper _ota_helper("hostname");
WiFiHelper _wifi_helper("wifi_ssid", "wifi_password", "hostname");

void setup() {
  Serial.begin(115200);
  _wifi_helper.connect();
  _ota_helper.setup();
}

void loop() {
  _wifi_helper.handle();
  _ota_helper.handle();
}
```

### Example
See [simple example](examples/Simple/WifiAndOta.ino).

### Functionallity verified on the following platforms and frameworks
- ESP32 (tested with platform I/O [espressif32@5.3.0](https://github.com/platformio/platform-espressif32) / [arduino-esp32@2.0.6](https://github.com/espressif/arduino-esp32) on ESP32-S2 and ESP32-C3)
- ESP8266 (tested with platform I/O [espressif8266@4.0.1](https://github.com/platformio/platform-espressif8266) / [ardunio-core@3.0.2](https://github.com/esp8266/Arduino))

Newer version most probably work too, but they have not been verified.

### Dependencies
- https://github.com/ayushsharma82/ElegantOTA @^2.2.9
