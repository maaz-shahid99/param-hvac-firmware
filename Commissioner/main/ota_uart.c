#include "ota_uart.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_ota_ops.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <string.h>
#include <stdio.h>

// Receives a firmware image from the C3 over UART (base64 chunks, stop-and-wait)
// and writes it to the inactive OTA partition. Protocol (C3 -> C6, one line each):
//   OTA_BEGIN <size>            -> reply "OTA_READY" or "OTA_ERR <why>"
//   OTA_DATA <seq> <base64>     -> reply "OTA_ACK <seq>" or "OTA_ERR <why>"
//   OTA_END                     -> reply "OTA_DONE" then reboot, or "OTA_ERR <why>"
//   OTA_ABORT                   -> cancel

static const char *TAG = "OTA_UART";

static esp_ota_handle_t      s_handle = 0;
static const esp_partition_t *s_part  = NULL;
static bool   s_active   = false;
static size_t s_written  = 0;
static int    s_next_seq = 0;

static void reply(const char *s) { printf("%s\n", s); fflush(stdout); }

void ota_uart_begin(size_t size)
{
    if (s_active) { esp_ota_abort(s_handle); s_active = false; }

    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) { reply("OTA_ERR NO_PARTITION"); return; }

    esp_err_t err = esp_ota_begin(s_part, size ? size : OTA_SIZE_UNKNOWN, &s_handle);
    if (err != ESP_OK) { printf("OTA_ERR BEGIN %d\n", err); fflush(stdout); return; }

    s_active = true; s_written = 0; s_next_seq = 0;
    ESP_LOGW(TAG, "OTA begin -> %s (size %u)", s_part->label, (unsigned)size);
    reply("OTA_READY");
}

void ota_uart_data(int seq, const char *b64)
{
    if (!s_active)            { reply("OTA_ERR NOT_ACTIVE"); return; }
    if (seq != s_next_seq)    { printf("OTA_ERR SEQ %d\n", s_next_seq); fflush(stdout); return; }

    uint8_t buf[768];
    size_t  olen = 0;
    if (mbedtls_base64_decode(buf, sizeof(buf), &olen,
                              (const unsigned char *)b64, strlen(b64)) != 0) {
        reply("OTA_ERR B64");
        return;
    }

    esp_err_t err = esp_ota_write(s_handle, buf, olen);
    if (err != ESP_OK) {
        printf("OTA_ERR WRITE %d\n", err); fflush(stdout);
        esp_ota_abort(s_handle);
        s_active = false;
        return;
    }

    s_written += olen;
    s_next_seq++;
    printf("OTA_ACK %d\n", seq); fflush(stdout);
}

void ota_uart_end(void)
{
    if (!s_active) { reply("OTA_ERR NOT_ACTIVE"); return; }

    esp_err_t err = esp_ota_end(s_handle);     // validates the image
    s_active = false;
    if (err != ESP_OK) { printf("OTA_ERR END %d\n", err); fflush(stdout); return; }

    err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) { printf("OTA_ERR SETBOOT %d\n", err); fflush(stdout); return; }

    ESP_LOGW(TAG, "OTA complete (%u bytes). Rebooting into %s...",
             (unsigned)s_written, s_part->label);
    reply("OTA_DONE");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
}

void ota_uart_abort(void)
{
    if (s_active) { esp_ota_abort(s_handle); s_active = false; reply("OTA_ABORTED"); }
}
