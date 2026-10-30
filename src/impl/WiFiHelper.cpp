#include "WiFiHelper.h"
#include "LogHelper.h"
#include <cstring>
#include <esp_err.h>
#include <esp_idf_version.h>
#include <esp_log.h>
#include <esp_mac.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <esp_wifi.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lwip/err.h>
#include <lwip/icmp.h>
#include <lwip/inet_chksum.h>
#include <lwip/ip.h>
#include <lwip/sockets.h>
#include <lwip/sys.h>
#include <nvs.h>
#include <nvs_flash.h>

// Event group bits
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_WAKE_MONITOR_BIT BIT1
#define WIFI_STOP_TASK_BIT BIT2
#define WIFI_TASK_STOPPED_BIT BIT3

// Reconnection and health check parameters
#define WIFI_MONITOR_TASK_STACK_SIZE 4096
#define WIFI_MONITOR_TASK_PRIORITY 5
#define WIFI_BACKOFF_MIN_MS 1000
#define WIFI_BACKOFF_MAX_MS 30000
#define WIFI_CONNECT_ATTEMPT_TIMEOUT_MS 15000
#define WIFI_DHCP_TIMEOUT_MS 20000
#define WIFI_DRIVER_RESET_FAILURE_COUNT 10
#define WIFI_GATEWAY_INITIAL_CHECK_MS 5000
#define WIFI_GATEWAY_CHECK_INTERVAL_MS 30000
#define WIFI_GATEWAY_MAX_FAILURES 3

void WiFiHelper::eventHandler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  WiFiHelper *_this = (WiFiHelper *)arg;
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    _this->_connect_in_progress = true;
    _this->_last_connect_attempt_ms = _this->millis();
    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
      _this->log(ESP_LOG_WARN, "esp_wifi_connect failed on start: " + std::string(esp_err_to_name(err)));
      _this->_connect_in_progress = false;
      _this->_last_disconnect_ms = _this->millis();
      _this->_consecutive_failures++;
      xEventGroupSetBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
    }
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
    _this->log(ESP_LOG_INFO, "Connected to AP, waiting for IP...");
    _this->_connect_in_progress = false;
    _this->_l2_connected_time_ms = _this->millis();
    _this->_is_l2_connected = true;
    xEventGroupSetBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
  } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    wifi_event_sta_disconnected_t *event = static_cast<wifi_event_sta_disconnected_t *>(event_data);
    uint8_t reason = event != nullptr ? event->reason : 0;
    _this->log(ESP_LOG_WARN, "WiFi disconnected (reason: " + std::to_string(reason) + ")");

    _this->_is_l2_connected = false;
    _this->_gateway_responds_to_ping = false;
    _this->_initial_gw_probe_done = false;
    xEventGroupClearBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);

    bool was_connected = _this->_is_connected.exchange(false);
    if (was_connected) {
      memset(&_this->_ip_addr, 0, sizeof(esp_ip4_addr_t));
      memset(&_this->_gw_addr, 0, sizeof(esp_ip4_addr_t));
      auto on_disconnected = _this->_on_disconnected;
      if (on_disconnected != nullptr) {
        on_disconnected();
      }
    }

    bool is_roaming = false;
#ifdef WIFI_REASON_ROAMING
    if (reason == WIFI_REASON_ROAMING) {
      is_roaming = true;
    }
