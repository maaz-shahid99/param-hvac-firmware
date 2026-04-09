#include "joiner_manager.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/commissioner.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>

static const char *TAG = "JOINER_MGR";

// --- Private Helper: Hex String to Byte Array ---
static bool hex_to_bytes(const char *hex_str, uint8_t *bytes, size_t len) {
    if (strlen(hex_str) != len * 2) return false;
    for (size_t i = 0; i < len; i++) {
        char buf[3] = {hex_str[i * 2], hex_str[i * 2 + 1], 0};
        char *endptr;
        bytes[i] = (uint8_t)strtoul(buf, &endptr, 16);
        if (*endptr != 0) return false;
    }
    return true;
}

// --- Public API ---
otError joiner_add_request(const char *eui64_str, const char *pskd, uint32_t timeout)
{
    otExtAddress id;
    otExtAddress *p_id = NULL;
    otError err = OT_ERROR_NONE;

    // 1. Parse EUI64 (if not wildcard)
    if (eui64_str && strcmp(eui64_str, "*") != 0) {
        if (!hex_to_bytes(eui64_str, id.m8, 8)) {
            ESP_LOGE(TAG, "Invalid EUI64 format: %s", eui64_str);
            return OT_ERROR_INVALID_ARGS;
        }
        p_id = &id;
    }

    // 2. Acquire Lock (Critical for OpenThread stability)
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "Failed to acquire OpenThread lock");
        return OT_ERROR_BUSY;
    }

    // 3. Call OpenThread API
    otInstance *instance = esp_openthread_get_instance();
    err = otCommissionerAddJoiner(instance, p_id, pskd, timeout);

    // 4. Release Lock
    esp_openthread_lock_release();

    // 5. Log Result (Internal Log)
    if (err == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Joiner added successfully: %s", eui64_str);
    } else {
        ESP_LOGW(TAG, "Failed to add joiner: %s (%d)", eui64_str, err);
    }

    return err;
}