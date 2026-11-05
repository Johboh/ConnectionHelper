// Copyright (c) Johan Böhlin
// SPDX-License-Identifier: MIT

#include "WiFiHelper.h"
#include "LogHelper.h"
#include <algorithm>
#include <cstring>
#include <esp_err.h>
#include <esp_idf_version.h>
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

// Reconnection parameters
#define WIFI_BACKOFF_MIN_MS 1000
#define WIFI_BACKOFF_MAX_MS 30000
#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_DHCP_TIMEOUT_MS 30000
#define WIFI_DRIVER_RESET_RETRY_COUNT 16

namespace {
wifi_config_t makeStaConfig(const char *ssid, const char *password) {
  wifi_config_t wifi_config = {};
  if (ssid != nullptr) {
    std::memcpy(wifi_config.sta.ssid, ssid, std::min(std::strlen(ssid), sizeof(wifi_config.sta.ssid)));
  }
  if (password != nullptr) {
    std::memcpy(wifi_config.sta.password, password,
                std::min(std::strlen(password), sizeof(wifi_config.sta.password) - 1));
  }
  wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
  wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
  wifi_config.sta.threshold.rssi = -127;
  wifi_config.sta.pmf_cfg.capable = true;
  wifi_config.sta.pmf_cfg.required = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
  wifi_config.sta.failure_retry_cnt = 2;
#endif
  return wifi_config;
}
} // namespace

void WiFiHelper::reconnectTimerCallback(void *arg) {
  WiFiHelper *_this = static_cast<WiFiHelper *>(arg);
  if (!_this->_reconnect) {
    return;
  }

  if (_this->_l2_connected) {
    esp_netif_ip_info_t ip_info = {};
    if (_this->_netif_sta != nullptr && esp_netif_get_ip_info(_this->_netif_sta, &ip_info) == ESP_OK &&
        ip_info.ip.addr != 0) {
      memcpy(&_this->_ip_addr, &ip_info.ip, sizeof(esp_ip4_addr_t));
      _this->_retry_count = 0;
      bool was_connected = _this->_is_connected.exchange(true);
      xEventGroupSetBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);
      auto on_connected = _this->_on_connected;
      if (!was_connected && on_connected != nullptr) {
        on_connected();
      }
      return;
    }

    _this->log(ESP_LOG_WARN, "DHCP timed out, forcing WiFi reconnect...");
    _this->_l2_connected = false;
    xEventGroupClearBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);
    if (_this->_is_connected.exchange(false)) {
      memset(&_this->_ip_addr, 0, sizeof(esp_ip4_addr_t));
      auto on_disconnected = _this->_on_disconnected;
      if (on_disconnected != nullptr) {
        on_disconnected();
      }
    }
    esp_timer_start_once(_this->_reconnect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
    esp_wifi_disconnect();
    return;
  }

  if (_this->_is_connected.exchange(false)) {
    xEventGroupClearBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);
    memset(&_this->_ip_addr, 0, sizeof(esp_ip4_addr_t));
    auto on_disconnected = _this->_on_disconnected;
    if (on_disconnected != nullptr) {
      on_disconnected();
    }
  }

  uint8_t retry = _this->_retry_count.load();
  if (retry >= WIFI_DRIVER_RESET_RETRY_COUNT && (retry % WIFI_DRIVER_RESET_RETRY_COUNT) == 0) {
    _this->log(ESP_LOG_WARN, "Resetting WiFi driver after repeated failures...");
    _this->_retry_count = retry + 1;
    esp_timer_start_once(_this->_reconnect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
    esp_wifi_stop();
    esp_wifi_start();
    return;
  }

  _this->log(ESP_LOG_WARN, "Trying to reconnect...");
  esp_timer_start_once(_this->_reconnect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
  esp_err_t err = esp_wifi_connect();
  if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
    _this->log(ESP_LOG_WARN, "esp_wifi_connect failed: " + std::string(esp_err_to_name(err)));
  }
}

