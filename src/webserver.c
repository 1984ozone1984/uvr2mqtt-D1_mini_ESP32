/**
 * UVR1611 DL-Bus Reader - Web Configuration Server Implementation
 *
 * HTTP server for status display and configuration.
 */

#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "esp_app_format.h"
#include "cJSON.h"


#include "webserver.h"
#include "wifi_manager.h"
#include "config_store.h"
#include "mqtt_ha.h"
#include "io_config.h"

static const char *TAG = "WEBSERVER";

static httpd_handle_t server = NULL;

// =============================================================================
// HTML Templates
// =============================================================================

static const char HTML_HEADER[] =
    "<!DOCTYPE html><html><head>"
    "<meta charset=\"UTF-8\">"
    "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
    "<title>UVR1611 Gateway</title>"
    "<style>"
    "body{font-family:sans-serif;margin:0;padding:20px;background:#f5f5f5;}"
    ".container{max-width:800px;margin:0 auto;}"
    "h1{color:#333;border-bottom:2px solid #007bff;padding-bottom:10px;}"
    "h2{color:#555;margin-top:30px;}"
    ".card{background:#fff;border-radius:8px;padding:20px;margin:15px 0;box-shadow:0 2px 4px rgba(0,0,0,0.1);}"
    ".grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(200px,1fr));gap:10px;}"
    ".item{padding:10px;background:#f8f9fa;border-radius:4px;}"
    ".item-name{font-weight:bold;color:#333;}"
    ".item-value{color:#007bff;font-size:1.2em;}"
    ".on{color:#28a745;}"
    ".off{color:#dc3545;}"
    "nav{margin-bottom:20px;}"
    "nav a{display:inline-block;padding:10px 20px;background:#007bff;color:#fff;text-decoration:none;border-radius:4px;margin-right:10px;margin-bottom:10px;}"
    "nav a:hover{background:#0056b3;}"
    "nav a.active{background:#0056b3;}"
    "form{margin:0;}"
    "label{display:block;margin:15px 0 5px;font-weight:bold;color:#333;}"
    "input[type=text],input[type=password],input[type=number]{width:100%;padding:10px;border:1px solid #ddd;border-radius:4px;box-sizing:border-box;font-size:1em;}"
    "button{background:#007bff;color:#fff;padding:12px 24px;border:none;border-radius:4px;cursor:pointer;font-size:1em;margin-top:20px;}"
    "button:hover{background:#0056b3;}"
    "button.danger{background:#dc3545;}"
    "button.danger:hover{background:#c82333;}"
    "button.warn{background:#ffc107;color:#212529;}"
    "button.warn:hover{background:#e0a800;}"
    ".status{padding:10px;border-radius:4px;margin:10px 0;}"
    ".status.ok{background:#d4edda;color:#155724;}"
    ".status.warn{background:#fff3cd;color:#856404;}"
    ".status.error{background:#f8d7da;color:#721c24;}"
    ".info{color:#666;font-size:0.9em;margin-top:5px;}"
    "table{width:100%;border-collapse:collapse;}"
    "th,td{padding:8px;text-align:left;border-bottom:1px solid #ddd;}"
    "th{background:#f8f9fa;}"
    "</style></head><body><div class=\"container\">";

static const char HTML_FOOTER[] =
    "</div></body></html>";

static const char HTML_NAV[] =
    "<nav>"
    "<a href=\"/\">Status</a>"
    "<a href=\"/config\">Device Settings</a>"
    "<a href=\"/config/io-names\">I/O Names</a>"
    "</nav>";

// =============================================================================
// Helper Functions
// =============================================================================

static void url_decode(char *dst, const char *src, size_t len)
{
    char a, b;
    size_t i = 0;
    while (*src && i < len - 1) {
        if (*src == '%' && src[1] && src[2]) {
            a = src[1];
            b = src[2];
            a = (a >= 'A') ? ((a & 0xDF) - 'A' + 10) : (a - '0');
            b = (b >= 'A') ? ((b & 0xDF) - 'A' + 10) : (b - '0');
            dst[i++] = (char)(16 * a + b);
            src += 3;
        } else if (*src == '+') {
            dst[i++] = ' ';
            src++;
        } else {
            dst[i++] = *src++;
        }
    }
    dst[i] = '\0';
}

