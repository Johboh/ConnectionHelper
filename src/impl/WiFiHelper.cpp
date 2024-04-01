#include "WiFiHelper.h"
#include <cstring>
#include <esp_err.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/err.h>
#include <lwip/sys.h>
#include <nvs.h>
#include <nvs_flash.h>

// Event group bits
#define WIFI_CONNECTED_BIT BIT0

void WiFiHelper::eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  WiFiHelper *_this = (WiFiHelper *)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(WiFiHelperLog::TAG, "WiFi disconnected");

    auto on_disconnected = _this->_on_disconnected;
    if (_this->_is_connected && on_disconnected != nullptr) {
      on_disconnected();
    }
    _this->_is_connected = false;

    if (_this->_reconnect) {
      ESP_LOGW(WiFiHelperLog::TAG, "Trying to reconnect...");
      esp_wifi_connect();
    }

  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    ESP_LOGI(WiFiHelperLog::TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
    memcpy(&_this->_ip_addr, &event->ip_info.ip, sizeof(esp_ip4_addr_t));

    xEventGroupSetBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);

    auto on_connected = _this->_on_connected;
    if (!_this->_is_connected && on_connected != nullptr) {
      on_connected();
    }
    _this->_is_connected = true;
  }
}

WiFiHelper::WiFiHelper(const char *device_hostname, std::function<void(void)> on_connected,
                       std::function<void(void)> on_disconnected)
    : _device_hostname(device_hostname), _on_connected(on_connected), _on_disconnected(on_disconnected) {
  _wifi_event_group = xEventGroupCreate();
}

bool WiFiHelper::connectToAp(const char *ssid, const char *password, bool initializeNVS, int timeout_ms,
                             bool reconnect) {
  _reconnect = reconnect;
  if (initializeNVS) {
    if (!this->initializeNVS()) {
      return false;
    }
  }

  if (!reportOnError(esp_netif_init(), "failed to initialize netif")) {
    return false;
  }
  if (!reportOnError(esp_event_loop_create_default(), "failed to create event loop")) {
    return false;
  }

  _netif_sta = esp_netif_create_default_wifi_sta();

  if (!reportOnError(esp_netif_set_hostname(_netif_sta, _device_hostname), "failed to set hostname")) {
    return false;
  }

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  if (!reportOnError(esp_wifi_init(&cfg), "failed to initialize wifi")) {
    return false;
  }

  esp_event_handler_instance_t instance_any_id;
  if (!reportOnError(
          esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, &instance_any_id),
          "failed to register event handler for any wifi event")) {
    return false;
  }

  esp_event_handler_instance_t instance_got_ip;
  if (!reportOnError(
          esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, this, &instance_got_ip),
          "failed to register event handler for IP event")) {
    return false;
  }

  wifi_config_t wifi_config = {};
  std::strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid));
  std::strncpy((char *)wifi_config.sta.password, password, sizeof(wifi_config.sta.password));

  if (!reportOnError(esp_wifi_set_mode(WIFI_MODE_STA), "failed to set wifi mode to STA")) {
    return false;
  }
  if (!reportOnError(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "failed to set wifi config")) {
    return false;
  }
  if (!reportOnError(esp_wifi_start(), "failed to start wifi")) {
    return false;
  }
  ESP_LOGI(WiFiHelperLog::TAG, "wifi_init_sta finished.");

  TickType_t xMaxBlockTime = timeout_ms / portTICK_PERIOD_MS;
  EventBits_t bits = xEventGroupWaitBits(_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, xMaxBlockTime);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
   * happened. */
  if (bits & WIFI_CONNECTED_BIT) {
    ESP_LOGI(WiFiHelperLog::TAG, "connected to AP with SSID: %s", ssid);
    return true;
  } else {
    ESP_LOGE(WiFiHelperLog::TAG, "Unable to connect to AP, timeout.");
  }

  // On failure, cleanup.
  disconnect();
  return false;
}

void WiFiHelper::disconnect() {
  _reconnect = false;
  esp_wifi_stop();
  if (_netif_sta != nullptr) {
    esp_netif_destroy_default_wifi(_netif_sta);
  }
  esp_event_loop_delete_default();
  esp_netif_deinit();
  esp_wifi_deinit();
}

bool WiFiHelper::initializeNVS() {
  ESP_LOGI(WiFiHelperLog::TAG, "Initializing NVS");
  // Initialize NVS

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_LOGW(WiFiHelperLog::TAG, "Erasing NVS (%s)", esp_err_to_name(err));
    err = nvs_flash_erase();
    if (err != ESP_OK) {
      ESP_LOGE(WiFiHelperLog::TAG, "Failed erase NVS (%s)", esp_err_to_name(err));
      return false;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    ESP_LOGE(WiFiHelperLog::TAG, "Failed to initialize NVS (%s)", esp_err_to_name(err));
    return false;
  }
  return true;
}

bool WiFiHelper::reportOnError(esp_err_t err, const char *msg) {
  if (err != ESP_OK) {
    ESP_LOGE(WiFiHelperLog::TAG, "%s: %s", msg, esp_err_to_name(err));
    return false;
  }
  return true;
}