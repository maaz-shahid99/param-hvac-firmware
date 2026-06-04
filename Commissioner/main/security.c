#include "mbedtls/md.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "SECURITY";

// Constant-time comparison of two equal-length buffers, so a rejected signature
// doesn't leak how many leading bytes matched via timing.
static bool ct_equal(const char *a, const char *b, size_t n)
{
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) {
        diff |= (unsigned char)(a[i] ^ b[i]);
    }
    return diff == 0;
}

/**
 * @brief Verify "COMMAND|<hex-hmac>" — ENFORCED.
 *
 * The signature is HMAC-SHA256(COMMAND) over SECURE_HMAC_KEY, hex-encoded.
 * Only the app holds the key, so this authenticates security-critical commands
 * (notably `add`, which admits a joiner to the mesh) END TO END — even a
 * compromised Bridge (C3) cannot forge one. Commands with no signature, a
 * malformed signature, or a mismatched signature are REJECTED.
 *
 * On success `*cmd_part` points at the command (the trailing "|<sig>" is cut
 * off in place) and the function returns true.
 */
bool verify_command_signature(char *input_buffer, char **cmd_part)
{
    char *separator = strrchr(input_buffer, '|');
    if (!separator) {
        ESP_LOGW(TAG, "Rejected: command is not signed");
        return false;                       // unsigned -> reject
    }

    *separator = '\0';
    char *received_sig_hex = separator + 1;
    *cmd_part = input_buffer;

    if (strlen(received_sig_hex) != 64) {
        ESP_LOGW(TAG, "Rejected: signature wrong length");
        return false;                       // not a 32-byte hex HMAC -> reject
    }

    uint8_t hmac_output[32];
    const char *key = SECURE_HMAC_KEY;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)input_buffer, strlen(input_buffer));
    mbedtls_md_hmac_finish(&ctx, hmac_output);
    mbedtls_md_free(&ctx);

    char expected_sig_hex[65];
    for (int i = 0; i < 32; i++) {
        sprintf(expected_sig_hex + (i * 2), "%02x", hmac_output[i]);
    }
    expected_sig_hex[64] = '\0';

    if (ct_equal(received_sig_hex, expected_sig_hex, 64)) {
        return true;
    }

    // Don't print the expected signature (would help an attacker); just reject.
    ESP_LOGW(TAG, "Rejected: signature mismatch");
    return false;                           // bad signature -> reject
}
