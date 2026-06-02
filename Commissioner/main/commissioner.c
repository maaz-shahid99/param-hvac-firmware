#include "commissioner.h"
#include "joiner_manager.h"
#include "esp_log.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/commissioner.h"
#include "openthread/instance.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "COMMISSIONER";

// --- Pending joiner (applied automatically once the commissioner is ACTIVE) ---
// Set when an `add` arrives while the commissioner session is not active, so we
// can re-petition and add the joiner as soon as the petition succeeds.
static bool     s_pending_valid = false;
static char     s_pending_eui[20];
static char     s_pending_pskd[40];
static uint32_t s_pending_timeout;

// Forward declarations (defined below)
static void commissioner_state_cb(otCommissionerState state, void *context);
static void commissioner_joiner_cb(otCommissionerJoinerEvent event,
                                   const otJoinerInfo *info,
                                   const otExtAddress *joiner_id,
                                   void *context);

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
        ESP_LOGI(TAG, "Commissioner ACTIVE. Awaiting scanned EUI64 entries via 'add' command.");

        // If an `add` arrived while we were re-petitioning, apply it now.
        // This callback runs in the OpenThread task context (lock held), so we
        // use the lock-free add.
        if (s_pending_valid) {
            otError err = joiner_add_locked(s_pending_eui, s_pending_pskd, s_pending_timeout);
            if (err == OT_ERROR_NONE) {
                printf("JOINER_ADDED %s\n", s_pending_eui);
                ESP_LOGI(TAG, "Pending joiner %s applied after re-petition", s_pending_eui);
            } else {
                printf("ERROR ADD_FAILED %d\n", err);
                ESP_LOGE(TAG, "Pending joiner %s failed after re-petition: %d", s_pending_eui, err);
            }
            fflush(stdout);
            s_pending_valid = false;
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

bool commissioner_add_joiner(const char *eui64, const char *pskd, uint32_t timeout)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
        ESP_LOGE(TAG, "add_joiner: could not acquire OpenThread lock");
        printf("ERROR ADD_FAILED %d\n", OT_ERROR_BUSY);
        fflush(stdout);
        return false;
    }

    otInstance *instance = esp_openthread_get_instance();
    otCommissionerState state = otCommissionerGetState(instance);

    // Happy path: commissioner is ACTIVE, add immediately.
    if (state == OT_COMMISSIONER_STATE_ACTIVE) {
        otError err = joiner_add_locked(eui64, pskd, timeout);
        esp_openthread_lock_release();

        if (err == OT_ERROR_NONE) {
            printf("JOINER_ADDED %s\n", eui64);
        } else {
            printf("ERROR ADD_FAILED %d\n", err);
        }
        fflush(stdout);
        return (err == OT_ERROR_NONE);
    }

    // Commissioner not active (session dropped or still petitioning): queue the
    // joiner and (re)petition. It will be applied in commissioner_state_cb()
    // as soon as the commissioner reaches the ACTIVE state.
    strncpy(s_pending_eui,  eui64, sizeof(s_pending_eui)  - 1); s_pending_eui[sizeof(s_pending_eui) - 1]   = '\0';
    strncpy(s_pending_pskd, pskd,  sizeof(s_pending_pskd) - 1); s_pending_pskd[sizeof(s_pending_pskd) - 1] = '\0';
    s_pending_timeout = timeout;
    s_pending_valid   = true;

    if (state == OT_COMMISSIONER_STATE_DISABLED) {
        otError serr = otCommissionerStart(instance, commissioner_state_cb, commissioner_joiner_cb, NULL);
        ESP_LOGW(TAG, "Commissioner was DISABLED — re-petitioning (start=%d). Joiner %s queued.", serr, eui64);
    } else {
        ESP_LOGW(TAG, "Commissioner is PETITIONING — joiner %s queued.", eui64);
    }

    esp_openthread_lock_release();

    // Tell the Bridge/app we're recovering; JOINER_ADDED will follow shortly.
    printf("COMMISSIONER_REPETITIONING\n");
    fflush(stdout);
    return false;
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