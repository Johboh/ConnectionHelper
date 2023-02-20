# ConnectionHelper
A small utility library for setting up WiFi and OTA (Over The Air)

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

### Supported platforms
- ESP32
- ESP8266

### Dependencies
- https://github.com/ayushsharma82/ElegantOTA @^2.2.9