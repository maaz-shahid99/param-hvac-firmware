#include "commissioner.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/commissioner.h"
#include "openthread/instance.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>

static const char *TAG = "COMMISSIONER";

// Helper: readable state
static const char *commissioner_state_to_str(otCommissionerState state)
{
    switch (state) {
        case OT_COMMISSIONER_STATE_DISABLED: return "DISABLED";
        case OT_COMMISSIONER_STATE_PETITION: return "PETITIONING (Wait for Leader/Network)";
        case OT_COMMISSIONER_STATE_ACTIVE:   return "ACTIVE (Ready for Joiners)";
        default:                             return "UNKNOWN";
    }
}

static void commissioner_state_cb(otCommissionerState state, void *context)
{
    ESP_LOGW(TAG, "COMMISSIONER STATE UPDATE: %s", commissioner_state_to_str(state));

    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        otInstance *instance = esp_openthread_get_instance();
        // Log the credentials we are about to allow
        ESP_LOGI(TAG, "Commissioner is ACTIVE. Auto-enabling joiner: PSKD=J01NME, Timeout=300s");
        otError err = otCommissionerAddJoiner(instance, NULL, "J01NME", 300);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to auto-add joiner! Error: %d", err);
        }
    }
}

static void commissioner_joiner_cb(otCommissionerJoinerEvent event,
                                   const otJoinerInfo *info,
                                   const otExtAddress *joiner_id,
                                   void *context)
{
    char id_str[17] = {0};
    if (joiner_id) {
        for (int i = 0; i < 8; i++) sprintf(id_str + (i * 2), "%02X", joiner_id->m8[i]);
    }

    switch (event) {
        case OT_COMMISSIONER_JOINER_START:
            ESP_LOGW(TAG, "[!] JOIN_REQ: Child %s started handshake", id_str);
            break;
        case OT_COMMISSIONER_JOINER_CONNECTED:
            ESP_LOGI(TAG, "[+] JOIN_CONN: Child %s connected (DTLS Up)", id_str);
            break;
        case OT_COMMISSIONER_JOINER_FINALIZE:
            ESP_LOGI(TAG, "[#] JOIN_FIN: Dataset sent to %s", id_str);
            break;
        case OT_COMMISSIONER_JOINER_END:
            ESP_LOGW(TAG, "[*] JOIN_END: Session closed for %s", id_str);
            break;
        case OT_COMMISSIONER_JOINER_REMOVED:
            ESP_LOGE(TAG, "[-] JOIN_REMOVED: Joiner entry cleared/timed out");
            break;
        default:
            ESP_LOGD(TAG, "Unknown Joiner Event: %d", event);
    }
}

void commissioner_start(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otCommissionerState state = otCommissionerGetState(instance);
    
    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        ESP_LOGI(TAG, "Commissioner already ACTIVE");
        return;
    }

    // Always re-register callbacks to ensure we catch events
    otError err = otCommissionerStart(instance, commissioner_state_cb, commissioner_joiner_cb, NULL);

    if (err == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Commissioner Start: OK");
    } else {
        ESP_LOGE(TAG, "Commissioner Start: FAILED %d", err);
    }
}

void commissioner_stop(void)
{
    otInstance *instance = esp_openthread_get_instance();
    otCommissionerStop(instance);
    ESP_LOGI(TAG, "Commissioner Stopped");
}

bool commissioner_is_active(void)
{
    otInstance *instance = esp_openthread_get_instance();
    return (otCommissionerGetState(instance) == OT_COMMISSIONER_STATE_ACTIVE);
}

void delayed_commissioner_start_task(void *arg)
{
    // Wait for the thread stack to promote this node to Leader
    vTaskDelay(pdMS_TO_TICKS(5000));

    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(2000))) {
        commissioner_start();
        esp_openthread_lock_release();
    } else {
        ESP_LOGE(TAG, "delayed_commissioner_start: could not acquire lock");
    }

    vTaskDelete(NULL);
}