static bool get_form_value(const char *body, const char *key, char *value, size_t value_len)
{
    char search[64];
    snprintf(search, sizeof(search), "%s=", key);

    const char *start = strstr(body, search);
    if (!start) {
        return false;
    }
    start += strlen(search);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    if (len >= value_len) {
        len = value_len - 1;
    }

    char encoded[256];
    if (len >= sizeof(encoded)) {
        len = sizeof(encoded) - 1;
    }
    strncpy(encoded, start, len);
    encoded[len] = '\0';

    url_decode(value, encoded, value_len);
    return true;
}

// =============================================================================
// Status Page Handler
// =============================================================================

static esp_err_t root_handler(httpd_req_t *req)
{
    char *response = malloc(8192);
    if (!response) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *p = response;
    size_t remaining = 8192;
    int n;

    // Header
    n = snprintf(p, remaining, "%s%s<h1>UVR1611 Gateway</h1>", HTML_HEADER, HTML_NAV);
    p += n; remaining -= n;

    // System Info Card
    const system_info_t *sys = mqtt_ha_get_system_info();
    char ip[16], mac[18];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_mac(mac, sizeof(mac));

    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>System Status</h2>"
        "<table>"
        "<tr><th>Hostname</th><td>%s</td></tr>"
        "<tr><th>IP Address</th><td>%s</td></tr>"
        "<tr><th>MAC Address</th><td>%s</td></tr>"
        "<tr><th>WiFi Mode</th><td>%s</td></tr>"
        "<tr><th>MQTT</th><td class=\"%s\">%s</td></tr>"
        "<tr><th>Uptime</th><td>%lu seconds</td></tr>"
        "</table></div>",
        config_get_hostname(),
        ip,
        mac,
        wifi_manager_is_ap_mode() ? "Access Point" : "Station",
        mqtt_ha_is_connected() ? "on" : "off",
        mqtt_ha_is_connected() ? "Connected" : "Disconnected",
        (unsigned long)sys->uptime_seconds);
    p += n; remaining -= n;

    // Sensor Values Card
    float sensor_values[NUM_SENSORS];
    bool sensor_valid[NUM_SENSORS];
    mqtt_ha_get_sensor_values(sensor_values, sensor_valid);

    n = snprintf(p, remaining, "<div class=\"card\"><h2>Sensors</h2><div class=\"grid\">");
    p += n; remaining -= n;

    for (int i = 0; i < NUM_SENSORS; i++) {
        if (strcmp(config_get_sensor_name(i), "---") == 0) {
            continue;
        }

        n = snprintf(p, remaining,
            "<div class=\"item\"><div class=\"item-name\">S%d %s</div>"
            "<div class=\"item-value\">%s</div></div>",
            i + 1, config_get_sensor_name(i),
            sensor_valid[i] ? "" : "---");

        if (sensor_valid[i]) {
            // Overwrite with actual value
            p += strlen("<div class=\"item\"><div class=\"item-name\">");
            p = response + (8192 - remaining);
            n = snprintf(p, remaining,
                "<div class=\"item\"><div class=\"item-name\">S%d %s</div>"
                "<div class=\"item-value\">%.1f</div></div>",
                i + 1, config_get_sensor_name(i), sensor_values[i]);
        }
        p += n; remaining -= n;
    }

    n = snprintf(p, remaining, "</div></div>");
    p += n; remaining -= n;

    // Output States Card
    bool output_states[NUM_OUTPUTS];
    mqtt_ha_get_output_states(output_states);

    n = snprintf(p, remaining, "<div class=\"card\"><h2>Outputs</h2><div class=\"grid\">");
    p += n; remaining -= n;

    for (int i = 0; i < NUM_OUTPUTS; i++) {
        n = snprintf(p, remaining,
            "<div class=\"item\"><div class=\"item-name\">A%d %s</div>"
            "<div class=\"item-value %s\">%s</div></div>",
            i + 1, config_get_output_name(i),
            output_states[i] ? "on" : "off",
            output_states[i] ? "ON" : "OFF");
        p += n; remaining -= n;
    }

    n = snprintf(p, remaining, "</div></div>");
    p += n; remaining -= n;

    // Footer
    snprintf(p, remaining, "%s", HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    free(response);

    return ESP_OK;
}

