#include "ota_service.h"

#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "user_app.h"
#include "wt32_eth.h"

static const char *TAG = "ota_svc";

#define OTA_RECV_BUF 4096
#define OTA_LOG_STEP (64 * 1024)
#define OTA_HEADER_BYTES \
    (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t))

static void field_copy(char *dst, size_t dst_len, const char *src, size_t src_len)
{
    size_t n = strnlen(src, src_len);
    if (n >= dst_len) {
        n = dst_len - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static const char *ota_state_name(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return "factory_or_unknown";
    }
    switch (state) {
    case ESP_OTA_IMG_NEW:
        return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:
        return "pending_verify";
    case ESP_OTA_IMG_VALID:
        return "valid";
    case ESP_OTA_IMG_INVALID:
        return "invalid";
    case ESP_OTA_IMG_ABORTED:
        return "aborted";
    case ESP_OTA_IMG_UNDEFINED:
        return "undefined";
    default:
        return "unknown";
    }
}

static void drain_body(httpd_req_t *req, int unread)
{
    uint8_t junk[512];
    int timeouts = 0;
    while (unread > 0) {
        int want = unread > (int)sizeof(junk) ? (int)sizeof(junk) : unread;
        int n = httpd_req_recv(req, (char *)junk, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                return;
            }
            continue;
        }
        if (n <= 0) {
            return;
        }
        unread -= n;
        timeouts = 0;
    }
}

static esp_err_t send_error(httpd_req_t *req, int unread, const char *status, const char *message)
{
    drain_body(req, unread);
    char body[192];
    snprintf(body, sizeof(body), "{\"error\":\"%s\"}", message);
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    ESP_LOGW(TAG, "%s — %s", status, message);
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static bool token_matches(const char *provided)
{
    const char *expect = CONFIG_WT32_OTA_TOKEN;
    size_t expect_len = strlen(expect);
    if (expect_len == 0) {
        return true;
    }
    size_t provided_len = strlen(provided);
    size_t n = expect_len > provided_len ? expect_len : provided_len;
    uint8_t diff = (uint8_t)(expect_len != provided_len);
    for (size_t i = 0; i < n; i++) {
        uint8_t a = i < expect_len ? (uint8_t)expect[i] : 0;
        uint8_t b = i < provided_len ? (uint8_t)provided[i] : 0;
        diff |= (uint8_t)(a ^ b);
    }
    return diff == 0;
}

static bool ota_authorized(httpd_req_t *req)
{
    if (strlen(CONFIG_WT32_OTA_TOKEN) == 0) {
        return true;
    }

    char token[128];
    esp_err_t err = httpd_req_get_hdr_value_str(req, "X-OTA-Token", token, sizeof(token));
    if (err != ESP_OK) {
        return false;
    }
    return token_matches(token);
}

static bool running_image_is_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (running == NULL || esp_ota_get_state_partition(running, &state) != ESP_OK) {
        return false;
    }
    return state == ESP_OTA_IMG_PENDING_VERIFY;
}

static esp_err_t validate_header(const uint8_t *raw,
                                 size_t image_len,
                                 const esp_partition_t *slot,
                                 char *version_out,
                                 size_t version_len)
{
    if (image_len < OTA_HEADER_BYTES || image_len > slot->size) {
        ESP_LOGE(TAG, "image length %u does not fit slot %s (%u bytes)",
                 (unsigned)image_len, slot->label, (unsigned)slot->size);
        return ESP_ERR_INVALID_SIZE;
    }
    if (raw[0] != ESP_IMAGE_HEADER_MAGIC) {
        ESP_LOGE(TAG, "image magic is 0x%02x, expected 0xE9", raw[0]);
        return ESP_ERR_INVALID_ARG;
    }

    esp_app_desc_t incoming;
    memcpy(&incoming,
           raw + sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t),
           sizeof(incoming));
    if (incoming.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "application description magic is 0x%08lx",
                 (unsigned long)incoming.magic_word);
        return ESP_ERR_INVALID_ARG;
    }

    const esp_app_desc_t *running = esp_app_get_description();
    if (strncmp(incoming.project_name, running->project_name, sizeof(incoming.project_name)) != 0) {
        char got[33];
        field_copy(got, sizeof(got), incoming.project_name, sizeof(incoming.project_name));
        ESP_LOGE(TAG, "project name \"%s\" does not match running project \"%s\"",
                 got, running->project_name);
        return ESP_ERR_INVALID_VERSION;
    }

    const esp_partition_t *invalid = esp_ota_get_last_invalid_partition();
    if (invalid != NULL) {
        esp_app_desc_t bad;
        if (esp_ota_get_partition_description(invalid, &bad) == ESP_OK &&
            strncmp(bad.version, incoming.version, sizeof(incoming.version)) == 0) {
            ESP_LOGE(TAG, "version matches the last image that failed confirmation");
            return ESP_ERR_INVALID_VERSION;
        }
    }

    field_copy(version_out, version_len, incoming.version, sizeof(incoming.version));
    ESP_LOGI(TAG, "image header accepted, version %s, slot %s", version_out, slot->label);
    return ESP_OK;
}