void WiFiHelper::eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  WiFiHelper *_this = static_cast<WiFiHelper *>(arg);
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    if (_this->_reconnect && _this->_reconnect_timer != nullptr) {
      esp_timer_stop(_this->_reconnect_timer);
      esp_timer_start_once(_this->_reconnect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
    }
    esp_wifi_connect();
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    _this->_l2_connected = true;
    if (_this->_reconnect && _this->_reconnect_timer != nullptr) {
      esp_timer_stop(_this->_reconnect_timer);
      esp_timer_start_once(_this->_reconnect_timer, WIFI_DHCP_TIMEOUT_MS * 1000ULL);
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    uint8_t reason = event != nullptr ? event->reason : 0;

#ifdef WIFI_REASON_ROAMING
    if (reason == WIFI_REASON_ROAMING) {
      _this->log(ESP_LOG_INFO, "WiFi roaming to another AP...");
      _this->_l2_connected = false;
      if (_this->_reconnect && _this->_reconnect_timer != nullptr) {
        esp_timer_stop(_this->_reconnect_timer);
        esp_timer_start_once(_this->_reconnect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
      }
      return;
    }
#endif

    _this->log(ESP_LOG_WARN, "WiFi disconnected (reason: " + std::to_string(reason) + ")");
    _this->_l2_connected = false;
    xEventGroupClearBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);

    if (_this->_is_connected.exchange(false)) {
      memset(&_this->_ip_addr, 0, sizeof(esp_ip4_addr_t));
      auto on_disconnected = _this->_on_disconnected;
      if (on_disconnected != nullptr) {
        on_disconnected();
      }
    }

    if (_this->_reconnect && _this->_reconnect_timer != nullptr) {
      uint8_t retry = _this->_retry_count.load();
      uint32_t shift = retry < 5 ? retry : 5;
      uint32_t delay_ms = std::min<uint32_t>(WIFI_BACKOFF_MAX_MS, WIFI_BACKOFF_MIN_MS << shift);
      uint8_t next_retry = retry + 1;
      if (next_retry >= 64) {
        next_retry = WIFI_DRIVER_RESET_RETRY_COUNT;
      }
      _this->_retry_count = next_retry;
      esp_timer_stop(_this->_reconnect_timer);
      esp_timer_start_once(_this->_reconnect_timer, static_cast<uint64_t>(delay_ms) * 1000ULL);
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = static_cast<ip_event_got_ip_t *>(event_data);
    auto ipaddr = event->ip_info.ip;
    _this->log(ESP_LOG_INFO, "got ip: " + std::to_string(esp_ip4_addr1(&ipaddr)) + "." +
                                 std::to_string(esp_ip4_addr2(&ipaddr)) + "." + std::to_string(esp_ip4_addr3(&ipaddr)) +
                                 "." + std::to_string(esp_ip4_addr4(&ipaddr)));
    memcpy(&_this->_ip_addr, &event->ip_info.ip, sizeof(esp_ip4_addr_t));
    _this->_l2_connected = true;
    _this->_retry_count = 0;
    if (_this->_reconnect_timer != nullptr) {
      esp_timer_stop(_this->_reconnect_timer);
    }

    bool was_connected = _this->_is_connected.exchange(true);
    xEventGroupSetBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);

    auto on_connected = _this->_on_connected;
    if (!was_connected && on_connected != nullptr) {
      on_connected();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    if (!_this->_is_connected.exchange(false)) {
      return;
    }

    _this->log(ESP_LOG_WARN, "WiFi lost IP address");
    memset(&_this->_ip_addr, 0, sizeof(esp_ip4_addr_t));
    xEventGroupClearBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);

    auto on_disconnected = _this->_on_disconnected;
    if (on_disconnected != nullptr) {
      on_disconnected();
    }

    if (_this->_reconnect && _this->_l2_connected && _this->_reconnect_timer != nullptr) {
      esp_timer_stop(_this->_reconnect_timer);
      esp_timer_start_once(_this->_reconnect_timer, WIFI_DHCP_TIMEOUT_MS * 1000ULL);
    }
  }
}

WiFiHelper::WiFiHelper(const char *device_hostname, std::function<void(void)> on_connected,
                       std::function<void(void)> on_disconnected)
    : _device_hostname(), _on_connected(on_connected), _on_disconnected(on_disconnected) {
  // ESP-IDF limits hostname to 32 bytes max
  if (device_hostname != nullptr) {
    std::string hostname(device_hostname);
    if (hostname.size() > 32) {
      hostname.resize(32);
    }
    _device_hostname = std::move(hostname);
  }
  _wifi_event_group = xEventGroupCreate();
}

