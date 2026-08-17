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
#include "config_sync.h"
#include "ota_uart.h"
#include "led.h"

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

    // OTA streams thousands of OTA_DATA lines — don't log each one (it would
    // double the UART traffic back to the C3 and slow the transfer).
    if (strncmp(raw_input, "OTA_", 4) != 0)
        ESP_LOGI(TAG, "Processing cmd len: %d", len);

    // --- UART OTA (C3 streams the C6 image). Handle before the copy/strtok. ---
    if (strncmp(raw_input, "OTA_BEGIN ", 10) == 0) {
        ota_uart_begin((size_t)strtoul(raw_input + 10, NULL, 10));
        return;
    }
    if (strncmp(raw_input, "OTA_DATA ", 9) == 0) {
        char *p = raw_input + 9;
        char *sp = strchr(p, ' ');
        if (sp) { *sp = '\0'; ota_uart_data(atoi(p), sp + 1); }
        else { printf("OTA_ERR FORMAT\n"); fflush(stdout); }
        return;
    }
    if (strcmp(raw_input, "OTA_END") == 0)   { ota_uart_end();   return; }
    if (strcmp(raw_input, "OTA_ABORT") == 0) { ota_uart_abort(); return; }

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

    // C3 -> C6: wipe this Commissioner. This arrives ONLY over the local UART
    // from the paired Bridge (which gated it behind the authenticated BLE
    // session, or behind an HMAC-verified mesh RESET). It is therefore a
    // trusted-link command and is intentionally NOT on the signed `add` path.
    if (token && strcmp(token, "factory_reset") == 0) {
        free(cmd_copy);
        led_signal_reset();                  // System LED: fast flutter while we wipe
        vTaskDelay(pdMS_TO_TICKS(500));      // let the flutter show before reboot
        nvs_flash_erase();
        esp_restart();
        return;  // unreachable
    }

    // C3 -> C6: plain restart, NO wipe. Trusted-link command on the same footing
    // as factory_reset: it arrives only over the local UART from the paired
    // Bridge, which gates it behind the authenticated BLE session or an
    // admin-authenticated restart relayed from the cloud.
    //
    // Distinct from factory_reset on purpose — that one erases NVS, so it can
    // never be the answer to "the radio is wedged, restart it". Restarting the
    // C6 drops the mesh for a few seconds and closes any open commissioning
    // window; children re-attach on their own.
    if (token && strcmp(token, "reboot") == 0) {
        free(cmd_copy);
        printf("REBOOTING\n");                // let the C3 log it before the UART dies
        vTaskDelay(pdMS_TO_TICKS(100));
        esp_restart();
        return;  // unreachable
    }

    // C3 -> C6: broadcast a fleet-OTA command (signed) over the mesh.
    // Format: "ota_broadcast <baseurl>"  e.g. "ota_broadcast http://10.14.98.109:8001"
    if (token && strcmp(token, "ota_broadcast") == 0) {
        const char *url = raw_input + strlen("ota_broadcast");
        while (*url == ' ') url++;
        if (*url) config_sync_broadcast_ota(url);
        free(cmd_copy);
        return;
    }

    // C3 -> C6: broadcast a signed fleet factory-reset over the mesh.
    if (token && strcmp(token, "reset_broadcast") == 0) {
        config_sync_broadcast_reset();
        free(cmd_copy);
        return;
    }

    // C3 -> C6: locally-provisioned Wi-Fi/app creds to replicate across the mesh.
    // Format: "cfg_publish <ssid>|<pass>|<zone>|<net>|<pin>"  (fields must not
    // contain '|'; <pin> is "-" to leave the fleet PIN unchanged)
    if (token && strcmp(token, "cfg_publish") == 0) {
        const char *payload = raw_input + strlen("cfg_publish");
        while (*payload == ' ') payload++;

        char tmp[256];
        strncpy(tmp, payload, sizeof(tmp) - 1);
        tmp[sizeof(tmp) - 1] = '\0';

        char *save = NULL;
        char *ssid = strtok_r(tmp,  "|", &save);
        char *pass = strtok_r(NULL, "|", &save);
        char *zone = strtok_r(NULL, "|", &save);
        char *net  = strtok_r(NULL, "|", &save);
        char *pin  = strtok_r(NULL, "|", &save);

        if (ssid && pass && net) {
            config_sync_publish_local(ssid, pass, zone ? zone : "Default", net,
                                      pin ? pin : "-");
            printf("CFG_PUBLISHED\n");
        } else {
            printf("ERROR CFG_PUBLISH_FORMAT\n");
        }
        fflush(stdout);
        free(cmd_copy);
        return;
    }

    // C3 -> C6: a BME environmental sample to relay toward the gateway/cloud.
    // Format: "ENV <t>,<h>,<p>,<voc>"  (config_sync tags it with our own EUI).
    if (token && strcmp(token, "ENV") == 0) {
        const char *payload = raw_input + strlen("ENV");
        while (*payload == ' ') payload++;
        config_sync_send_env(payload);
        free(cmd_copy);
        return;
    }

    // C3 -> C6: a firmware crash report to relay toward the gateway/cloud.
    // Format: "CRASH <reset>|<pc>|<bt>"  (config_sync tags it with our own EUI).
    if (token && strcmp(token, "CRASH") == 0) {
        const char *payload = raw_input + strlen("CRASH");
        while (*payload == ' ') payload++;
        config_sync_send_crash(payload);
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
            uint32_t timeout = timeout_str ? (uint32_t)strtoul(timeout_str, NULL, 10) : 120;
            if (timeout == 0) timeout = 120;

            // The PSKD is a join secret and the raw input embeds it — only log
            // it when sensitive logging is explicitly enabled (see config.h).
#if LOG_SENSITIVE
            ESP_LOGE(TAG, "========= PARSED VALUES =========");
            ESP_LOGE(TAG, "Raw Input: '%s'", raw_debug);
            ESP_LOGE(TAG, "EUI64    : '%s'", id_str);
            ESP_LOGE(TAG, "PSKD     : '%s'", cred);
            ESP_LOGE(TAG, "Timeout  : %lu", (unsigned long)timeout);
            ESP_LOGE(TAG, "=================================");
#else
            ESP_LOGI(TAG, "add: EUI=%s (PSKD redacted), timeout=%lu",
                     id_str, (unsigned long)timeout);
            (void)raw_debug;
#endif

            // Bind this PSKD to the scanned EUI64 only — reject wildcards.
            if (strcmp(id_str, "*") == 0) {
                printf("ERROR ADD_FAILED WILDCARD_NOT_ALLOWED\n");
            } else {
                // Self-healing add: re-petitions the commissioner if its session
                // has dropped, then applies the joiner once ACTIVE. Prints the
                // protocol response itself (JOINER_ADDED / ERROR / REPETITIONING).
                commissioner_add_joiner(id_str, cred, timeout);
            }
        }
    }
    // NOTE: `factory_reset` is handled earlier as a trusted-link command (it
    // arrives unsigned over UART from the local Bridge), not here on the signed
    // path. `add` is the only command that requires an HMAC signature.
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