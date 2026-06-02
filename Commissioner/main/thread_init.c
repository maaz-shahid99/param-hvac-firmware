
#include "thread_init.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_event.h"
#include "esp_vfs_eventfd.h"
#include "openthread/dataset_ftd.h"
#include "nvs_flash.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "esp_openthread_netif_glue.h"
#include "esp_ot_config.h"
#include "openthread/instance.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#include "openthread/dataset.h"
#include "openthread/link.h"
#include "esp_random.h" 
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "commissioner.h"
#include "joiner_role.h"
#include "config_sync.h"

static const char *TAG = "THREAD";

static esp_netif_t *init_openthread_netif(const esp_openthread_platform_config_t *config)
{
    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_OPENTHREAD();
    esp_netif_t *netif = esp_netif_new(&cfg);
    assert(netif);
    ESP_ERROR_CHECK(esp_netif_attach(netif, esp_openthread_netif_glue_init(config)));
    return netif;
}


void form_new_network(const char *network_name) {
    otInstance *instance = esp_openthread_get_instance();
    
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {

        // This device has been designated the network former. If it was
        // scanning as a joiner (no dataset yet), abandon that role first.
        router_joiner_stop();

        // Idempotency guard: if we ALREADY hold a network, do NOT create a new
        // one. otDatasetCreateNewNetwork() mints a fresh random Network Key,
        // which would rotate the key and orphan every already-joined router.
        // Re-assert the existing network instead. (Use 'factory_reset' to
        // intentionally start a brand-new network from scratch.)
        otOperationalDataset existing;
        if (otDatasetGetActive(instance, &existing) == OT_ERROR_NONE) {
            ESP_LOGW(TAG, "Network already exists (PAN 0x%04X) — FORM_NET ignored, key NOT rotated.",
                     existing.mPanId);
            // Already attached and running — do NOT toggle IP6/Thread here, that
            // forces a needless detach/reattach blip (and drops the gateway Wi-Fi).
            // Just acknowledge and make sure a commissioner is running.
            printf("NETWORK_FORMED\n");      // keep the Bridge's provisioning flow happy
            fflush(stdout);
            esp_openthread_lock_release();
            xTaskCreate(delayed_commissioner_start_task, "delay_comm", 3072, NULL, 5, NULL);
            return;
        }

        ESP_LOGI(TAG, "Creating New Network Dataset...");

        otThreadSetEnabled(instance, false);
        otIp6SetEnabled(instance, false);
        
        otOperationalDataset dataset;
        
        // 1. MUST DO THIS: Auto-generate a perfectly legal dataset
        // This fills in the Mesh Local Prefix, PSKc, Security Policy, etc.
        otError err = otDatasetCreateNewNetwork(instance, &dataset);
        if (err != OT_ERROR_NONE) {
            ESP_LOGE(TAG, "Failed to create new network dataset: %d", err);
            esp_openthread_lock_release();
            return;
        }

        // 2. NOW overwrite only the fields you want to customize
        otNetworkName name;
        snprintf(name.m8, sizeof(name), "%s", network_name);
        dataset.mNetworkName = name;
        dataset.mComponents.mIsNetworkNamePresent = true;

        dataset.mPanId = 0x1234; 
        dataset.mComponents.mIsPanIdPresent = true;

        dataset.mChannel = 15; 
        dataset.mComponents.mIsChannelPresent = true;

        // 3. Commit the perfectly valid dataset
        otDatasetSetActive(instance, &dataset);
        
        // 4. Bring Interface Back Up
        otIp6SetEnabled(instance, true);
        otThreadSetEnabled(instance, true);

        printf("NETWORK_FORMED\n");
        fflush(stdout);

        ESP_LOGI(TAG, "Network '%s' configured. Waiting for stack promotion...", network_name);

        // This node just became a network member (the former) -> start
        // credential replication now (we still hold the OT lock here).
        config_sync_init();

        esp_openthread_lock_release();

        xTaskCreate(delayed_commissioner_start_task, "delay_comm", 3072, NULL, 5, NULL);
    }
}

void thread_init(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NVS Init failed!");
        return;
    }

    if (esp_netif_init() != ESP_OK) {
        ESP_LOGE(TAG, "Netif Init failed!");
        return;
    }
    
    ESP_ERROR_CHECK(esp_vfs_eventfd_register(&(esp_vfs_eventfd_config_t){
        .max_fds = 3,
    }));

    esp_openthread_platform_config_t config = {
        .radio_config = ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG(),
        .host_config  = ESP_OPENTHREAD_DEFAULT_HOST_CONFIG(),
        .port_config  = ESP_OPENTHREAD_DEFAULT_PORT_CONFIG(),
    };

    if (esp_openthread_init(&config) != ESP_OK) {
        ESP_LOGE(TAG, "OT Init failed!");
        return;
    }

    esp_netif_t *ot_netif = init_openthread_netif(&config);
    if (!ot_netif) {
        ESP_LOGE(TAG, "Netif creation failed!");
        return;
    }

    otInstance *instance = esp_openthread_get_instance();

    // --- Boot role decision ---------------------------------------------
    // 1. If we already hold network credentials, just attach (this is the
    //    network former rejoining, OR a router that joined previously).
    // 2. Otherwise become a Joiner and wait to be commissioned into a network.
    //    A device explicitly told to FORM_NET will override this and become
    //    the former (see form_new_network()).
    otOperationalDataset dataset;
    if (otDatasetGetActive(instance, &dataset) == OT_ERROR_NONE) {
        ESP_LOGI(TAG, "Existing dataset found (PAN 0x%04X). Attaching to network...",
                 dataset.mPanId);
        otIp6SetEnabled(instance, true);
        otThreadSetEnabled(instance, true);

        // We are already a network member -> start credential replication.
        // IMPORTANT: do NOT start this on the joiner path. While joining, the
        // config-sync task would contend for the OpenThread lock and emit
        // multicasts, disrupting the DTLS commissioning handshake. A joiner
        // reboots after a successful join and re-enters this branch.
        config_sync_init();
    } else {
        ESP_LOGW(TAG, "No dataset found. Starting Joiner (router) — "
                      "scanning for a network to join...");
        router_joiner_start();
    }

    ESP_LOGI(TAG, "Launching Main Loop");
    esp_openthread_launch_mainloop();
}