#include <Arduino.h>
#include <OThread.h>
#include "esp_mac.h"
#include <nvs_flash.h>
#include <nvs.h>
#include "esp_openthread.h"
#include "esp_openthread_lock.h"

#include "openthread/instance.h"
#include "openthread/thread.h"
#include "openthread/joiner.h"
#include "openthread/link.h"
#include "openthread/udp.h"
#include "openthread/ip6.h"
#include "openthread/dataset.h"

// --- NEW: Sensor Libraries & Config ---
#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS D4     // Data wire is plugged into D4
#define NUM_SENSORS 8      // (legacy) nominal DS18B20 count
#define MAX_PROBES  10     // hard cap on probes reported per reading

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature ds18b20(&oneWire);

// Join passphrase (PSKd). PRODUCTION: provision a UNIQUE value per device into
// NVS (namespace "factory", key "pskd") at the bench/factory and print it on the
// unit's QR label; the commissioner must `add <EUI> <that PSKd>`. If NVS has no
// value we fall back to this build default so dev boards still join.
char g_pskd[33] = "J01NME";

// Load a per-device PSKd from NVS if one was provisioned (else keep the default).
static void load_pskd_from_nvs() {
  nvs_handle_t h;
  if (nvs_open("factory", NVS_READONLY, &h) == ESP_OK) {
    size_t n = sizeof(g_pskd);
    nvs_get_str(h, "pskd", g_pskd, &n);   // leaves g_pskd unchanged on ESP_ERR_NVS_NOT_FOUND
    nvs_close(h);
  }
}

// Factory EUI-64 (hex), used to tag every UDP payload so the gateway/host can
// map this physical sensor to its rack box/slot. Set once in setup().
char g_eui[17] = "0000000000000000";

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
    ESP.restart(); 
    
  } else if (aError == 23) {
    Serial.println("[JOINER] Error 23: Missed the router's beacon. Will retry...");
    g_failed = false; 
    
  } else {
    g_failed = true;
    Serial.printf("[JOINER] FATAL: Handshake failed with Error %d. STOPPING.\n", aError);
    if (aError == 28) Serial.println("[DEBUG] Error 28 = Security Rejected. PSKD mismatch.");
  }
}

