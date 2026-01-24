# UVR1611 MQTT Gateway - Component Registration

FILE(GLOB_RECURSE app_sources ${CMAKE_SOURCE_DIR}/src/*.*)

idf_component_register(
    SRCS ${app_sources}
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_http_server
        mqtt
        json
        mdns
)