static esp_err_t info_get_handler(httpd_req_t *req)
{
    const esp_app_desc_t *app = esp_app_get_description();
    const esp_partition_t *running = esp_ota_get_running_partition();
    char ip[16];
    wt32_eth_get_ip_str(ip, sizeof(ip));

    char body[512];
    snprintf(body,
             sizeof(body),
             "{\"project\":\"%s\",\"version\":\"%s\",\"idf_version\":\"%s\","
             "\"partition\":\"%s\",\"ota_state\":\"%s\",\"ip\":\"%s\",\"addr_mode\":\"%s\"}",
             app->project_name,
             app->version,
             app->idf_ver,
             running != NULL ? running->label : "unknown",
             ota_state_name(),
             ip,
             wt32_eth_addr_mode());

    httpd_resp_set_type(req, "application/json");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t ota_post_handler(httpd_req_t *req)
{
    if (!ota_authorized(req)) {
        return send_error(req, req->content_len, "401 Unauthorized", "missing or wrong X-OTA-Token");
    }
    if (running_image_is_pending()) {
        return send_error(req, req->content_len, "409 Conflict", "running image is still pending verification");
    }
    if (req->content_len <= 0) {
        return send_error(req, 0, "400 Bad Request", "Content-Length is required");
    }

    const esp_partition_t *slot = esp_ota_get_next_update_partition(NULL);
    if (slot == NULL) {
        return send_error(req, req->content_len, "500 Internal Server Error", "no inactive OTA slot");
    }
    if ((size_t)req->content_len > slot->size) {
        return send_error(req, req->content_len, "400 Bad Request", "image is larger than the OTA slot");
    }

    ESP_LOGI(TAG, "POST /ota  %d bytes -> %s", req->content_len, slot->label);

    esp_ota_handle_t ota = 0;
    esp_err_t err = esp_ota_begin(slot, (size_t)req->content_len, &ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return send_error(req, req->content_len, "500 Internal Server Error", "esp_ota_begin failed");
    }

    uint8_t buf[OTA_RECV_BUF];
    uint8_t header[OTA_HEADER_BYTES];
    size_t header_got = 0;
    bool header_ok = false;
    char new_version[32] = {0};
    int remaining = req->content_len;
    int received = 0;
    int next_log = OTA_LOG_STEP;
    int timeouts = 0;

    while (remaining > 0) {
        int want = remaining > (int)sizeof(buf) ? (int)sizeof(buf) : remaining;
        int n = httpd_req_recv(req, (char *)buf, want);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            if (++timeouts > 5) {
                esp_ota_abort(ota);
                return send_error(req, remaining, "408 Request Timeout", "timed out while reading the image");
            }
            continue;
        }
        timeouts = 0;
        if (n <= 0) {
            esp_ota_abort(ota);
            return send_error(req, 0, "400 Bad Request", "connection closed before the image finished");
        }

        if (!header_ok) {
            size_t need = sizeof(header) - header_got;
            size_t take = (size_t)n < need ? (size_t)n : need;
            memcpy(header + header_got, buf, take);
            header_got += take;
            if (header_got == sizeof(header)) {
                err = validate_header(header, (size_t)req->content_len, slot, new_version, sizeof(new_version));
                if (err != ESP_OK) {
                    esp_ota_abort(ota);
                    return send_error(req, remaining - n, "400 Bad Request", "image header rejected");
                }
                header_ok = true;
            }
        }

        err = esp_ota_write(ota, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write: %s", esp_err_to_name(err));
            esp_ota_abort(ota);
            return send_error(req, remaining - n, "500 Internal Server Error", "esp_ota_write failed");
        }

        remaining -= n;
        received += n;
        if (received >= next_log || remaining == 0) {
            ESP_LOGI(TAG, "wrote %d / %d bytes", received, req->content_len);
            next_log += OTA_LOG_STEP;
        }
    }

    if (!header_ok) {
        esp_ota_abort(ota);
        return send_error(req, 0, "400 Bad Request", "image is smaller than an application header");
    }

    err = esp_ota_end(ota);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end: %s", esp_err_to_name(err));
        return send_error(req, 0, "400 Bad Request", "image validation failed");
    }

    err = esp_ota_set_boot_partition(slot);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition: %s", esp_err_to_name(err));
        return send_error(req, 0, "500 Internal Server Error", "failed to set the boot partition");
    }

    char body[160];
    snprintf(body,
             sizeof(body),
             "{\"status\":\"ok\",\"version\":\"%s\",\"partition\":\"%s\"}",
             new_version,
             slot->label);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
    ESP_LOGI(TAG, "slot %s selected; rebooting into %s", slot->label, new_version);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_service_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CONFIG_WT32_OTA_PORT;
    cfg.stack_size = 10240;
    cfg.recv_wait_timeout = 30;
    cfg.send_wait_timeout = 10;
    cfg.lru_purge_enable = true;
    cfg.max_uri_handlers = 4;

    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "httpd_start: %s", esp_err_to_name(err));
        return err;
    }

    const httpd_uri_t info = {
        .uri = "/info",
        .method = HTTP_GET,
        .handler = info_get_handler,
    };
    const httpd_uri_t ota = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post_handler,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &info));
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &ota));

    char ip[16];
    wt32_eth_get_ip_str(ip, sizeof(ip));
    ESP_LOGI(TAG, "listening on http://%s:%d  (GET /info, POST /ota)", ip, CONFIG_WT32_OTA_PORT);
    if (strlen(CONFIG_WT32_OTA_TOKEN) == 0) {
        ESP_LOGW(TAG, "OTA token is empty; POST /ota is not authenticated");
    } else {
        ESP_LOGI(TAG, "POST /ota requires X-OTA-Token");
    }
    return ESP_OK;
}