// =============================================================================
// Configuration Page Handler
// =============================================================================

static esp_err_t config_handler(httpd_req_t *req)
{
    const size_t buf_size = 8192;
    char *response = malloc(buf_size);
    if (!response) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *p = response;
    size_t remaining = buf_size;
    int n;

    // Header
    n = snprintf(p, remaining, "%s%s<h1>Device Settings</h1>", HTML_HEADER, HTML_NAV);
    p += n; remaining -= n;

    // WiFi Configuration Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>WiFi Settings</h2>"
        "<form method=\"POST\" action=\"/config/wifi\">"
        "<label>SSID</label>"
        "<input type=\"text\" name=\"ssid\" maxlength=\"32\" placeholder=\"WiFi network name\">"
        "<label>Password</label>"
        "<input type=\"password\" name=\"password\" maxlength=\"63\" placeholder=\"WiFi password\">"
        "<p class=\"info\">Leave password empty for open networks. Credentials are stored securely and never logged.</p>"
        "<button type=\"submit\">Save WiFi Settings</button>"
        "</form>"
        "</div>");
    p += n; remaining -= n;

    // Device Settings Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Hostname</h2>"
        "<form method=\"POST\" action=\"/config/hostname\">"
        "<label>Hostname</label>"
        "<input type=\"text\" name=\"hostname\" value=\"%s\" maxlength=\"31\">"
        "<p class=\"info\">Device will be accessible at hostname.local via mDNS.</p>"
        "<button type=\"submit\">Save Hostname</button>"
        "</form>"
        "</div>",
        config_get_hostname());
    p += n; remaining -= n;

    // MQTT Settings Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>MQTT Settings</h2>"
        "<form method=\"POST\" action=\"/config/mqtt\">"
        "<label>Broker URI</label>"
        "<input type=\"text\" name=\"broker_uri\" value=\"%s\" maxlength=\"127\">"
        "<p class=\"info\">Format: mqtt://host:port or mqtts://host:port</p>"
        "<label>Base Topic</label>"
        "<input type=\"text\" name=\"base_topic\" value=\"%s\" maxlength=\"63\">"
        "<label>Username (optional)</label>"
        "<input type=\"text\" name=\"username\" maxlength=\"63\" placeholder=\"Leave empty if not required\">"
        "<label>Password (optional)</label>"
        "<input type=\"password\" name=\"mqtt_password\" maxlength=\"63\" placeholder=\"Leave empty if not required\">"
        "<button type=\"submit\">Save MQTT Settings</button>"
        "</form>"
        "</div>",
        config_get_mqtt_broker_uri(),
        config_get_mqtt_base_topic());
    p += n; remaining -= n;

    // OTA Update Card
    const esp_app_desc_t *app_desc = esp_app_get_description();
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Firmware Update</h2>"
        "<p>Current version: <strong>%s</strong> (built %s %s)</p>"
        "<label>Select firmware file (.bin)</label>"
        "<input type=\"file\" id=\"fwfile\" accept=\".bin\">"
        "<p class=\"info\">Upload a new firmware binary. The device will restart automatically after the update.</p>"
        "<div id=\"progress\" style=\"display:none;\"><div class=\"status warn\">Uploading... <span id=\"pct\">0</span>%%</div></div>"
        "<button type=\"button\" class=\"warn\" onclick=\"uploadFW()\">Upload &amp; Update Firmware</button>"
        "<script>"
        "function uploadFW(){"
        "var f=document.getElementById('fwfile').files[0];"
        "if(!f){alert('Please select a firmware file');return;}"
        "if(!f.name.endsWith('.bin')){alert('Please select a .bin file');return;}"
        "document.getElementById('progress').style.display='block';"
        "var xhr=new XMLHttpRequest();"
        "xhr.open('POST','/ota',true);"
        "xhr.setRequestHeader('Content-Type','application/octet-stream');"
        "xhr.upload.onprogress=function(e){if(e.lengthComputable){document.getElementById('pct').textContent=Math.round(e.loaded/e.total*100);}};"
        "xhr.onload=function(){if(xhr.status==200){document.body.innerHTML=xhr.responseText;}else{alert('Update failed: '+xhr.responseText);document.getElementById('progress').style.display='none';}};"
        "xhr.onerror=function(){alert('Upload failed');document.getElementById('progress').style.display='none';};"
        "xhr.send(f);}"
        "</script>"
        "</div>",
        app_desc->version, app_desc->date, app_desc->time);
    p += n; remaining -= n;

    // Reboot Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Device Control</h2>"
        "<form method=\"POST\" action=\"/reboot\">"
        "<p>Restart the device to apply configuration changes.</p>"
        "<button type=\"submit\" class=\"danger\">Reboot Device</button>"
        "</form>"
        "</div>");
    p += n; remaining -= n;

    // Footer
    snprintf(p, remaining, "%s", HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    free(response);

    return ESP_OK;
}