#endif

    if (is_roaming) {
      _this->log(ESP_LOG_INFO, "WiFi roaming to another AP...");
      _this->_connect_in_progress = true;
      _this->_last_connect_attempt_ms = _this->millis();
    } else {
      _this->_connect_in_progress = false;
      _this->_last_disconnect_ms = _this->millis();
      _this->_consecutive_failures++;
      if (_this->_reconnect) {
        xEventGroupSetBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
      }
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
    auto ipaddr = event->ip_info.ip;
    _this->log(ESP_LOG_INFO, "got ip: " + std::to_string(esp_ip4_addr1(&ipaddr)) + "." +
                                 std::to_string(esp_ip4_addr2(&ipaddr)) + "." + std::to_string(esp_ip4_addr3(&ipaddr)) +
                                 "." + std::to_string(esp_ip4_addr4(&ipaddr)));
    memcpy(&_this->_ip_addr, &event->ip_info.ip, sizeof(esp_ip4_addr_t));
    memcpy(&_this->_gw_addr, &event->ip_info.gw, sizeof(esp_ip4_addr_t));

    _this->_connect_in_progress = false;
    _this->_is_l2_connected = true;
    _this->_consecutive_failures = 0;
    _this->_consecutive_gw_failures = 0;
    _this->_gateway_responds_to_ping = false;
    _this->_initial_gw_probe_done = false;
    _this->_last_gateway_check_ms = _this->millis();

    bool was_connected = _this->_is_connected.exchange(true);
    xEventGroupSetBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_WAKE_MONITOR_BIT);

    auto on_connected = _this->_on_connected;
    if (!was_connected && on_connected != nullptr) {
      on_connected();
    }
  } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_LOST_IP) {
    _this->log(ESP_LOG_WARN, "WiFi lost IP address");
    memset(&_this->_ip_addr, 0, sizeof(esp_ip4_addr_t));
    memset(&_this->_gw_addr, 0, sizeof(esp_ip4_addr_t));
    _this->_gateway_responds_to_ping = false;
    _this->_initial_gw_probe_done = false;
    _this->_l2_connected_time_ms = _this->millis();
    xEventGroupClearBits(_this->_wifi_event_group, WIFI_CONNECTED_BIT);

    bool was_connected = _this->_is_connected.exchange(false);
    auto on_disconnected = _this->_on_disconnected;
    if (was_connected && on_disconnected != nullptr) {
      on_disconnected();
    }

    if (_this->_reconnect) {
      xEventGroupSetBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
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
  _reconnect = reconnect;

  std::string new_ssid = ssid != nullptr ? ssid : "";
  std::string new_password = password != nullptr ? password : "";

  if (_wifi_initialized) {
    if (_is_connected && _ssid == new_ssid && _password == new_password) {
      return true;
    }

    if (_ssid != new_ssid || _password != new_password) {
      _ssid = new_ssid;
      _password = new_password;

      wifi_config_t wifi_config = {};
      std::strncpy((char *)wifi_config.sta.ssid, _ssid.c_str(), sizeof(wifi_config.sta.ssid));
      std::strncpy((char *)wifi_config.sta.password, _password.c_str(), sizeof(wifi_config.sta.password));
      wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
      wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
      wifi_config.sta.threshold.rssi = -127;
      wifi_config.sta.pmf_cfg.capable = true;
      wifi_config.sta.pmf_cfg.required = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
      wifi_config.sta.failure_retry_cnt = 3;
#endif

      esp_wifi_disconnect();
      if (!reportOnError(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "failed to set wifi config")) {
        return false;
      }
      _consecutive_failures = 0;
      _connect_in_progress = true;
      _last_connect_attempt_ms = millis();
      esp_wifi_connect();
    } else if (!_is_l2_connected && !_connect_in_progress) {
      _connect_in_progress = true;
      _last_connect_attempt_ms = millis();
      esp_err_t err = esp_wifi_connect();
      if (err != ESP_OK) {
        _connect_in_progress = false;
        _last_disconnect_ms = millis();
        _consecutive_failures++;
      }
    }

    if (_reconnect) {
      startMonitorTask();
    } else {
      stopMonitorTask();
    }
  } else {
    _ssid = new_ssid;
    _password = new_password;

    if (initializeNVS) {
      if (!this->initializeNVS()) {
        return false;
      }
    }

    xEventGroupClearBits(_wifi_event_group,
                         WIFI_CONNECTED_BIT | WIFI_WAKE_MONITOR_BIT | WIFI_STOP_TASK_BIT | WIFI_TASK_STOPPED_BIT);

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

    if (!reportOnError(esp_netif_set_hostname(_netif_sta, _device_hostname.c_str()), "failed to set hostname")) {
      return false;
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    if (!reportOnError(esp_wifi_init(&cfg), "failed to initialize wifi")) {
      return false;
    }

    esp_wifi_set_storage(WIFI_STORAGE_RAM);

    if (!reportOnError(
            esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &eventHandler, this, &_instance_any_id),
            "failed to register event handler for any wifi event")) {
      return false;
    }

    if (!reportOnError(
            esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &eventHandler, this, &_instance_got_ip),
            "failed to register event handler for IP got event")) {
      return false;
    }

    if (!reportOnError(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_LOST_IP, &eventHandler, this,
                                                           &_instance_lost_ip),
                       "failed to register event handler for IP lost event")) {
      return false;
    }

    wifi_config_t wifi_config = {};
    std::strncpy((char *)wifi_config.sta.ssid, _ssid.c_str(), sizeof(wifi_config.sta.ssid));
    std::strncpy((char *)wifi_config.sta.password, _password.c_str(), sizeof(wifi_config.sta.password));
    wifi_config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    wifi_config.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    wifi_config.sta.threshold.rssi = -127;
    wifi_config.sta.pmf_cfg.capable = true;
    wifi_config.sta.pmf_cfg.required = false;
#if ESP_IDF_VERSION >= ESP_IDF_VERSION_VAL(5, 0, 0)
    wifi_config.sta.failure_retry_cnt = 3;
#endif

    if (!reportOnError(esp_wifi_set_mode(WIFI_MODE_STA), "failed to set wifi mode to STA")) {
      return false;
    }
    if (!reportOnError(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), "failed to set wifi config")) {
      return false;
    }

    _consecutive_failures = 0;
    _last_connect_attempt_ms = millis();
    if (!reportOnError(esp_wifi_start(), "failed to start wifi")) {
      return false;
    }
    _wifi_initialized = true;
    log(ESP_LOG_INFO, "wifi_init_sta finished.");

    if (_reconnect) {
      startMonitorTask();
    }
  }

  TickType_t xMaxBlockTime = timeout_ms / portTICK_PERIOD_MS;
  EventBits_t bits = xEventGroupWaitBits(_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdFALSE, xMaxBlockTime);

  /* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
   * happened. */
  if ((bits & WIFI_CONNECTED_BIT) != 0) {
    log(ESP_LOG_INFO, "connected to AP with SSID: " + _ssid);
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
  stopMonitorTask();

  if (_wifi_initialized) {
    esp_wifi_disconnect();
    esp_wifi_stop();

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

    esp_wifi_deinit();
    _wifi_initialized = false;
  }

  if (_netif_sta != nullptr) {
    esp_netif_destroy_default_wifi(_netif_sta);
    _netif_sta = nullptr;
  }

  _is_connected = false;
  _is_l2_connected = false;
  _connect_in_progress = false;
  _gateway_responds_to_ping = false;
  _initial_gw_probe_done = false;
  memset(&_ip_addr, 0, sizeof(esp_ip4_addr_t));
  memset(&_gw_addr, 0, sizeof(esp_ip4_addr_t));
  if (_wifi_event_group != nullptr) {
    xEventGroupClearBits(_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_WAKE_MONITOR_BIT);
  }
}