esp_err_t ota_service_confirm_image(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (running == NULL) {
        ESP_LOGE(TAG, "no running partition");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "running partition \"%s\" at 0x%08lx",
             running->label, (unsigned long)running->address);

    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) {
        ESP_LOGI(TAG, "no OTA state recorded; this boot does not need confirmation");
        return ESP_OK;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGI(TAG, "OTA state is not pending verify");
        return ESP_OK;
    }

    ESP_LOGW(TAG, "first boot after OTA; running diagnostics before this image is trusted");
    if (!wt32_eth_driver_ready()) {
        ESP_LOGE(TAG, "Ethernet driver did not start; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return ESP_FAIL;
    }

    if (wt32_eth_link_is_up() && !wt32_eth_has_ip()) {
        ESP_LOGI(TAG, "link is up and no address is installed yet; waiting %d s",
                 CONFIG_WT32_DIAG_TIMEOUT_S);
        EventGroupHandle_t events = wt32_eth_events();
        EventBits_t bits = 0;
        if (events != NULL) {
            bits = xEventGroupWaitBits(events,
                                        WT32_IP_READY_BIT,
                                        pdFALSE,
                                        pdTRUE,
                                        pdMS_TO_TICKS(CONFIG_WT32_DIAG_TIMEOUT_S * 1000));
        }
        if ((bits & WT32_IP_READY_BIT) == 0) {
            ESP_LOGE(TAG, "link stayed up without an IP; rolling back");
            esp_ota_mark_app_invalid_rollback_and_reboot();
            return ESP_FAIL;
        }
    } else if (!wt32_eth_link_is_up()) {
        ESP_LOGW(TAG, "link is down; an unplugged cable does not fail this image");
    }

    if (!user_app_diagnostics()) {
        ESP_LOGE(TAG, "user_app_diagnostics rejected this image; rolling back");
        esp_ota_mark_app_invalid_rollback_and_reboot();
        return ESP_FAIL;
    }

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_mark_app_valid_cancel_rollback: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "diagnostics passed; this image is marked valid");
    return ESP_OK;
}
