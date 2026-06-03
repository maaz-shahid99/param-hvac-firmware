#pragma once

// --- Security Configuration ---
#define SECURE_HMAC_KEY "PROD_SECRET_KEY_CHANGE_ME" // Shared with Bridge ESP32
#define SECURE_COMMAND_TIMEOUT_MS 5000              // Max time to acquire lock

// --- System Reliability ---
#define SYSTEM_WATCHDOG_TIMEOUT_SEC 10
#define HEAP_WARNING_THRESHOLD      10240           // Warn if < 10KB free

// --- Logging ---
// #define CONFIG_LOG_CREDENTIALS 1                 // COMMENT OUT FOR PRODUCTION!

// --- Thread Configuration ---
#define THREAD_TASK_STACK_SIZE      8192
#define THREAD_TASK_PRIORITY        5

// --- OTA ---
// C6 firmware version, reported to the C3 so fleet OTA only applies newer images.
// Bump this on every Commissioner build you publish.
#define COMMISSIONER_FW_VERSION     3

// --- Router Joiner (device-side commissioning) ---
// Used when a device boots WITHOUT an operational dataset and is NOT the
// designated network former. It scans for the network and joins via the
// commissioner using this PSKd. The commissioner must authorize this device
// first with `add <EUI64> <PSKd>` using a MATCHING PSKd.
//
// PRODUCTION: this must be UNIQUE per device and provisioned at the factory
// (e.g. written to NVS / printed on the unit's QR label), NOT a shared constant.
#define ROUTER_JOIN_PSKD     "J01NME"   // TODO: provision per-device in production
#define ROUTER_JOIN_CHANNEL  15         // Must match the network the former creates
#define ROUTER_JOIN_RETRY_MS 5000       // Re-arm discovery if the joiner goes idle
#define ROUTER_VENDOR_NAME   "HVAC"
#define ROUTER_VENDOR_MODEL  "Router"
#define ROUTER_VENDOR_SW_VER "1.0.0"
