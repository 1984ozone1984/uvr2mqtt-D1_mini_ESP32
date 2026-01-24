/**
 * UVR1611 DL-Bus Reader - WiFi Manager Implementation
 *
 * Manages WiFi connectivity with STA/AP mode switching and mDNS.
 */

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_mac.h"
#include "mdns.h"

#include "wifi_manager.h"
#include "config_store.h"

static const char *TAG = "WIFI-MGR";

// =============================================================================
// Event Group Bits
// =============================================================================
#define WIFI_CONNECTED_BIT  BIT0
#define WIFI_FAIL_BIT       BIT1

// =============================================================================
// State
// =============================================================================
static EventGroupHandle_t wifi_event_group = NULL;
static esp_netif_t *sta_netif = NULL;
static esp_netif_t *ap_netif = NULL;
static bool initialized = false;
static wifi_mode_t current_mode = WIFI_MODE_NULL;
static char current_ip[16] = {0};
static char current_mac[18] = {0};
static char ap_ssid[WIFI_AP_SSID_MAX_LEN] = {0};

// =============================================================================
// Event Handler
// =============================================================================
static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    static int retry_count = 0;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
            case WIFI_EVENT_STA_START:
                ESP_LOGI(TAG, "STA started, connecting...");
                esp_wifi_connect();
                break;

            case WIFI_EVENT_STA_DISCONNECTED:
                if (retry_count < 10) {
                    retry_count++;
                    ESP_LOGI(TAG, "Retrying WiFi connection (%d/10)...", retry_count);
                    esp_wifi_connect();
                } else {
                    ESP_LOGW(TAG, "WiFi connection failed after 10 retries");
                    xEventGroupSetBits(wifi_event_group, WIFI_FAIL_BIT);
                }
                current_ip[0] = '\0';
                break;

            case WIFI_EVENT_AP_START:
                ESP_LOGI(TAG, "AP started: %s", ap_ssid);
                current_mode = WIFI_MODE_AP;
                snprintf(current_ip, sizeof(current_ip), WIFI_AP_IP_ADDR);
                break;

            case WIFI_EVENT_AP_STACONNECTED: {
                wifi_event_ap_staconnected_t *event = (wifi_event_ap_staconnected_t *)event_data;
                ESP_LOGI(TAG, "Station connected to AP, AID=%d", event->aid);
                break;
            }

            case WIFI_EVENT_AP_STADISCONNECTED: {
                wifi_event_ap_stadisconnected_t *event = (wifi_event_ap_stadisconnected_t *)event_data;
                ESP_LOGI(TAG, "Station disconnected from AP, AID=%d", event->aid);
                break;
            }

            default:
                break;
        }
    } else if (event_base == IP_EVENT) {
        if (event_id == IP_EVENT_STA_GOT_IP) {
            ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
            snprintf(current_ip, sizeof(current_ip), IPSTR, IP2STR(&event->ip_info.ip));
            ESP_LOGI(TAG, "Got IP: %s", current_ip);
            current_mode = WIFI_MODE_STA;
            retry_count = 0;
            xEventGroupSetBits(wifi_event_group, WIFI_CONNECTED_BIT);
        }
    }
}

// =============================================================================
// mDNS Setup
// =============================================================================
static esp_err_t start_mdns(void)
{
    const char *hostname = config_get_hostname();

    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mDNS hostname set failed: %s", esp_err_to_name(err));
        return err;
    }

    err = mdns_instance_name_set("UVR1611 MQTT Gateway");
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS instance name set failed: %s", esp_err_to_name(err));
    }

    // Add HTTP service for discovery
    mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);

    ESP_LOGI(TAG, "mDNS started: %s.local", hostname);
    return ESP_OK;
}

// =============================================================================
// STA Mode Setup
// =============================================================================
static esp_err_t start_sta_mode(void)
{
    ESP_LOGI(TAG, "Starting STA mode...");

    // Get credentials from config store
    char ssid_buf[CONFIG_WIFI_SSID_MAX_LEN];
    char pass_buf[CONFIG_WIFI_PASSWORD_MAX_LEN];

    if (config_get_wifi_ssid(ssid_buf, sizeof(ssid_buf)) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to get WiFi SSID");
        return ESP_ERR_NOT_FOUND;
    }

    // Password might be empty for open networks
    if (config_get_wifi_password(pass_buf, sizeof(pass_buf)) != ESP_OK) {
        pass_buf[0] = '\0';
    }

    wifi_config_t wifi_config = {
        .sta = {
            .threshold.authmode = pass_buf[0] ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN,
        },
    };

    strncpy((char *)wifi_config.sta.ssid, ssid_buf, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass_buf, sizeof(wifi_config.sta.password) - 1);

    // Clear sensitive data from stack
    memset(ssid_buf, 0, sizeof(ssid_buf));
    memset(pass_buf, 0, sizeof(pass_buf));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));

    // Clear password from config struct
    memset(wifi_config.sta.password, 0, sizeof(wifi_config.sta.password));

    // Set hostname before starting
    const char *hostname = config_get_hostname();
    esp_netif_set_hostname(sta_netif, hostname);

    // Disable power save for DL-Bus timing
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_ERROR_CHECK(esp_wifi_start());

    return ESP_OK;
}

