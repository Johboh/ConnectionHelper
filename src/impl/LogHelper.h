#pragma once

#ifndef __LOG_HELPER_H__
#define __LOG_HELPER_H__

#include <cstdint>
#include <esp_log.h>

namespace LogHelper {
static inline void log(const char *tag, const esp_log_level_t log_level, std::string &message) {
  va_list args;
  switch (log_level) {
  case ESP_LOG_ERROR:
    ESP_LOGE(tag, "%s", message.c_str());
    break;
  case ESP_LOG_WARN:
    ESP_LOGW(tag, "%s", message.c_str());
    break;
  case ESP_LOG_INFO:
    ESP_LOGI(tag, "%s", message.c_str());
    break;
  case ESP_LOG_VERBOSE:
    ESP_LOGV(tag, "%s", message.c_str());
    break;
  case ESP_LOG_DEBUG:
    ESP_LOGD(tag, "%s", message.c_str());
    break;
  }
}
} // namespace LogHelper

#endif // __LOG_HELPER_H__