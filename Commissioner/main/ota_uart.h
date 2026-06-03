#pragma once

#include <stddef.h>

/**
 * @brief UART-streamed OTA receiver for the C6 (image delivered by the C3).
 *
 * The C3 (which has Wi-Fi) downloads the C6 firmware and streams it here over
 * the UART link as base64 chunks with stop-and-wait ACKs. The image is written
 * to the inactive OTA partition; on OTA_END it is validated, made bootable, and
 * the C6 reboots.
 */
void ota_uart_begin(size_t size);                 // OTA_BEGIN <size>
void ota_uart_data(int seq, const char *b64);     // OTA_DATA <seq> <base64>
void ota_uart_end(void);                          // OTA_END
void ota_uart_abort(void);                         // OTA_ABORT