// =============================================================================
// I/O Names Configuration Page Handler
// =============================================================================

static esp_err_t config_io_names_handler(httpd_req_t *req)
{
    const size_t buf_size = 12288;
    char *response = malloc(buf_size);
    if (!response) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    char *p = response;
    size_t remaining = buf_size;
    int n;

    // Header
    n = snprintf(p, remaining, "%s%s<h1>I/O Names Configuration</h1>", HTML_HEADER, HTML_NAV);
    p += n; remaining -= n;

    // Info message
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<p class=\"info\">Use '---' or leave empty to disable an entity. "
        "Disabled entities are not published to MQTT and not shown in Home Assistant.</p>"
        "</div>");
    p += n; remaining -= n;

    // Start single form for all I/O names
    n = snprintf(p, remaining,
        "<form method=\"POST\" action=\"/config/io\">");
    p += n; remaining -= n;

    // Sensor Names Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Sensor Names (S1-S16)</h2>"
        "<div class=\"grid\">");
    p += n; remaining -= n;

    for (int i = 0; i < CONFIG_NUM_SENSORS; i++) {
        n = snprintf(p, remaining,
            "<div class=\"item\">"
            "<label>S%d</label>"
            "<input type=\"text\" name=\"sensor_%d\" value=\"%s\" maxlength=\"23\">"
            "</div>",
            i + 1, i, config_get_sensor_name(i));
        p += n; remaining -= n;
    }

    n = snprintf(p, remaining, "</div></div>");
    p += n; remaining -= n;

    // Output Names Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Output Names (A1-A13)</h2>"
        "<div class=\"grid\">");
    p += n; remaining -= n;

    for (int i = 0; i < CONFIG_NUM_OUTPUTS; i++) {
        n = snprintf(p, remaining,
            "<div class=\"item\">"
            "<label>A%d</label>"
            "<input type=\"text\" name=\"output_%d\" value=\"%s\" maxlength=\"23\">"
            "</div>",
            i + 1, i, config_get_output_name(i));
        p += n; remaining -= n;
    }

    n = snprintf(p, remaining, "</div></div>");
    p += n; remaining -= n;

    // Speed Level Names Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Speed Level Names (Drehzahlstufen)</h2>"
        "<p class=\"info\">Speed levels for outputs A1, A2, A6, A7</p>"
        "<div class=\"grid\">");
    p += n; remaining -= n;

    const char *speed_labels[] = {"A1", "A2", "A6", "A7"};
    for (int i = 0; i < CONFIG_NUM_SPEED_LEVELS; i++) {
        n = snprintf(p, remaining,
            "<div class=\"item\">"
            "<label>Drehz %s</label>"
            "<input type=\"text\" name=\"speed_%d\" value=\"%s\" maxlength=\"23\">"
            "</div>",
            speed_labels[i], i, config_get_speed_name(i));
        p += n; remaining -= n;
    }

    n = snprintf(p, remaining, "</div></div>");
    p += n; remaining -= n;

    // Heat Meter Names Card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<h2>Heat Meter Names (Waermemengenzaehler)</h2>"
        "<p class=\"info\">Power (kW) and Energy (kWh) for each heat meter</p>"
        "<div class=\"grid\">");
    p += n; remaining -= n;

    for (int i = 0; i < CONFIG_NUM_HEAT_METERS; i++) {
        n = snprintf(p, remaining,
            "<div class=\"item\">"
            "<label>WMZ%d Leistung</label>"
            "<input type=\"text\" name=\"hm_pwr_%d\" value=\"%s\" maxlength=\"23\">"
            "</div>"
            "<div class=\"item\">"
            "<label>WMZ%d Energie</label>"
            "<input type=\"text\" name=\"hm_nrg_%d\" value=\"%s\" maxlength=\"23\">"
            "</div>",
            i + 1, i, config_get_heat_meter_power_name(i),
            i + 1, i, config_get_heat_meter_energy_name(i));
        p += n; remaining -= n;
    }

    n = snprintf(p, remaining, "</div></div>");
    p += n; remaining -= n;

    // Save and Reboot buttons card
    n = snprintf(p, remaining,
        "<div class=\"card\">"
        "<button type=\"submit\">Save I/O Names</button>"
        "</form>"
        "<form method=\"POST\" action=\"/reboot\" style=\"display:inline;margin-left:10px;\">"
        "<button type=\"submit\" class=\"danger\">Save &amp; Reboot</button>"
        "</form>"
        "<p class=\"info\" style=\"margin-top:15px;\">Changes take effect immediately for web display. "
        "Reboot required to update MQTT topics and Home Assistant.</p>"
        "</div>");
    p += n; remaining -= n;

    // Footer
    snprintf(p, remaining, "%s", HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    free(response);

    return ESP_OK;
}

