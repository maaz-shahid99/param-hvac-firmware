#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "openthread/error.h" // Needed for otError return type

/**
 * @brief Thread-safe request to add a joiner to the network.
 * * Handles parsing of the EUI64 string, locking the OpenThread stack,
 * and calling the commissioner API.
 * * @param eui64_str Hex string of the device EUI64 (e.g., "0011223344556677") or "*" for any.
 * @param pskd The Pre-Shared Key for Device (commissioning credential).
 * @param timeout Seconds to keep the joining window open (usually 120).
 * * @return OT_ERROR_NONE on success, or an error code on failure.
 */
otError joiner_add_request(const char *eui64_str, const char *pskd, uint32_t timeout);