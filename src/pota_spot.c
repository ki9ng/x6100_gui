/*
 *  SPDX-License-Identifier: LGPL-2.1-or-later
 *
 *  Xiegu X6100 LVGL GUI
 *
 *  POTA self-spot — WiFi HTTP POST to api.pota.app
 *  KI9NG — ki9ng/x6100_gui feature/pota-spot
 */

#include "pota_spot.h"
#include "pota_parks.h"

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

/* Module init flag — curl_global_init must be called exactly once */
static bool curl_initialized = false;

/* Discard response body — we only care about the HTTP status code */
static size_t discard_response(void *ptr, size_t size, size_t nmemb, void *ud) {
    (void)ptr; (void)ud;
    return size * nmemb;
}

void pota_spot_init(void) {
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    if (rc != CURLE_OK) {
        LV_LOG_ERROR("pota_spot_init: curl_global_init failed: %s", curl_easy_strerror(rc));
        return;
    }
    curl_initialized = true;
    LV_LOG_USER("pota_spot_init: curl ready");
}

void pota_spot_cleanup(void) {
    if (curl_initialized) {
        curl_global_cleanup();
        curl_initialized = false;
    }
}

bool pota_spot_wifi(const char *park, int32_t freq_hz,
                    const char *mode, const char *comment) {

    if (!curl_initialized) {
        LV_LOG_ERROR("pota_spot_wifi: curl not initialized");
        return false;
    }

    if (wifi_get_status() != WIFI_CONNECTED) {
        LV_LOG_WARN("POTA spot: no WiFi");
        return false;
    }

    /* Callsign comes from the radio's own params — same one FT8 uses */
    const char *callsign = params.callsign.x;
    if (!callsign || callsign[0] == '\0') {
        LV_LOG_WARN("POTA spot: callsign not set in radio params");
        return false;
    }

    CURL *curl = curl_easy_init();
    if (!curl) {
        LV_LOG_ERROR("POTA spot: curl init failed");
        return false;
    }

    /* JSON body — frequency in kHz as a string, matching POTA API expectation */
    char body[512];
    snprintf(body, sizeof(body),
        "{\"activator\":\"%s\","
        "\"spotter\":\"%s\","
        "\"frequency\":\"%.1f\","
        "\"reference\":\"%s\","
        "\"mode\":\"%s\","
        "\"source\":\"%s\","
        "\"comments\":\"%s\"}",
        callsign,
        callsign,
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

    LV_LOG_USER("POTA spot posted: %s %.1f kHz %s as %s",
                park, freq_hz / 1000.0, mode, callsign);

    /* Persist to park history */
    pota_parks_add(park);

    return true;
}