// =============================================================================
// Form Submission Handlers
// =============================================================================

static esp_err_t config_response_page(httpd_req_t *req, const char *title,
                                      const char *message, bool success,
                                      const char *back_url)
{
    char response[2560];
    snprintf(response, sizeof(response),
        "%s%s<h1>%s</h1>"
        "<div class=\"card\">"
        "<div class=\"status %s\">%s</div>"
        "<p><a href=\"%s\">Back to Configuration</a></p>"
        "</div>%s",
        HTML_HEADER, HTML_NAV,
        title,
        success ? "ok" : "error",
        message,
        back_url,
        HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);
    return ESP_OK;
}

static esp_err_t config_wifi_handler(httpd_req_t *req)
{
    char body[256];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        return config_response_page(req, "WiFi Configuration", "Failed to receive form data", false, "/config");
    }
    body[ret] = '\0';

    char ssid[CONFIG_WIFI_SSID_MAX_LEN];
    char password[CONFIG_WIFI_PASSWORD_MAX_LEN];

    if (!get_form_value(body, "ssid", ssid, sizeof(ssid))) {
        return config_response_page(req, "WiFi Configuration", "SSID is required", false, "/config");
    }

    if (strlen(ssid) == 0) {
        return config_response_page(req, "WiFi Configuration", "SSID cannot be empty", false, "/config");
    }

    // Password is optional (for open networks)
    if (!get_form_value(body, "password", password, sizeof(password))) {
        password[0] = '\0';
    }

    // Save credentials
    esp_err_t err = config_set_wifi_credentials(ssid, password);

    // Clear sensitive data
    memset(ssid, 0, sizeof(ssid));
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));

    if (err != ESP_OK) {
        return config_response_page(req, "WiFi Configuration", "Failed to save credentials", false, "/config");
    }

    ESP_LOGI(TAG, "WiFi credentials saved");
    return config_response_page(req, "WiFi Configuration",
        "WiFi credentials saved. Reboot to connect with new settings.", true, "/config");
}

static esp_err_t config_hostname_handler(httpd_req_t *req)
{
    char body[128];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        return config_response_page(req, "Hostname Configuration", "Failed to receive form data", false, "/config");
    }
    body[ret] = '\0';

    char hostname[CONFIG_HOSTNAME_MAX_LEN];
    if (!get_form_value(body, "hostname", hostname, sizeof(hostname))) {
        return config_response_page(req, "Hostname Configuration", "Hostname is required", false, "/config");
    }

    if (strlen(hostname) == 0) {
        return config_response_page(req, "Hostname Configuration", "Hostname cannot be empty", false, "/config");
    }

    esp_err_t err = config_set_hostname(hostname);
    if (err != ESP_OK) {
        return config_response_page(req, "Hostname Configuration", "Failed to save hostname", false, "/config");
    }

    ESP_LOGI(TAG, "Hostname saved: %s", hostname);
    return config_response_page(req, "Hostname Configuration",
        "Hostname saved. Reboot to apply changes.", true, "/config");
}

