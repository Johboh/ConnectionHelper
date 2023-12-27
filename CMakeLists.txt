FILE(GLOB_RECURSE lib_sources "./espidf/impl/*.*")

if(IDF_VERSION_MAJOR GREATER_EQUAL 5)
set(required_components nvs_flash mbedtls esp_wifi esp_http_server esp_http_client esp-tls bootloader_support app_update esp_partition esp_timer)
else()
set(required_components nvs_flash mbedtls esp_wifi esp_http_server esp_http_client esp-tls bootloader_support app_update)
endif()

idf_component_register(COMPONENT_NAME "ConnectionHelper"
                        SRCS ${lib_sources}
                        INCLUDE_DIRS "./includes/" "./espidf/"
                        EMBED_FILES "./espidf/html/ota.html"
                        REQUIRES ${required_components})

if(IDF_VERSION_MAJOR LESS 5) # 5+ compiles with c++23.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
endif()