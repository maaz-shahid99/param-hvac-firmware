#include "joiner_role.h"
#include "config.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/joiner.h"
#include "openthread/link.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "JOINER_ROLE";

// Set once the join completes (success) or the role is abandoned (FORM_NET).
// Stops the retry task from re-arming the joiner.
static volatile bool s_stop_retry = false;

// --- Joiner result callback (runs in the OpenThread task context) ---
static void joiner_cb(otError err, void *ctx)
{
    (void)ctx;

    if (err == OT_ERROR_NONE) {
        // Credentials are now stored in NVS. Reboot so we come back up on the
        // "dataset exists -> attach" path and let the stack promote us to Router.
        ESP_LOGI(TAG, "[+] Join SUCCESS — network credentials saved. Rebooting to attach...");
        s_stop_retry = true;
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    } else if (err == OT_ERROR_RESPONSE_TIMEOUT) {  // 23: missed the beacon
        ESP_LOGW(TAG, "[~] Joiner missed router beacon (err 23). Will retry.");
    } else if (err == OT_ERROR_SECURITY) {          // 28: PSKd mismatch / not yet authorized
        ESP_LOGW(TAG, "[!] Joiner rejected: PSKd mismatch or not yet authorized (err 28). Will retry.");
    } else {
        ESP_LOGW(TAG, "[!] Joiner handshake failed (err %d). Will retry.", err);
    }
    // NOTE: We deliberately do NOT latch a permanent failure. A router that is
    // powered before the commissioner has run `add <EUI64> <PSKd>` simply keeps
    // scanning and joins automatically once it is authorized.
}

// --- (Re)start the discovery/handshake. Caller must hold the OT lock. ---
static void start_joiner_locked(otInstance *inst)
{
    otJoinerStop(inst);

    otError err = otJoinerStart(inst,
                                ROUTER_JOIN_PSKD,        // pre-shared device key
                                NULL,                    // provisioning URL
                                ROUTER_VENDOR_NAME,
                                ROUTER_VENDOR_MODEL,
                                ROUTER_VENDOR_SW_VER,
                                NULL,                    // vendor data
                                joiner_cb, NULL);

    if (err != OT_ERROR_NONE) {
        ESP_LOGW(TAG, "otJoinerStart failed: %d", err);
    } else {
        ESP_LOGI(TAG, "Joiner started — scanning channel %d for a network to join...",
                 ROUTER_JOIN_CHANNEL);
    }
}

// --- Retry loop: re-arm the joiner whenever it falls back to IDLE ---
static void router_joiner_task(void *arg)
{
    (void)arg;

    while (!s_stop_retry) {
        if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
            otInstance *inst = esp_openthread_get_instance();

            if (!s_stop_retry && otJoinerGetState(inst) == OT_JOINER_STATE_IDLE) {
                ESP_LOGI(TAG, "Joiner idle — retrying discovery...");
                start_joiner_locked(inst);
            }
            esp_openthread_lock_release();
        }
        vTaskDelay(pdMS_TO_TICKS(ROUTER_JOIN_RETRY_MS));
    }

    vTaskDelete(NULL);
}

void router_joiner_start(void)
{
    otInstance *inst = esp_openthread_get_instance();

    // Router-eligible Full Thread Device, radio always on.
    otLinkModeConfig mode = {
        .mRxOnWhenIdle = 1,
        .mDeviceType   = 1,   // FTD (router-capable) — differs from the SED sensor
        .mNetworkData  = 1,
    };
    otThreadSetLinkMode(inst, mode);

    otIp6SetEnabled(inst, true);
    otLinkSetChannel(inst, ROUTER_JOIN_CHANNEL);
    otLinkSetSupportedChannelMask(inst, (1 << ROUTER_JOIN_CHANNEL));

    // Kick off the first attempt now (we are in the OT task context, pre-mainloop),
    // then hand off re-tries to a background task.
    start_joiner_locked(inst);
    xTaskCreate(router_joiner_task, "rjoin", 4096, NULL, 5, NULL);
}

void router_joiner_stop(void)
{
    s_stop_retry = true;  // tell the retry task to exit
    otJoinerStop(esp_openthread_get_instance());
}