WiFiHelper::~WiFiHelper() {
  disconnect();
  if (_wifi_event_group != nullptr) {
    vEventGroupDelete(_wifi_event_group);
    _wifi_event_group = nullptr;
  }
}

bool WiFiHelper::connectToAp(const char *ssid, const char *password, bool initializeNVS, int timeout_ms,
                             bool reconnect) {
  if (ssid == nullptr || ssid[0] == '\0') {
    log(ESP_LOG_ERROR, "SSID must not be empty");
    return false;
  }

  _reconnect = reconnect;
  wifi_config_t wifi_config = makeStaConfig(ssid, password);

  if (_initialized) {
    wifi_config_t current_cfg = {};
    bool same_cfg =
        (esp_wifi_get_config(WIFI_IF_STA, &current_cfg) == ESP_OK &&
         std::memcmp(current_cfg.sta.ssid, wifi_config.sta.ssid, sizeof(wifi_config.sta.ssid)) == 0 &&
         std::memcmp(current_cfg.sta.password, wifi_config.sta.password, sizeof(wifi_config.sta.password)) == 0);

    if (same_cfg && _is_connected) {
      return true;
    }

    if (!same_cfg) {
      if (_reconnect_timer != nullptr) {
        esp_timer_stop(_reconnect_timer);
      }
      _retry_count = 0;
      xEventGroupClearBits(_wifi_event_group, WIFI_CONNECTED_BIT);
      esp_wifi_stop();
      if (!reportOnError(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "failed to set wifi config")) {
        return false;
      }
      if (!reportOnError(esp_wifi_start(), "failed to start wifi")) {
        return false;
      }
    } else if (!_l2_connected && (_reconnect_timer == nullptr || !esp_timer_is_active(_reconnect_timer))) {
      if (_reconnect && _reconnect_timer != nullptr) {
        esp_timer_start_once(_reconnect_timer, WIFI_CONNECT_TIMEOUT_MS * 1000ULL);
      }
      esp_wifi_connect();
    }
  } else {
    if (initializeNVS) {
      if (!this->initializeNVS()) {
        return false;
      }
    }

    _retry_count = 0;
    xEventGroupClearBits(_wifi_event_group, WIFI_CONNECTED_BIT);

    if (!reportOnError(esp_netif_init(), "failed to initialize netif")) {
      return false;
    }

    esp_err_t loop_err = esp_event_loop_create_default();
    if (loop_err != ESP_OK && loop_err != ESP_ERR_INVALID_STATE) {
      reportOnError(loop_err, "failed to create event loop");
      return false;
    }

    if (_netif_sta == nullptr) {
      _netif_sta = esp_netif_create_default_wifi_sta();
    }

    if (!_device_hostname.empty()) {
      if (!reportOnError(esp_netif_set_hostname(_netif_sta, _device_hostname.c_str()), "failed to set hostname")) {
        disconnect();
        return false;
      }
    }

    if (_reconnect_timer == nullptr) {
      esp_timer_create_args_t timer_args = {};
      timer_args.callback = &reconnectTimerCallback;
      timer_args.arg = this;
      timer_args.dispatch_method = ESP_TIMER_TASK;
      timer_args.name = "wifi_reconnect";
      if (!reportOnError(esp_timer_create(&timer_args, &_reconnect_timer), "failed to create reconnect timer")) {
        disconnect();
        return false;
      }
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!reportOnError(esp_wifi_init(&cfg), "failed to initialize wifi")) {
      disconnect();
      return false;
    }
    _initialized = true;

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (!reportOnError(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, &_instance_any_id),
            "failed to register event handler for any wifi event")) {
      disconnect();
      return false;
    }

    if (!reportOnError(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, this, &_instance_got_ip),
            "failed to register event handler for IP got event")) {
      disconnect();
      return false;
    }

    if (!reportOnError(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &eventHandler, this,
                                                           &_instance_lost_ip),
                       "failed to register event handler for IP lost event")) {
      disconnect();
      return false;
    }

    if (!reportOnError(esp_wifi_set_mode(WIFI_MODE_STA), "failed to set wifi mode to STA")) {
      disconnect();
      return false;
    }
    if (!reportOnError(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "failed to set wifi config")) {
      disconnect();
      return false;
    }
    if (!reportOnError(esp_wifi_start(), "failed to start wifi")) {
      disconnect();
      return false;
    }
    log(ESP_LOG_INFO, "wifi_init_sta finished.");
  }

  TickType_t xMaxBlockTime = timeout_ms > 0 ? pdMS_TO_TICKS(timeout_ms) : 0;
  EventBits_t bits = xEventGroupWaitBits(_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, xMaxBlockTime);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
   * happened. */
  if ((bits & WIFI_CONNECTED_BIT) != 0) {
    log(ESP_LOG_INFO, "connected to AP with SSID: " + std::string(ssid));
    return true;
  }

  if (_reconnect) {
    log(ESP_LOG_WARN, "Unable to connect to AP within timeout, continuing to reconnect in background.");
    return false;
  }

  log(ESP_LOG_ERROR, "Unable to connect to AP, timeout.");
  // On failure without background reconnect, cleanup.
  disconnect();
  return false;
}

