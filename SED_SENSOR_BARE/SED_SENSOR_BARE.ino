#include <Arduino.h>
#include <OThread.h>
#include "esp_mac.h"
#include <nvs_flash.h>
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/instance.h"
#include "openthread/thread.h"
#include "openthread/joiner.h"
#include "openthread/link.h"
#include "openthread/udp.h"
#include "openthread/ip6.h"
#include "openthread/dataset.h"

// Your secure passphrase
const char *pskd = "J01NME";

// Global state flags
volatile bool g_joined = false;
volatile bool g_failed = false;

// --- JOINER CALLBACK ---
void otaJoinerCallback(otError aError, void *aContext) {
  Serial.printf("\n[JOINER] Callback received with error code: %d\n", aError);

  if (aError == OT_ERROR_NONE) {
    g_joined = true;
    Serial.println("[JOINER] Successfully authenticated! Network keys saved to NVS.");

    Serial.println("[SYSTEM] Rebooting to apply new credentials...");
    delay(500);
    ESP.restart(); // The easiest/safest way to transition from Joiner to normal End Device in ESP32
    
  } else if (aError == 23) {
    // ERROR 23 = Not Found. This is normal wireless packet loss.
    Serial.println("[JOINER] Error 23: Missed the router's beacon. Will retry...");
    g_failed = false; // Do NOT give up!
    
  } else {
    // ERROR 28 (Security) or other fatal errors
    g_failed = true;
    Serial.printf("[JOINER] FATAL: Handshake failed with Error %d. STOPPING.\n", aError);
    if (aError == 28) Serial.println("[DEBUG] Error 28 = Security Rejected. PSKD mismatch.");
  }
}

// --- Helper: start joiner (caller MUST hold OT lock) ---
static void start_joiner_locked(otInstance *inst) {
  otJoinerStop(inst);

  otError err = otJoinerStart(
    inst,
    pskd,
    NULL,
    "MyVendor",
    "MySensor",
    "1.0.0",
    NULL,
    otaJoinerCallback,
    NULL);

  if (err != OT_ERROR_NONE) {
    Serial.printf("[JOINER] WARNING: Joiner failed to initialize! Error: %d\n", err);
  } else {
    Serial.println("[JOINER] Joiner process started. Scanning...");
  }
}

// --- SETUP ---
// void setup() {
//   Serial.begin(115200);
//   delay(2000);

//   nvs_flash_erase();
//   esp_err_t ret = nvs_flash_init();
//   if (ret != ESP_OK) {
//       Serial.printf("[SYSTEM] NVS Init Failed: %s\n", esp_err_to_name(ret));
//       nvs_flash_init(); 
//   } else {
//       Serial.println("[SYSTEM] NVS Partition mounted.");
//   }

//   Serial.println("\n[BOOT] Starting OpenThread SED Device...");
//   OpenThread::begin(false); // Do not auto-start with default PAN
//   delay(500); 

//   if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
//     Serial.println("[FATAL] Could not acquire OT lock in setup!");
//     return;
//   }

//   otInstance *inst = esp_openthread_get_instance();

//   // 2. CHECK FOR EXISTING CREDENTIALS FIRST
//   otOperationalDataset activeDataset;
//   if (otDatasetGetActive(inst, &activeDataset) == OT_ERROR_NONE) {
//     Serial.printf("[SYSTEM] Found existing credentials (PAN: 0x%04X). Connecting...\n", activeDataset.mPanId);
    
//     otLinkModeConfig linkMode = { .mRxOnWhenIdle = 0, .mDeviceType = 0, .mNetworkData = 1 };
//     otThreadSetLinkMode(inst, linkMode);

//     otIp6SetEnabled(inst, true);
//     otThreadSetEnabled(inst, true);
//     g_joined = true;

//   } else {
//     Serial.println("[SYSTEM] No credentials found. Starting Joiner Process...");
    
//     // Read hardware MAC for logging
//     uint8_t hardware_mac[8];
//     if (esp_read_mac(hardware_mac, ESP_MAC_IEEE802154) == ESP_OK) {
//       Serial.print("[HW] Factory EUI-64: ");
//       for (int i = 0; i < 8; i++) Serial.printf("%02x", hardware_mac[i]);
//       Serial.println();
//     }

//     // Keep radio awake during handshake
//     otLinkModeConfig joinMode = { .mRxOnWhenIdle = 1, .mDeviceType = 0, .mNetworkData = 1 };
//     otThreadSetLinkMode(inst, joinMode);

//     otIp6SetEnabled(inst, true);
//     otLinkSetChannel(inst, 15);
//     otLinkSetSupportedChannelMask(inst, (1 << 15));

//     start_joiner_locked(inst); 
//   }

