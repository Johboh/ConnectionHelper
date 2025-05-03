# ConnectionHelper
[![PlatformIO CI](https://github.com/Johboh/ConnectionHelper/actions/workflows/platformio.yaml/badge.svg)](https://registry.platformio.org/libraries/johboh/ConnectionHelper)
[![ESP-IDF](https://github.com/Johboh/ConnectionHelper/actions/workflows/espidf.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper/actions/workflows/espidf.yaml)
[![Arduino IDE](https://github.com/Johboh/ConnectionHelper/actions/workflows/arduino_cli.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper/actions/workflows/arduino_cli.yaml)
[![GitHub release](https://img.shields.io/github/release/Johboh/ConnectionHelper.svg)](https://github.com/Johboh/ConnectionHelper/releases)
[![Clang-format](https://github.com/Johboh/ConnectionHelper/actions/workflows/clang-format.yaml/badge.svg)](https://github.com/Johboh/ConnectionHelper)

Arduino (using Arduino IDE or PlatformIO) and ESP-IDF (using Espressif IoT Development Framework or PlatformIO) compatible library for setting up WiFi and OTA (Over The Air)

### OTA support
- ArduinoOTA (using the Arduino IDE, PlatformIO and using `upload_protocol = espota` or using the esp IDF command line options, `espota`)
- Upload via Web UI
  - via HTTP interface in browser
  - Via command line. Example: `curl -X POST -H "X-Flash-Mode: firmware" -H "Content-Type: application/octet-stream" --data-binary "@/path/to/firmware.bin" http://<device-ip>:<port-number>/`
  - Or use the included [upload.py](./upload.py) script: `python ./upload.py -u http://192.168.1.10:81 ./build/firmware.bin`
- Upload from URI (client driven).

### Installation
#### PlatformIO (Arduino or ESP-IDF):
Add the following to `libs_deps` for __ESP32__:
```
   Johboh/ConnectionHelper@^3.0.11
```
For __ESP8266__, use the legacy 2.x version:
```
   Johboh/ConnectionHelper@^2.0.5
```

#### Arduino IDE:
Search for `ConnectionHelper` by `johboh` in the library manager. See note about version above.

__Note__: Need ESP32 core v3.0.11 until [this issue](https://github.com/espressif/arduino-esp32/issues/10084) has been fixed. If you get issues with `undefined reference to `lwip_hook_ip6_input'`, try a different ESP32 core version. Need at least 3+ for C++17 support.
#### Espressif IoT Development Framework:
In your existing `idf_component.yml` or in a new `idf_component.yml` next to your main component:
```
dependencies:
  johboh/connectionhelper:
    version: ">=3.0.11"
```

### Example
- [Arduino framework](examples/arduino/simple/simple.ino)
- [ESP-IDF framework](examples/espidf/simple/main/main.cpp)

### Parition table
You need to have two app partitions in your parition table to be able to swap between otas, as well as the `otadata` section. This is an example for a 4MB flash:
```
# Name,   Type,  SubType, Offset,          Size, Flags
nvs,      data,      nvs,       ,           16K
otadata,  data,      ota,       ,            8K
phy_init, data,      phy,       ,            4K
coredump, data, coredump,       ,           64K
ota_0,     app,    ota_0,       ,         1500K
ota_1,     app,    ota_1,       ,         1500K
spiffs,   data,   spiffs,       ,          800K
```
To set partition table, save above in a file called `partitions_with_ota.csv`. For ESP-IDF, specify to use this one using menuconfig. For platform I/O, add the following to your `platformio.ini`: `board_build.partitions = partitions_with_ota.csv`

### Functionallity verified on the following platforms and frameworks
- ESP32 (tested with platform I/O [espressif32@6.4.0](https://github.com/platformio/platform-espressif32) / [arduino-esp32@2.0.11](https://github.com/espressif/arduino-esp32) / [ESP-IDF@4.4.6](https://github.com/espressif/esp-idf) / [ESP-IDF@5.1.2](https://github.com/espressif/esp-idf) on ESP32-S2 and ESP32-C3)
- __2.x-version__: ESP8266 (tested with platform I/O [espressif8266@4.2.1](https://github.com/platformio/platform-espressif8266) / [ardunio-core@3.1.2](https://github.com/esp8266/Arduino))

Newer version most probably work too, but they have not been verified.

### Dependencies
- None