void WiFiHelper::disconnect() {
  _reconnect = false;
  if (_reconnect_timer != nullptr) {
    esp_timer_stop(_reconnect_timer);
    esp_timer_delete(_reconnect_timer);
    _reconnect_timer = nullptr;
  }

  if (_instance_any_id != nullptr) {
    esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, _instance_any_id);
    _instance_any_id = nullptr;
  }
  if (_instance_got_ip != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, _instance_got_ip);
    _instance_got_ip = nullptr;
  }
  if (_instance_lost_ip != nullptr) {
    esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_LOST_IP, _instance_lost_ip);
    _instance_lost_ip = nullptr;
  }

  if (_initialized) {
    esp_wifi_disconnect();
    esp_wifi_stop();
    esp_wifi_deinit();
    _initialized = false;
  }

  if (_netif_sta != nullptr) {
    esp_netif_destroy_default_wifi(_netif_sta);
    _netif_sta = nullptr;
  }

  _is_connected = false;
  _l2_connected = false;
  _retry_count = 0;
  memset(&_ip_addr, 0, sizeof(esp_ip4_addr_t));
  if (_wifi_event_group != nullptr) {
    xEventGroupClearBits(_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

std::optional<int8_t> WiFiHelper::rssi() {
  if (!_is_connected) {
    return std::nullopt;
  }

  int rssi_value;
  esp_err_t err = esp_wifi_sta_get_rssi(&rssi_value);
  if (err != ESP_OK) {
    log(ESP_LOG_WARN, "Failed to get RSSI: " + std::string(esp_err_to_name(err)));
    return std::nullopt;
  }
  return static_cast<int8_t>(rssi_value);
}

bool WiFiHelper::initializeNVS() {
  log(ESP_LOG_INFO, "Initializing NVS");
  // Initialize NVS

  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    log(ESP_LOG_WARN, "Erasing NVS: " + std::string(esp_err_to_name(err)));
    err = nvs_flash_erase();
    if (err != ESP_OK) {
      log(ESP_LOG_ERROR, "Failed erase NVS: " + std::string(esp_err_to_name(err)));
      return false;
    }
    err = nvs_flash_init();
  }
  if (err != ESP_OK) {
    log(ESP_LOG_ERROR, "Failed to initialize NVS: " + std::string(esp_err_to_name(err)));
    return false;
  }
  return true;
}

bool WiFiHelper::reportOnError(esp_err_t err, const char *msg) {
  if (err != ESP_OK) {
    log(ESP_LOG_ERROR, std::string(msg) + ": " + std::string(esp_err_to_name(err)));
    return false;
  }
  return true;
}

void WiFiHelper::log(const esp_log_level_t log_level, const std::string &message) {
  if (!_on_log.empty()) {
    for (const auto &on_log : _on_log) {
      on_log(message, log_level);
    }
  } else {
    LogHelper::log(WiFiHelperLog::TAG, log_level, message.c_str());
  }
}
