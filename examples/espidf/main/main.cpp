#include "credentials.h"
#include <OtaHelper.h>
#include <WiFiHelper.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "example"

#define PIN_LED GPIO_NUM_14

OtaHelper::Configuration ota_configuration = {
    .web_ota =
        {
            .id = hostname,
        },
};
OtaHelper _ota_helper(ota_configuration);
WiFiHelper _wifi_helper(
    hostname, []() { ESP_LOGI(TAG, "on connected callback"); }, []() { ESP_LOGI(TAG, "on disconnected callback"); });

void blinkAndSerialTask(void *pvParameters) {
  bool swap = false;
  while (1) {
    gpio_set_level(PIN_LED, swap);
    swap = !swap;
    ESP_LOGI(TAG, "Hello");
    vTaskDelay(1000 / portTICK_PERIOD_MS);
  }
}

extern "C" {
void app_main();
}

void app_main(void) {
  gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_LED, 1);

  xTaskCreate(blinkAndSerialTask, "blinkAndSerialTask", 2048, NULL, 15, NULL);

  auto connected = _wifi_helper.connectToAp(wifi_ssid, wifi_password, true, 10000);

  if (connected) {
    if (!_ota_helper.start()) {
      ESP_LOGE(TAG, "Failed to start OTA");
    }
  } else {
    ESP_LOGE(TAG, "Failed to connect");
  }

  while (1) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    fflush(stdout);
  }
}
