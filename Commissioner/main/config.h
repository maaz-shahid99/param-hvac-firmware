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