void WiFiHelper::startMonitorTask() {
  if (_monitor_task_handle != nullptr) {
    return;
  }
  xEventGroupClearBits(_wifi_event_group, WIFI_STOP_TASK_BIT | WIFI_TASK_STOPPED_BIT);
  xTaskCreate(connectionMonitorTask, "wifi_monitor", WIFI_MONITOR_TASK_STACK_SIZE, this, WIFI_MONITOR_TASK_PRIORITY,
              &_monitor_task_handle);
}

void WiFiHelper::stopMonitorTask() {
  if (_monitor_task_handle == nullptr || _wifi_event_group == nullptr) {
    return;
  }
  xEventGroupSetBits(_wifi_event_group, WIFI_STOP_TASK_BIT | WIFI_WAKE_MONITOR_BIT);
  xEventGroupWaitBits(_wifi_event_group, WIFI_TASK_STOPPED_BIT, pdTRUE, pdFALSE, pdMS_TO_TICKS(5000));
  _monitor_task_handle = nullptr;
}

void WiFiHelper::connectionMonitorTask(void *pvParameters) {
  WiFiHelper *_this = static_cast<WiFiHelper *>(pvParameters);

  while (true) {
    EventBits_t current_bits = xEventGroupGetBits(_this->_wifi_event_group);
    if ((current_bits & WIFI_STOP_TASK_BIT) != 0) {
      break;
    }

    if (!_this->_reconnect) {
      xEventGroupWaitBits(_this->_wifi_event_group, WIFI_STOP_TASK_BIT | WIFI_WAKE_MONITOR_BIT, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(1000));
      xEventGroupClearBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
      continue;
    }

    uint32_t now_ms = _this->millis();

    if (!_this->_is_connected) {
      if (_this->_is_l2_connected) {
        // Associated at Layer 2, waiting for DHCP IP assignment
        uint32_t l2_elapsed_ms = now_ms - _this->_l2_connected_time_ms.load();
        if (l2_elapsed_ms >= WIFI_DHCP_TIMEOUT_MS) {
          _this->log(ESP_LOG_WARN, "Connected to AP but DHCP timed out after " + std::to_string(l2_elapsed_ms) +
                                       "ms, forcing reconnect...");
          if (_this->_netif_sta != nullptr) {
            esp_netif_dhcpc_stop(_this->_netif_sta);
            esp_netif_dhcpc_start(_this->_netif_sta);
          }
          _this->_is_l2_connected = false;
          _this->_connect_in_progress = false;
          _this->_last_disconnect_ms = _this->millis();
          _this->_consecutive_failures++;
          esp_wifi_disconnect();
        }
      } else if (_this->_connect_in_progress) {
        // Waiting for esp_wifi_connect() to finish scanning/associating
        uint32_t attempt_elapsed_ms = now_ms - _this->_last_connect_attempt_ms.load();
        if (attempt_elapsed_ms >= WIFI_CONNECT_ATTEMPT_TIMEOUT_MS) {
          _this->log(ESP_LOG_WARN, "Connection attempt timed out without event, resetting state...");
          _this->_connect_in_progress = false;
          _this->_last_disconnect_ms = now_ms;
          _this->_consecutive_failures++;
          esp_wifi_disconnect();
        }
      } else {
        // Disconnected and not currently attempting connection; apply exponential backoff
        uint32_t failures = _this->_consecutive_failures.load();
        if (failures == 0) {
          failures = 1;
          _this->_consecutive_failures = 1;
        }

        uint32_t shift = (failures - 1) < 5 ? (failures - 1) : 5;
        uint32_t backoff_ms = WIFI_BACKOFF_MIN_MS << shift;
        if (backoff_ms > WIFI_BACKOFF_MAX_MS) {
          backoff_ms = WIFI_BACKOFF_MAX_MS;
        }

        uint32_t elapsed_since_disconnect_ms = now_ms - _this->_last_disconnect_ms.load();
        if (elapsed_since_disconnect_ms >= backoff_ms) {
          if (failures % WIFI_DRIVER_RESET_FAILURE_COUNT == 0) {
            _this->log(ESP_LOG_WARN, "Multiple failed reconnect attempts (" + std::to_string(failures) +
                                         "), resetting WiFi driver...");
            esp_wifi_disconnect();
            esp_wifi_stop();
            vTaskDelay(pdMS_TO_TICKS(500));
            _this->_connect_in_progress = true;
            _this->_last_connect_attempt_ms = _this->millis();
            esp_err_t start_err = esp_wifi_start();
            if (start_err != ESP_OK) {
              _this->log(ESP_LOG_ERROR, "esp_wifi_start failed: " + std::string(esp_err_to_name(start_err)));
              _this->_connect_in_progress = false;
              _this->_last_disconnect_ms = _this->millis();
              _this->_consecutive_failures++;
            }
          } else {
            _this->log(ESP_LOG_WARN, "Trying to reconnect (attempt " + std::to_string(failures) + ")...");
            _this->_connect_in_progress = true;
            _this->_last_connect_attempt_ms = now_ms;
            esp_err_t err = esp_wifi_connect();
            if (err != ESP_OK) {
              _this->log(ESP_LOG_WARN, "esp_wifi_connect failed: " + std::string(esp_err_to_name(err)));
              _this->_connect_in_progress = false;
              _this->_last_disconnect_ms = _this->millis();
              _this->_consecutive_failures++;
            }
          }
        }
      }

      xEventGroupWaitBits(_this->_wifi_event_group, WIFI_STOP_TASK_BIT | WIFI_WAKE_MONITOR_BIT, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(500));
      xEventGroupClearBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
    } else {
      // Connected with an IP; periodically verify gateway reachability
      uint32_t check_interval_ms =
          _this->_initial_gw_probe_done ? WIFI_GATEWAY_CHECK_INTERVAL_MS : WIFI_GATEWAY_INITIAL_CHECK_MS;
      uint32_t elapsed_since_check_ms = now_ms - _this->_last_gateway_check_ms.load();

      if (elapsed_since_check_ms >= check_interval_ms) {
        _this->_initial_gw_probe_done = true;
        _this->_last_gateway_check_ms = now_ms;
        if (_this->pingGateway()) {
          _this->_gateway_responds_to_ping = true;
          _this->_consecutive_gw_failures = 0;
        } else if (_this->_gateway_responds_to_ping) {
          uint32_t gw_failures = ++_this->_consecutive_gw_failures;
          _this->log(ESP_LOG_WARN, "Gateway reachability check failed (" + std::to_string(gw_failures) + "/" +
                                       std::to_string(WIFI_GATEWAY_MAX_FAILURES) + ")");
          if (gw_failures >= WIFI_GATEWAY_MAX_FAILURES) {
            _this->log(ESP_LOG_ERROR, "Gateway unreachable, forcing WiFi reconnect...");
            _this->_consecutive_gw_failures = 0;
            _this->_gateway_responds_to_ping = false;
            _this->_initial_gw_probe_done = false;
            _this->_last_disconnect_ms = _this->millis();
            esp_wifi_disconnect();
          }
        }
      }

      xEventGroupWaitBits(_this->_wifi_event_group, WIFI_STOP_TASK_BIT | WIFI_WAKE_MONITOR_BIT, pdFALSE, pdFALSE,
                          pdMS_TO_TICKS(1000));
      xEventGroupClearBits(_this->_wifi_event_group, WIFI_WAKE_MONITOR_BIT);
    }
  }

  _this->_monitor_task_handle = nullptr;
  xEventGroupSetBits(_this->_wifi_event_group, WIFI_TASK_STOPPED_BIT);
  vTaskDelete(NULL);
}

