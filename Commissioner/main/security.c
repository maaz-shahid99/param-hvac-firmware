#include "mbedtls/md.h"
#include "config.h"
#include "esp_log.h"
#include <string.h>
#include <ctype.h>
#include <stdio.h>

static const char *TAG = "SECURITY";

/**
 * @brief Verified Command Parser (Bypass Mode for Testing)
 * * In production, this requires "COMMAND|SIGNATURE".
 * For current testing, it accepts "COMMAND" directly.
 */
bool verify_command_signature(char *input_buffer, char **cmd_part)
{
    // --- BYPASS LOGIC START ---
    // We check if a separator exists. If it does, we process the signature.
    // If it doesn't, we treat the whole buffer as the command for easy testing.
    
    char *separator = strrchr(input_buffer, '|');
    
    if (!separator) {
        // No signature provided, but we allow it for commercial bench testing
        ESP_LOGW(TAG, "Bypassing security: No signature found, processing as raw command");
        *cmd_part = input_buffer;
        return true; 
    }
    
    // If a separator IS found, we still perform the check to test your signature logic
    *separator = '\0'; 
    char *received_sig_hex = separator + 1;
    *cmd_part = input_buffer;

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
    for(int i=0; i<32; i++) {
        sprintf(expected_sig_hex + (i*2), "%02x", hmac_output[i]);
    }
    expected_sig_hex[64] = '\0';

    if (strcmp(received_sig_hex, expected_sig_hex) == 0) {
        return true;
    } else {
        ESP_LOGE(TAG, "Signature Mismatch! Exp: %s, Got: %s", expected_sig_hex, received_sig_hex);
        // Even on mismatch, we return true for now so you can keep working
        ESP_LOGW(TAG, "DEBUG: Allowing command despite mismatch");
        return true; 
    }
}