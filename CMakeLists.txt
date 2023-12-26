FILE(GLOB_RECURSE lib_sources "./espidf/impl/*.*")

idf_component_register(COMPONENT_NAME "ConnectionHelper"
                        SRCS ${lib_sources}
                        INCLUDE_DIRS "./includes/" "./espidf/"
                        EMBED_FILES "./espidf/html/ota.html"
                        REQUIRES nvs_flash esp_http_server bootloader_support app_update)

if(IDF_VERSION_MAJOR LESS 5) # 5+ compiles with c++23.
target_compile_options(${COMPONENT_LIB} PRIVATE -std=gnu++17)
endif()