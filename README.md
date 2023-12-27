# ConnectionHelper
[![Platform I/O CI](https://github.com/Johboh/ConnectionHelper/actions/workflows/platformio.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper/actions/workflows/platformio.yaml)
[![ESP-IDF v5 CI](https://github.com/Johboh/ConnectionHelper/actions/workflows/espidf5.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper/actions/workflows/espidf5.yaml)
[![ESP-IDF v4 CI](https://github.com/Johboh/ConnectionHelper/actions/workflows/espidf4.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper/actions/workflows/espidf4.yaml)
[![ESP-IDF Component Upload](https://github.com/Johboh/ConnectionHelper/actions/workflows/esp_upload_component.yaml/badge.svg)](https://components.espressif.com/components/johboh/connectionhelper)
[![GitHub release](https://img.shields.io/github/release/Johboh/ConnectionHelper.svg)](https://github.com/Johboh/ConnectionHelper/releases)

Arduino (using Arduino IDE or Platform I/O) and ESP-IDF (using Espressif IoT Development Framework or Platform I/O) compatible library for setting up WiFi and OTA (Over The Air)

I found myself repeating the WiFI and OTA setup in all my projects so I made a reusable library for it.

### OTA support

#### Arduino framework
- ArduinoOTA (using the Arduino IDE, Platform I/O and using `upload_protocol = espota` or using the esp IDF command line options, `espota`.)
- Upload via Web UI using the [ElegantOTA](https://github.com/ayushsharma82/ElegantOTA) web UI provided update tool.

#### ESP-IDF framework
- ArduinoOTA (using the Arduino IDE, Platform I/O and using `upload_protocol = espota` or using the esp IDF command line options, `espota`)
- Upload via Web UI
- Upload from URI (client driven).

### Installation
#### Platform I/O (Arduino or ESP-IDF):
Add the following to `libs_deps`:
```
   Johboh/ConnectionHelper
```
#### Espressif IoT Development Framework:
In your existing `idf_component.yml` or in a new `idf_component.yml` next to your main component:
```
dependencies:
  johboh/ConnectionHelper:
    version: ">=2.0.0"
```

### Example
- [Arduino framework](examples/arduino/WifiAndOta.ino)
- [ESP-IDF framework](examples/espidf/main/main.cpp)

### Parition table
You need to have two app partitions in your parition table to be able to swap between otas. This is an example:
```
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     ,         0x4000
otadata,  data, ota,     ,         0x2000
phy_init, data, phy,     ,         0x1000
coredump, data, coredump,,         64K
ota_0,    app,  ota_0,   ,         1500K
ota_1,    app,  ota_1,   ,         1500K
spiffs,	  data,	spiffs,	 ,	       800K
```
To set partition table, save above in a file called `partitions_with_ota.csv`. For ESP-IDF, specify to use this one using menuconfig. For platform I/O, add the following to your `platformio.ini`: `board_build.partitions = partitions_with_ota.csv`

### Functionallity verified on the following platforms and frameworks
- ESP32 (tested with platform I/O [espressif32@6.4.0](https://github.com/platformio/platform-espressif32) / [arduino-esp32@2.0.11](https://github.com/espressif/arduino-esp32) / [ESP-IDF@4.4.6](https://github.com/espressif/esp-idf) / [ESP-IDF@5.1.2](https://github.com/espressif/esp-idf) on ESP32-S2 and ESP32-C3)
- ESP8266 (tested with platform I/O [espressif8266@4.2.1](https://github.com/platformio/platform-espressif8266) / [ardunio-core@3.1.2](https://github.com/esp8266/Arduino))

Newer version most probably work too, but they have not been verified.

### Dependencies
- Arduino only: https://github.com/ayushsharma82/ElegantOTA @^2.2.9