static esp_err_t config_mqtt_handler(httpd_req_t *req)
{
    char body[512];
    int ret = httpd_req_recv(req, body, sizeof(body) - 1);
    if (ret <= 0) {
        return config_response_page(req, "MQTT Configuration", "Failed to receive form data", false, "/config");
    }
    body[ret] = '\0';

    char broker_uri[CONFIG_MQTT_URI_MAX_LEN];
    char base_topic[CONFIG_MQTT_TOPIC_MAX_LEN];
    char username[CONFIG_MQTT_USERNAME_MAX_LEN];
    char password[CONFIG_MQTT_PASSWORD_MAX_LEN];

    if (get_form_value(body, "broker_uri", broker_uri, sizeof(broker_uri)) && strlen(broker_uri) > 0) {
        config_set_mqtt_broker_uri(broker_uri);
    }

    if (get_form_value(body, "base_topic", base_topic, sizeof(base_topic)) && strlen(base_topic) > 0) {
        config_set_mqtt_base_topic(base_topic);
    }

    // Handle MQTT credentials
    bool has_username = get_form_value(body, "username", username, sizeof(username)) && strlen(username) > 0;
    get_form_value(body, "mqtt_password", password, sizeof(password));

    if (has_username) {
        config_set_mqtt_credentials(username, password);
    } else {
        config_clear_mqtt_credentials();
    }

    // Clear sensitive data
    memset(username, 0, sizeof(username));
    memset(password, 0, sizeof(password));
    memset(body, 0, sizeof(body));

    ESP_LOGI(TAG, "MQTT settings saved");
    return config_response_page(req, "MQTT Configuration",
        "MQTT settings saved. Reboot to apply changes.", true, "/config");
}

static esp_err_t config_io_handler(httpd_req_t *req)
{
    // Large buffer needed for all I/O fields (sensors, outputs, speeds, heat meters)
    char *body = malloc(4096);
    if (!body) {
        return config_response_page(req, "I/O Configuration", "Memory allocation failed", false, "/config/io-names");
    }

    int ret = httpd_req_recv(req, body, 4095);
    if (ret <= 0) {
        free(body);
        return config_response_page(req, "I/O Configuration", "Failed to receive form data", false, "/config/io-names");
    }
    body[ret] = '\0';

    char name[CONFIG_IO_NAME_MAX_LEN];
    char key[16];

    // Process sensor names (S1-S16)
    for (int i = 0; i < CONFIG_NUM_SENSORS; i++) {
        snprintf(key, sizeof(key), "sensor_%d", i);
        if (get_form_value(body, key, name, sizeof(name)) && strlen(name) > 0) {
            config_set_sensor_name(i, name);
        }
    }

    // Process output names (A1-A13)
    for (int i = 0; i < CONFIG_NUM_OUTPUTS; i++) {
        snprintf(key, sizeof(key), "output_%d", i);
        if (get_form_value(body, key, name, sizeof(name)) && strlen(name) > 0) {
            config_set_output_name(i, name);
        }
    }

    // Process speed level names (Drehzahlstufen)
    for (int i = 0; i < CONFIG_NUM_SPEED_LEVELS; i++) {
        snprintf(key, sizeof(key), "speed_%d", i);
        if (get_form_value(body, key, name, sizeof(name)) && strlen(name) > 0) {
            config_set_speed_name(i, name);
        }
    }

    // Process heat meter names
    for (int i = 0; i < CONFIG_NUM_HEAT_METERS; i++) {
        snprintf(key, sizeof(key), "hm_pwr_%d", i);
        if (get_form_value(body, key, name, sizeof(name)) && strlen(name) > 0) {
            config_set_heat_meter_power_name(i, name);
        }
        snprintf(key, sizeof(key), "hm_nrg_%d", i);
        if (get_form_value(body, key, name, sizeof(name)) && strlen(name) > 0) {
            config_set_heat_meter_energy_name(i, name);
        }
    }

    free(body);

    ESP_LOGI(TAG, "I/O names saved");
    return config_response_page(req, "I/O Configuration",
        "I/O names saved. Changes take effect immediately for the web interface. "
        "Reboot to update MQTT topics and Home Assistant.", true, "/config/io-names");
}