bool WiFiHelper::pingGateway() {
  if (!_is_connected || _gw_addr.addr == 0) {
    return true;
  }

  int sock = socket(AF_INET, SOCK_RAW, IP_PROTO_ICMP);
  if (sock < 0) {
    return true;
  }

  struct timeval timeout = {};
  timeout.tv_sec = 2;
  timeout.tv_usec = 0;
  setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  struct sockaddr_in to = {};
  to.sin_family = AF_INET;
  to.sin_addr.s_addr = _gw_addr.addr;

  struct icmp_echo_hdr echo_hdr = {};
  ICMPH_TYPE_SET(&echo_hdr, ICMP_ECHO);
  ICMPH_CODE_SET(&echo_hdr, 0);
  uint16_t ping_id = static_cast<uint16_t>(reinterpret_cast<uintptr_t>(this) & 0xFFFF);
  uint16_t seqno = ++_ping_seqno;
  echo_hdr.id = htons(ping_id);
  echo_hdr.seqno = htons(seqno);
  echo_hdr.chksum = 0;
  echo_hdr.chksum = inet_chksum(&echo_hdr, sizeof(echo_hdr));

  ssize_t sent = sendto(sock, &echo_hdr, sizeof(echo_hdr), 0, reinterpret_cast<struct sockaddr *>(&to), sizeof(to));
  if (sent != static_cast<ssize_t>(sizeof(echo_hdr))) {
    close(sock);
    return false;
  }

  uint32_t start_ms = millis();
  uint8_t buf[64];
  while ((millis() - start_ms) < 2000) {
    struct sockaddr_in from = {};
    socklen_t fromlen = sizeof(from);
    int len = recvfrom(sock, buf, sizeof(buf), 0, reinterpret_cast<struct sockaddr *>(&from), &fromlen);
    if (len < 0) {
      break;
    }
    if (len >= static_cast<int>(sizeof(struct ip_hdr) + sizeof(struct icmp_echo_hdr))) {
      struct ip_hdr *iphdr = reinterpret_cast<struct ip_hdr *>(buf);
      size_t hlen = IPH_HL_BYTES(iphdr);
      if (len >= static_cast<int>(hlen + sizeof(struct icmp_echo_hdr))) {
        struct icmp_echo_hdr *iecho = reinterpret_cast<struct icmp_echo_hdr *>(buf + hlen);
        if (ICMPH_TYPE(iecho) == ICMP_ER && iecho->id == echo_hdr.id && iecho->seqno == echo_hdr.seqno) {
          close(sock);
          return true;
        }
      }
    }
  }

  close(sock);
  return false;
}

uint32_t WiFiHelper::millis() const { return static_cast<uint32_t>(esp_timer_get_time() / 1000ULL); }

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