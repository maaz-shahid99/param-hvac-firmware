#include "uart_rx.h"
#include "config.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_task_wdt.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "nvs_flash.h"
#include "openthread/commissioner.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include "thread_init.h"
#include "commissioner.h"
#include "joiner_manager.h"

// Forward declaration for security check
bool verify_command_signature(char *input_buffer, char **cmd_part);

static const char *TAG = "UART_RX";

#define UART_PORT_NUM UART_NUM_0
#define UART_RX_BUF_SIZE 1024

// --- Command Processor ---
static void process_command(char *raw_input) {
    // Strip trailing whitespace/CR/LF
    size_t len = strlen(raw_input);
    while (len > 0 && (raw_input[len - 1] == '\r' || raw_input[len - 1] == '\n' || raw_input[len - 1] == ' ')) {
        raw_input[--len] = '\0';
    }

    if (strlen(raw_input) == 0) return;

    ESP_LOGI(TAG, "Processing cmd len: %d", len);

    char *cmd_str = NULL;
    char *cmd_copy = strdup(raw_input);
    if (!cmd_copy) {
        ESP_LOGE(TAG, "OOM");
        return;
    }
    
    // 1. Check for UNSIGNED Internal Commands
    char *token = strtok(cmd_copy, " ");
    
    if (token && strcmp(token, "commissioner_start") == 0) {
        if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
            commissioner_start();  // Use wrapper to register callbacks + auto-add joiner
            printf("COMMISSIONER_STARTED\n");
            esp_openthread_lock_release();
        }
        free(cmd_copy);
        return;
    }

    if (token && strcmp(token, "commissioner_stop") == 0) {
        if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
            otCommissionerStop(esp_openthread_get_instance());
            printf("COMMISSIONER_STOPPED\n");
            esp_openthread_lock_release();
        }
        free(cmd_copy);
        return;
    }

    if (token && strcmp(token, "FORM_NET") == 0) {
        char *net_name = strtok(NULL, " ");
        if (net_name) {
            form_new_network(net_name);
        }
        free(cmd_copy);
        return;
    }
    free(cmd_copy); 

    // 2. SIGNED Commands
    // Keep a copy of raw_input for debugging before verify modifies it
    char raw_debug[256];
    strncpy(raw_debug, raw_input, sizeof(raw_debug) - 1);
    raw_debug[sizeof(raw_debug) - 1] = '\0';

    if (!verify_command_signature(raw_input, &cmd_str)) {
        ESP_LOGW(TAG, "Security: Rejected (Invalid Sig)");
        printf("ERROR SIG_INVALID\n");
        return;
    }

    // Parse the payload (cmd_str is now the part BEFORE the | )
    token = strtok(cmd_str, " ");
    if (!token) return;

    if (strcmp(token, "add") == 0) {
        char *id_str = strtok(NULL, " ");
        char *cred = strtok(NULL, " ");
        char *timeout_str = strtok(NULL, " "); // Extract timeout if provided
        
        if (id_str && cred) {
            // ---> THE DEBUG PRINTS <---
            ESP_LOGE(TAG, "========= PARSED VALUES =========");
            ESP_LOGE(TAG, "Raw Input: '%s'", raw_debug);
            ESP_LOGE(TAG, "EUI64    : '%s'", id_str);
            ESP_LOGE(TAG, "PSKD     : '%s'", cred);
            if (timeout_str) {
                ESP_LOGE(TAG, "Timeout  : '%s'", timeout_str);
            }
            ESP_LOGE(TAG, "=================================");

            // REFACTORED: Pass logic to joiner_manager
            otError err = joiner_add_request("*", cred, 120);

            if (err == OT_ERROR_NONE) {
                // Bridge expects this exact string
                printf("JOINER_ADDED %s\n", id_str); 
            } else {
                printf("ERROR ADD_FAILED %d\n", err);
            }
        }
    } 
    else if (strcmp(token, "factory_reset") == 0) {
        nvs_flash_erase();
        esp_restart();
    }
}

// --- UART Task (Unchanged Buffer Logic) ---
static void uart_rx_task(void *arg) {
    static uint8_t line_buffer[UART_RX_BUF_SIZE];
    static int line_pos = 0;
    uint8_t *chunk = (uint8_t *) malloc(128); 
    
    while (1) {
        int len = uart_read_bytes(UART_PORT_NUM, chunk, 127, pdMS_TO_TICKS(50));
        
        if (len > 0) {
            for (int i = 0; i < len; i++) {
                uint8_t c = chunk[i];
                if (line_pos >= UART_RX_BUF_SIZE - 1) line_pos = 0; // Overflow reset

                if (c == '\n') {
                    line_buffer[line_pos] = '\0';
                    if (line_pos > 0) process_command((char *)line_buffer);
                    line_pos = 0; 
                } else if (c != '\r') {
                    line_buffer[line_pos++] = c;
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10)); 
    }
    free(chunk);
}

void uart_rx_init(void) {
    if (uart_is_driver_installed(UART_PORT_NUM)) {
        ESP_LOGI(TAG, "UART Driver already installed (Console). Using existing.");
    } else {
        uart_config_t uart_config = {
            .baud_rate = 115200,
            .data_bits = UART_DATA_8_BITS,
            .parity = UART_PARITY_DISABLE,
            .stop_bits = UART_STOP_BITS_1,
            .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
            .source_clk = UART_SCLK_DEFAULT,
        };
        ESP_ERROR_CHECK(uart_param_config(UART_PORT_NUM, &uart_config));
        ESP_ERROR_CHECK(uart_driver_install(UART_PORT_NUM, UART_RX_BUF_SIZE * 2, 0, 0, NULL, 0));
    }
    xTaskCreate(uart_rx_task, "uart_rx", 4096, NULL, 5, NULL);
}