static esp_err_t reboot_handler(httpd_req_t *req)
{
    char response[2560];
    snprintf(response, sizeof(response),
        "%s%s<h1>Rebooting...</h1>"
        "<div class=\"card\">"
        "<div class=\"status warn\">Device is restarting. Please wait...</div>"
        "<p>You will need to reconnect in a few seconds.</p>"
        "</div>"
        "<script>setTimeout(function(){window.location='/';},10000);</script>"
        "%s",
        HTML_HEADER, HTML_NAV, HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);

    ESP_LOGI(TAG, "Reboot requested via web interface");

    // Delay to allow response to be sent
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

static esp_err_t ota_update_handler(httpd_req_t *req)
{
    esp_ota_handle_t ota_handle = 0;
    const esp_partition_t *update_partition = NULL;
    esp_err_t err;
    char *buf = NULL;
    int received;
    int remaining = req->content_len;
    bool header_checked = false;

    ESP_LOGI(TAG, "OTA update started, size: %d bytes", remaining);

    // Get the next OTA partition to write to
    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "No OTA partition found");
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No OTA partition available");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Writing to partition: %s at offset 0x%lx",
             update_partition->label, (unsigned long)update_partition->address);

    // Allocate buffer for receiving data
    buf = malloc(4096);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Memory allocation failed");
        return ESP_FAIL;
    }

    // Begin OTA update
    err = esp_ota_begin(update_partition, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        free(buf);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_FAIL;
    }

    // Receive and write firmware data
    while (remaining > 0) {
        size_t chunk = remaining < 4096 ? remaining : 4096;
        received = httpd_req_recv(req, buf, chunk);
        if (received <= 0) {
            if (received == HTTPD_SOCK_ERR_TIMEOUT) {
                continue;  // Retry on timeout
            }
            ESP_LOGE(TAG, "File receive failed");
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "File receive failed");
            return ESP_FAIL;
        }

        // Check firmware header on first chunk
        if (!header_checked && received >= sizeof(esp_image_header_t)) {
            esp_image_header_t *header = (esp_image_header_t *)buf;
            if (header->magic != ESP_IMAGE_HEADER_MAGIC) {
                ESP_LOGE(TAG, "Invalid firmware image (bad magic)");
                esp_ota_abort(ota_handle);
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Invalid firmware file");
                return ESP_FAIL;
            }
            header_checked = true;
        }

        // Write chunk to flash
        err = esp_ota_write(ota_handle, buf, received);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
            esp_ota_abort(ota_handle);
            free(buf);
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Flash write failed");
            return ESP_FAIL;
        }

        remaining -= received;
        ESP_LOGD(TAG, "OTA progress: %d bytes remaining", remaining);
    }

    free(buf);

    // Finish OTA update
    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_FAIL;
    }

    // Set the new partition as boot partition
    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "Failed to set boot partition");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "OTA update successful! Rebooting...");

    // Send success response
    char response[2560];
    snprintf(response, sizeof(response),
        "%s%s<h1>Firmware Update Complete</h1>"
        "<div class=\"card\">"
        "<div class=\"status ok\">Firmware uploaded successfully!</div>"
        "<p>Device is restarting with the new firmware...</p>"
        "</div>"
        "<script>setTimeout(function(){window.location='/';},15000);</script>"
        "%s",
        HTML_HEADER, HTML_NAV, HTML_FOOTER);

    httpd_resp_set_type(req, "text/html");
    httpd_resp_send(req, response, HTTPD_RESP_USE_STRLEN);

    // Delay to allow response to be sent, then reboot
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();

    return ESP_OK;
}

// =============================================================================
// API Handlers
// =============================================================================

