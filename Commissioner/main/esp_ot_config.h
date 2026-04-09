#pragma once

#include "esp_openthread_types.h"

/**
 * @brief Default OpenThread radio configuration for ESP32-C6
 * 
 * Uses native 802.15.4 radio
 */
#define ESP_OPENTHREAD_DEFAULT_RADIO_CONFIG()                                      \
    {                                                                              \
        .radio_mode = RADIO_MODE_NATIVE,                                          \
    }

/**
 * @brief Default OpenThread host configuration
 * 
 * No host connection (standalone Thread device)
 */
#define ESP_OPENTHREAD_DEFAULT_HOST_CONFIG()                                       \
    {                                                                              \
        .host_connection_mode = HOST_CONNECTION_MODE_NONE,                        \
    }

/**
 * @brief Default OpenThread port configuration
 * 
 * Configures NVS storage and queue sizes
 */
#define ESP_OPENTHREAD_DEFAULT_PORT_CONFIG()                                       \
    {                                                                              \
        .storage_partition_name = "nvs",                                          \
        .netif_queue_size = 10,                                                   \
        .task_queue_size = 10,                                                    \
    }
