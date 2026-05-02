/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot — WiFi HTTP POST to api.pota.app
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "pota_spot.h"

#include "lvgl/lvgl.h"
#include "params/params.h"
#include "wifi.h"

#include <curl/curl.h>
#include <stdio.h>
#include <string.h>

#define POTA_API_URL    "https://api.pota.app/spot/"
#define POTA_API_ORIGIN "https://pota.app"
#define POTA_SOURCE     "X6100-firmware"
#define HTTP_TIMEOUT_SEC 10L

/* Discard response body — we only care about the HTTP status code */
static size_t discard_response(void *ptr, size_t size, size_t nmemb, void *ud) {
    (void)ptr; (void)ud;
    return size * nmemb;
}

bool pota_spot_wifi(const char *park, int32_t freq_hz,
                    const char *mode, const char *comment) {
    if (wifi_get_status() != WIFI_CONNECTED) {
        LV_LOG_WARN("POTA spot: no WiFi");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        LV_LOG_ERROR("POTA spot: curl init failed");
        return false;
    }

    /* JSON body */
    char body[512];
    snprintf(body, sizeof(body),
        "{\"activator\":\"%s\","
        "\"spotter\":\"%s\","
        "\"frequency\":\"%.1f\","
        "\"reference\":\"%s\","
        "\"mode\":\"%s\","
        "\"source\":\"%s\","
        "\"comments\":\"%s\"}",
        params.callsign.x,
        params.callsign.x,
        freq_hz / 1000.0,
        park,
        mode,
        POTA_SOURCE,
        comment ? comment : "Self-spotted from X6100 firmware");

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "Origin: " POTA_API_ORIGIN);
    headers = curl_slist_append(headers, "Referer: " POTA_API_ORIGIN "/");
    headers = curl_slist_append(headers, "User-Agent: X6100-firmware/1.0");

    curl_easy_setopt(curl, CURLOPT_URL,            POTA_API_URL);
    curl_easy_setopt(curl, CURLOPT_POST,           1L);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS,     body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER,     headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT,        HTTP_TIMEOUT_SEC);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 0L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,  discard_response);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        LV_LOG_WARN("POTA spot: curl error %d (%s)", (int)res, curl_easy_strerror(res));
        return false;
    }
    if (http_code != 200) {
        LV_LOG_WARN("POTA spot: HTTP %ld", http_code);
        return false;
    }

    LV_LOG_USER("POTA spot posted: %s %.1f kHz %s", park, freq_hz / 1000.0, mode);
    return true;
}