static esp_err_t api_status_handler(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    // System info
    const system_info_t *sys = mqtt_ha_get_system_info();
    char ip[16], mac[18];
    wifi_manager_get_ip(ip, sizeof(ip));
    wifi_manager_get_mac(mac, sizeof(mac));

    cJSON *system = cJSON_AddObjectToObject(root, "system");
    cJSON_AddStringToObject(system, "hostname", config_get_hostname());
    cJSON_AddStringToObject(system, "ip", ip);
    cJSON_AddStringToObject(system, "mac", mac);
    cJSON_AddStringToObject(system, "wifi_mode", wifi_manager_is_ap_mode() ? "AP" : "STA");
    cJSON_AddBoolToObject(system, "mqtt_connected", mqtt_ha_is_connected());
    cJSON_AddNumberToObject(system, "uptime", sys->uptime_seconds);

    // Sensors
    float sensor_values[NUM_SENSORS];
    bool sensor_valid[NUM_SENSORS];
    mqtt_ha_get_sensor_values(sensor_values, sensor_valid);

    cJSON *sensors = cJSON_AddObjectToObject(root, "sensors");
    for (int i = 0; i < NUM_SENSORS; i++) {
        if (strcmp(config_get_sensor_name(i), "---") != 0 && sensor_valid[i]) {
            cJSON_AddNumberToObject(sensors, config_get_sensor_name(i), sensor_values[i]);
        }
    }

    // Outputs
    bool output_states[NUM_OUTPUTS];
    mqtt_ha_get_output_states(output_states);

    cJSON *outputs = cJSON_AddObjectToObject(root, "outputs");
    for (int i = 0; i < NUM_OUTPUTS; i++) {
        cJSON_AddBoolToObject(outputs, config_get_output_name(i), output_states[i]);
    }

    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);

    if (!json) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, json, HTTPD_RESP_USE_STRLEN);
    free(json);

    return ESP_OK;
}

// =============================================================================
// Public Functions
// =============================================================================

esp_err_t webserver_start(void)
{
    if (server) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 12;
    config.stack_size = 8192;

    ESP_LOGI(TAG, "Starting web server on port %d", config.server_port);

    esp_err_t err = httpd_start(&server, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start server: %s", esp_err_to_name(err));
        return err;
    }

    // Register URI handlers
    httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_handler,
    };
    httpd_register_uri_handler(server, &root_uri);

    httpd_uri_t config_uri = {
        .uri = "/config",
        .method = HTTP_GET,
        .handler = config_handler,
    };
    httpd_register_uri_handler(server, &config_uri);

    httpd_uri_t config_io_names_uri = {
        .uri = "/config/io-names",
        .method = HTTP_GET,
        .handler = config_io_names_handler,
    };
    httpd_register_uri_handler(server, &config_io_names_uri);

    httpd_uri_t config_wifi_uri = {
        .uri = "/config/wifi",
        .method = HTTP_POST,
        .handler = config_wifi_handler,
    };
    httpd_register_uri_handler(server, &config_wifi_uri);

    httpd_uri_t config_hostname_uri = {
        .uri = "/config/hostname",
        .method = HTTP_POST,
        .handler = config_hostname_handler,
    };
    httpd_register_uri_handler(server, &config_hostname_uri);

    httpd_uri_t config_mqtt_uri = {
        .uri = "/config/mqtt",
        .method = HTTP_POST,
        .handler = config_mqtt_handler,
    };
    httpd_register_uri_handler(server, &config_mqtt_uri);

    httpd_uri_t config_io_uri = {
        .uri = "/config/io",
        .method = HTTP_POST,
        .handler = config_io_handler,
    };
    httpd_register_uri_handler(server, &config_io_uri);

    httpd_uri_t reboot_uri = {
        .uri = "/reboot",
        .method = HTTP_POST,
        .handler = reboot_handler,
    };
    httpd_register_uri_handler(server, &reboot_uri);

    httpd_uri_t ota_uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_update_handler,
    };
    httpd_register_uri_handler(server, &ota_uri);

    httpd_uri_t api_status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = api_status_handler,
    };
    httpd_register_uri_handler(server, &api_status_uri);

    ESP_LOGI(TAG, "Web server started");
    return ESP_OK;
}

esp_err_t webserver_stop(void)
{
    if (!server) {
        return ESP_OK;
    }

    esp_err_t err = httpd_stop(server);
    server = NULL;

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Web server stopped");
    }

    return err;
}

bool webserver_is_running(void)
{
    return server != NULL;
}