// =============================================================================
// AP Mode Setup
// =============================================================================
static esp_err_t start_ap_mode(void)
{
    ESP_LOGI(TAG, "Starting AP mode...");

    // Build AP SSID from hostname
    const char *hostname = config_get_hostname();
    snprintf(ap_ssid, sizeof(ap_ssid), "ESP32-%s-SETUP", hostname);

    wifi_config_t wifi_config = {
        .ap = {
            .channel = WIFI_AP_CHANNEL,
            .max_connection = WIFI_AP_MAX_CONNECTIONS,
            .authmode = WIFI_AUTH_OPEN,  // Open network for easy first-time setup
        },
    };

    strncpy((char *)wifi_config.ap.ssid, ap_ssid, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(ap_ssid);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    current_mode = WIFI_MODE_AP;
    snprintf(current_ip, sizeof(current_ip), WIFI_AP_IP_ADDR);

    ESP_LOGI(TAG, "AP mode started: SSID=%s, IP=%s", ap_ssid, current_ip);

    return ESP_OK;
}

// =============================================================================
// Public Functions
// =============================================================================

esp_err_t wifi_manager_init(void)
{
    if (initialized) {
        ESP_LOGW(TAG, "Already initialized");
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Initializing WiFi Manager...");

    // Create event group
    wifi_event_group = xEventGroupCreate();
    if (wifi_event_group == NULL) {
        ESP_LOGE(TAG, "Failed to create event group");
        return ESP_FAIL;
    }

    // Initialize TCP/IP stack
    ESP_ERROR_CHECK(esp_netif_init());

    // Create default event loop
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Create network interfaces
    sta_netif = esp_netif_create_default_wifi_sta();
    ap_netif = esp_netif_create_default_wifi_ap();

    // Initialize WiFi with default config
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    // Register event handlers
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    // Get MAC address
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(current_mac, sizeof(current_mac),
             "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    // Start mDNS
    start_mdns();

    initialized = true;
    ESP_LOGI(TAG, "WiFi Manager initialized (MAC: %s)", current_mac);

    return ESP_OK;
}

esp_err_t wifi_manager_start(void)
{
    if (!initialized) {
        ESP_LOGE(TAG, "Not initialized");
        return ESP_ERR_INVALID_STATE;
    }

    // Check if WiFi credentials are configured
    if (!config_has_wifi_credentials()) {
        ESP_LOGW(TAG, "No WiFi credentials - starting AP mode");
        return start_ap_mode();
    }

    // Try STA mode
    esp_err_t err = start_sta_mode();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "STA mode start failed - starting AP mode");
        return start_ap_mode();
    }

    // Wait for connection with timeout
    uint16_t timeout_s = config_get_wifi_timeout();
    ESP_LOGI(TAG, "Waiting for WiFi connection (timeout: %ds)...", timeout_s);

    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_s * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "WiFi connected in STA mode");
        current_mode = WIFI_MODE_STA;
        return ESP_OK;
    }

    // Connection failed - switch to AP mode
    ESP_LOGW(TAG, "WiFi connection timed out - starting AP mode");
    esp_wifi_stop();
    return start_ap_mode();
}

bool wifi_manager_is_connected(void)
{
    return current_mode == WIFI_MODE_STA && current_ip[0] != '\0';
}

bool wifi_manager_is_ap_mode(void)
{
    return current_mode == WIFI_MODE_AP;
}

wifi_mode_t wifi_manager_get_mode(void)
{
    return current_mode;
}

void wifi_manager_get_ip(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, current_ip, len - 1);
        buf[len - 1] = '\0';
    }
}

void wifi_manager_get_ap_ssid(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, ap_ssid, len - 1);
        buf[len - 1] = '\0';
    }
}

void wifi_manager_get_mac(char *buf, size_t len)
{
    if (buf && len > 0) {
        strncpy(buf, current_mac, len - 1);
        buf[len - 1] = '\0';
    }
}

esp_err_t wifi_manager_reconnect(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    if (!config_has_wifi_credentials()) {
        ESP_LOGW(TAG, "No WiFi credentials configured");
        return ESP_ERR_NOT_FOUND;
    }

    ESP_LOGI(TAG, "Reconnecting...");

    // Stop current WiFi
    esp_wifi_stop();

    // Clear event bits
    xEventGroupClearBits(wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT);

    // Start STA mode
    esp_err_t err = start_sta_mode();
    if (err != ESP_OK) {
        return err;
    }

    // Wait for connection
    uint16_t timeout_s = config_get_wifi_timeout();
    EventBits_t bits = xEventGroupWaitBits(
        wifi_event_group,
        WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
        pdFALSE, pdFALSE,
        pdMS_TO_TICKS(timeout_s * 1000));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Reconnected successfully");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Reconnection failed");
    return ESP_FAIL;
}

esp_err_t wifi_manager_start_ap(void)
{
    if (!initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ESP_LOGI(TAG, "Forcing AP mode...");

    // Stop current WiFi
    esp_wifi_stop();

    return start_ap_mode();
}