// --- Helper: start joiner (caller MUST hold OT lock) ---
static void start_joiner_locked(otInstance *inst) {
  otJoinerStop(inst);

  otError err = otJoinerStart(
    inst, g_pskd, NULL, "MyVendor", "MySensor", "1.0.0", NULL, otaJoinerCallback, NULL);

  if (err != OT_ERROR_NONE) {
    Serial.printf("[JOINER] WARNING: Joiner failed to initialize! Error: %d\n", err);
  } else {
    Serial.println("[JOINER] Joiner process started. Scanning...");
  }
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  delay(2000);

  // Initialize Dallas Temperature Library
  ds18b20.begin();
  Serial.printf("[SENSORS] DS18B20 init. Found %d sensors.\n", ds18b20.getDeviceCount());

  // 1. ROBUST NVS INITIALIZATION
  esp_err_t ret = nvs_flash_init();
  if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
      Serial.println("[SYSTEM] NVS partition requires formatting. Erasing...");
      nvs_flash_erase();
      ret = nvs_flash_init(); 
  }
  
  if (ret != ESP_OK) {
      Serial.printf("[SYSTEM] NVS Init Failed: %s\n", esp_err_to_name(ret));
  } else {
      Serial.println("[SYSTEM] NVS Partition mounted successfully.");
  }

  // Load a per-device join PSKd if one was provisioned at the factory.
  load_pskd_from_nvs();

  // Capture our factory EUI-64 once (hex), for tagging UDP payloads.
  {
    uint8_t mac8[8];
    if (esp_read_mac(mac8, ESP_MAC_IEEE802154) == ESP_OK) {
      for (int i = 0; i < 8; i++) sprintf(g_eui + i * 2, "%02x", mac8[i]);
    }
    Serial.printf("[HW] EUI-64: %s\n", g_eui);
  }

  Serial.println("\n[BOOT] Starting OpenThread SED Device...");
  OpenThread::begin(false);
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
    
    uint8_t hardware_mac[8];
    if (esp_read_mac(hardware_mac, ESP_MAC_IEEE802154) == ESP_OK) {
      Serial.print("[HW] Factory EUI-64: ");
      for (int i = 0; i < 8; i++) Serial.printf("%02x", hardware_mac[i]);
      Serial.println();
    }

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

      static uint32_t radar_timer = 0;
      if (millis() - radar_timer > 2000) {
        radar_timer = millis();
        const otMacCounters *mac = otLinkGetCounters(inst);
        Serial.printf("[JOINER RADAR] State: %d | MAC TX: %lu  RX: %lu | Ch: %d PAN: 0x%04X\n",
                      state, (unsigned long)mac->mTxTotal, (unsigned long)mac->mRxTotal,
                      otLinkGetChannel(inst), otLinkGetPanId(inst));
      }

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

  // --- 3. SENSOR READING & UDP TX AFTER JOIN ---
  static uint32_t last = 0;
  
  // We check the timer OUTSIDE the lock. 
  if (g_joined && (millis() - last > 10000)) {
    
    // 3a. Read Sensors (This blocks for ~750ms, so we do it BEFORE locking Thread)
    ds18b20.requestTemperatures();
    
    // Payload tags the sensor's EUI then a CSV of ROM-tagged probe temps:
    //   "EUI=58e6c5fffe164ec0;t=28ff..a1:23.1,28ff..b2:err,..."
    // Each probe is identified by its DS18B20 ROM (64-bit serial) so an exhaust
    // mapping survives unplug/reorder. Count is auto-detected (capped MAX_PROBES).
    char payload[360];
    snprintf(payload, sizeof(payload), "EUI=%s;t=", g_eui);
    char tempStr[32];
    char romStr[17];

    int n = ds18b20.getDeviceCount();
    if (n > MAX_PROBES) n = MAX_PROBES;
    int emitted = 0;
    DeviceAddress addr;
    for (int i = 0; i < n; i++) {
      if (!ds18b20.getAddress(addr, i)) continue;   // probe vanished mid-scan
      for (int b = 0; b < 8; b++) snprintf(romStr + b * 2, 3, "%02x", addr[b]);
      float temp = ds18b20.getTempC(addr);

      if (emitted > 0) strcat(payload, ",");
      if (temp == DEVICE_DISCONNECTED_C) {
        snprintf(tempStr, sizeof(tempStr), "%s:err", romStr);
      } else {
        snprintf(tempStr, sizeof(tempStr), "%s:%.1f", romStr, temp);
      }
      strcat(payload, tempStr);
      emitted++;
    }

    // 3b. Acquire OpenThread Lock and Transmit
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
      otInstance *inst = esp_openthread_get_instance();
      otDeviceRole currentRole = otThreadGetDeviceRole(inst);

      // Only send data if attached to the mesh as a CHILD
      if (currentRole == OT_DEVICE_ROLE_CHILD) {
        last = millis(); // Reset timer only upon successful check

        otMessage *msg = otUdpNewMessage(inst, NULL);
        if (msg != NULL) {
          
          otMessageAppend(msg, payload, strlen(payload));

          otUdpSocket ephemeralSocket;
          memset(&ephemeralSocket, 0, sizeof(ephemeralSocket));
          otUdpOpen(inst, &ephemeralSocket, NULL, NULL);

          otMessageInfo messageInfo;
          memset(&messageInfo, 0, sizeof(messageInfo));
          messageInfo.mPeerPort = 1234;
          
          otIp6AddressFromString("ff03::2", &messageInfo.mPeerAddr);

          otError sendErr = otUdpSend(inst, &ephemeralSocket, msg, &messageInfo);
          otUdpClose(inst, &ephemeralSocket);

          if (sendErr == OT_ERROR_NONE) {
            Serial.printf("[UDP] Packet sent: %s\n", payload);
          } else {
            Serial.printf("[UDP] Send failed: %d\n", sendErr);
            otMessageFree(msg);
          }
        }
      }
      esp_openthread_lock_release();
    }
  }

  delay(10);
}