#include <OtaHelper.h>
#include <WiFiHelper.h>
#include <driver/gpio.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// For this example only.
// Suggest to not store credentials in version controlled files.
const char hostname[] = "my-hostname";
const char wifi_ssid[] = "my-ssid";
const char wifi_password[] = "my-password";

#define TAG "example"

#define PIN_LED GPIO_NUM_14

// Configure OTA and set hostname for identifying this device.
// Otherwise use defaults.
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
  // Setup led and blinking led task
  gpio_set_direction(PIN_LED, GPIO_MODE_OUTPUT);
  gpio_set_level(PIN_LED, 1);
  xTaskCreate(blinkAndSerialTask, "blinkAndSerialTask", 2048, NULL, 15, NULL);

  // Set log levels.
  esp_log_level_set(OtaHelperLog::TAG, ESP_LOG_INFO);
  esp_log_level_set(WiFiHelperLog::TAG, ESP_LOG_INFO);

  // Connect to WIFI with 10s timeout.
  bool initialize_nvs = true;
  bool timeout_ms = 10000;
  auto connected = _wifi_helper.connectToAp(wifi_ssid, wifi_password, initialize_nvs, timeout_ms);
  if (connected) {
    // Connected to WIFI, start OTA.
    if (!_ota_helper.start()) {
      ESP_LOGE(TAG, "Failed to start OTA");
    }
  } else {
    ESP_LOGE(TAG, "Failed to connect");
  }

  // Run forever.
  while (1) {
    vTaskDelay(500 / portTICK_PERIOD_MS);
    fflush(stdout);
  }
}