//   esp_openthread_lock_release();
// }

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(2000);

  // 1. ROBUST NVS INITIALIZATION
  // Try to initialize NVS first without erasing
  esp_err_t ret = nvs_flash_init();
  
  // Only erase if there are no free pages or a new version is found
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      Serial.println("[SYSTEM] NVS partition requires formatting. Erasing...");
      nvs_flash_erase();
      ret = nvs_flash_init(); // Retry init after erase
  }
  
  if (ret != ESP_OK) {
      Serial.printf("[SYSTEM] NVS Init Failed: %s\n", esp_err_to_name(ret));
  } else {
      Serial.println("[SYSTEM] NVS Partition mounted successfully.");
  }

  Serial.println("\n[BOOT] Starting OpenThread SED Device...");
  OpenThread::begin(false); // Do not auto-start with default PAN
  delay(500); 

  if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(5000))) {
    Serial.println("[FATAL] Could not acquire OT lock in setup!");
    return;
  }

  otInstance *inst = esp_openthread_get_instance();

  // 2. CHECK FOR EXISTING CREDENTIALS FIRST
  otOperationalDataset activeDataset;
  if (otDatasetGetActive(inst, &activeDataset) == OT_ERROR_NONE) {
    Serial.printf("[SYSTEM] Found existing credentials (PAN: 0x%04X). Connecting...\n", activeDataset.mPanId);
    
    otLinkModeConfig linkMode = { .mRxOnWhenIdle = 0, .mDeviceType = 0, .mNetworkData = 1 };
    otThreadSetLinkMode(inst, linkMode);

    otIp6SetEnabled(inst, true);
    otThreadSetEnabled(inst, true);
    g_joined = true;

  } else {
    Serial.println("[SYSTEM] No credentials found. Starting Joiner Process...");
    
    // Read hardware MAC for logging
    uint8_t hardware_mac[8];
    if (esp_read_mac(hardware_mac, ESP_MAC_IEEE802154) == ESP_OK) {
      Serial.print("[HW] Factory EUI-64: ");
      for (int i = 0; i < 8; i++) Serial.printf("%02x", hardware_mac[i]);
      Serial.println();
    }

    // Keep radio awake during handshake
    otLinkModeConfig joinMode = { .mRxOnWhenIdle = 1, .mDeviceType = 0, .mNetworkData = 1 };
    otThreadSetLinkMode(inst, joinMode);

    otIp6SetEnabled(inst, true);
    otLinkSetChannel(inst, 15);
    otLinkSetSupportedChannelMask(inst, (1 << 15));

    start_joiner_locked(inst); 
  }

  esp_openthread_lock_release();
}


// --- MAIN LOOP ---
void loop() {
  // --- 1. JOINER RADAR & RETRY LOGIC ---
  if (!g_joined && !g_failed) {
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
      otInstance *inst = esp_openthread_get_instance();
      otJoinerState state = otJoinerGetState(inst);

      // Radar every 2 seconds
      static uint32_t radar_timer = 0;
      if (millis() - radar_timer > 2000) {
        radar_timer = millis();
        const otMacCounters *mac = otLinkGetCounters(inst);
        Serial.printf("[JOINER RADAR] State: %d | MAC TX: %lu  RX: %lu | Ch: %d PAN: 0x%04X\n",
                      state, (unsigned long)mac->mTxTotal, (unsigned long)mac->mRxTotal,
                      otLinkGetChannel(inst), otLinkGetPanId(inst));
      }

      // Smart Retry: Only if idle (e.g. Error 23 timeout) and 5 seconds have passed
      static uint32_t retry_timer = 0;
      if (state == OT_JOINER_STATE_IDLE && millis() - retry_timer > 5000) {
        retry_timer = millis();
        Serial.println("[JOINER] Retrying discovery...");
        start_joiner_locked(inst);
      }

      esp_openthread_lock_release();
    }
  }
  // --- 2. FATAL ERROR HALT ---
  else if (g_failed) {
      static bool printed = false;
      if (!printed) {
          Serial.println("[SYSTEM] Join failed permanently. Check Router configuration and reset device.");
          printed = true;
      }
  }

  // --- 3. UDP TX AFTER JOIN ---
  if (g_joined && esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
    otInstance *inst = esp_openthread_get_instance();
    static uint32_t last = 0;
    otDeviceRole currentRole = otThreadGetDeviceRole(inst);

    // Only send data if we are successfully attached to the mesh as a CHILD
    if (currentRole == OT_DEVICE_ROLE_CHILD && millis() - last > 10000) {
      last = millis();

      otMessage *msg = otUdpNewMessage(inst, NULL);
      if (msg != NULL) {
        const char *p = "temp=23";
        otMessageAppend(msg, p, strlen(p));

        otUdpSocket ephemeralSocket;
        memset(&ephemeralSocket, 0, sizeof(ephemeralSocket));
        otUdpOpen(inst, &ephemeralSocket, NULL, NULL);

        otMessageInfo messageInfo;
        memset(&messageInfo, 0, sizeof(messageInfo));
        messageInfo.mPeerPort = 1234;
        
        // Target the Commissioner without hardcoding IP via Realm-Local All-Routers multicast
        otIp6AddressFromString("ff03::2", &messageInfo.mPeerAddr);

        otError sendErr = otUdpSend(inst, &ephemeralSocket, msg, &messageInfo);
        otUdpClose(inst, &ephemeralSocket);

        if (sendErr == OT_ERROR_NONE) {
          Serial.println("[UDP] Packet sent to Router (temp=23)");
        } else {
          Serial.printf("[UDP] Send failed: %d\n", sendErr);
          otMessageFree(msg);
        }
      }
    }
    esp_openthread_lock_release();
  }

  delay(10);
}