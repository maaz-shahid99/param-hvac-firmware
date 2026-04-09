#include "config.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_openthread.h"
#include "esp_task_wdt.h"

// --- OpenThread Includes ---
#include "openthread/instance.h"
#include "openthread/thread.h"        // Needed for otGetVersionString and Role
#include "openthread/commissioner.h"  // Needed for otCommissionerStart/Stop
#include "openthread/error.h"         // Needed for otError definitions
#include "esp_openthread_lock.h"      // IMPORTANT: Needed for locking mechanism

#include "thread_init.h"
#include "commissioner.h" // CRITICAL: This header must include your wrapper prototype
#include "uart_rx.h"
#include "udp_listener.h"

static const char *TAG = "MAIN";

// --- Network State Monitor ---
static void on_thread_state_changed(void *arg, esp_event_base_t event_base,
                                   int32_t event_id, void *event_data)
{
    if (event_base != OPENTHREAD_EVENT) return;

    otInstance *instance = esp_openthread_get_instance();
    otDeviceRole role = otThreadGetDeviceRole(instance);

    if (event_id == OPENTHREAD_EVENT_ROLE_CHANGED) {
        ESP_LOGW(TAG, "NETWORK ROLE CHANGED: %d", role);
        
        if (role == OT_DEVICE_ROLE_LEADER) {
            if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
                // DEBUG: Print actual radio parameters
                ESP_LOGW(TAG, "--- ACTIVE NETWORK INFO ---");
                ESP_LOGW(TAG, "Channel: %d", otLinkGetChannel(instance));
                ESP_LOGW(TAG, "PAN ID:  0x%04X", otLinkGetPanId(instance));
                
                commissioner_start();
                udp_listener_start();
                esp_openthread_lock_release();
            }
        }
    }
}
// --- NEW: FreeRTOS Task Wrapper for OpenThread ---
static void ot_task_worker(void *arg)
{
    ESP_LOGI(TAG, "OpenThread Task started with %d byte stack", THREAD_TASK_STACK_SIZE);
    
    // This function contains the OpenThread main loop and will block forever.
    thread_init(); 
    
    // We should never reach here, but this cleans up the task just in case.
    vTaskDelete(NULL); 
}

void app_main(void)
{
    // 1. Initialize Watchdog (Optional, enable if production requires strict timeouts)
    // esp_task_wdt_init(10, true);

    // 2. NVS with Recovery
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS corruption detected. Erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Critical NVS Failure. Restarting...");
        esp_restart();
    }

    // 3. Event Loop
    if (esp_event_loop_create_default() != ESP_OK) {
        ESP_LOGE(TAG, "Event Loop Failed. Restarting...");
        esp_restart();
    }

    // 4. Register State Monitor
    ESP_ERROR_CHECK(esp_event_handler_register(OPENTHREAD_EVENT, 
                                               ESP_EVENT_ANY_ID, 
                                               on_thread_state_changed, NULL));

    // 5. Start UART Task (MUST be before thread_init blocks)
    uart_rx_init();

    // 6. Start Thread
    ESP_LOGI(TAG, "Initializing Thread Stack...");
    
    // Note: thread_init() contains the main loop call and will NOT return.
    //thread_init(); 
    // 6. Start Thread
    ESP_LOGI(TAG, "Initializing Thread Stack in dedicated task...");
    
    // NEW: Spawn the dedicated task instead of calling thread_init directly
    xTaskCreate(ot_task_worker, "ot_task", THREAD_TASK_STACK_SIZE, NULL, THREAD_TASK_PRIORITY, NULL);
}