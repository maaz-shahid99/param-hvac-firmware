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

// --- Auto network-recovery timeouts ---------------------------------------
// If we boot holding stored credentials but can't (re)attach to that network
// within OLD_NET_TIMEOUT_MS, clear the Thread credentials and reboot so the
// device drops into the joiner and hunts for a NEW network. If the joiner then
// can't find a new network within NEW_NET_TIMEOUT_MS, reboot to keep searching.
// Together this stops a unit from being stranded on a router that is gone.
#define OLD_NET_TIMEOUT_MS  60000UL     // 60 s trying the stored / old network
#define NEW_NET_TIMEOUT_MS  180000UL    // 180 s searching for a new network
uint32_t g_attach_deadline = 0;   // ms deadline to attach to the stored network (0 = disarmed)
uint32_t g_join_deadline   = 0;   // ms deadline for the joiner to find a network (0 = disarmed)

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

// --- Forget the stored Thread network, then reboot to search for a new one ---
// Erases only the OpenThread persistent info (the stored dataset), NOT the whole
// NVS, so a factory-provisioned per-device PSKd ("factory"/"pskd") survives. On
// the next boot otDatasetGetActive() fails -> the joiner runs -> new network.
static void leaveNetworkAndReboot() {
  if (esp_openthread_lock_acquire(pdMS_TO_TICKS(1000))) {
    otInstance *inst = esp_openthread_get_instance();
    otThreadSetEnabled(inst, false);       // must be disabled before erasing
    otInstanceErasePersistentInfo(inst);   // wipe the stored Thread dataset
    esp_openthread_lock_release();
  }
  delay(200);
  ESP.restart();                           // does not return
}

// --- STATUS LEDS (3 discrete) ----------------------------------------------
// Three individual (single-colour) LEDs give at-a-glance status without serial:
//
//   * POWER   (green) — solid once the firmware is running. (May instead be wired
//                       straight to the 3V3 rail; then drop LED_PWR_PIN below.)
//   * NETWORK (blue)  — ~1.25 Hz blink while joining/attaching, solid once
//                       attached to the mesh as a CHILD (i.e. reporting).
//   * FAULT   (red)   — solid on a permanent join failure (e.g. PSKd mismatch),
//                       brief blink on a transient error (DS18B20 err / UDP TX fail).
//
// Wiring: each pin -> resistor -> LED -> GND (active-HIGH). D4 is the OneWire bus,
// so it is avoided. Pins are XIAO ESP32-C6 pads; adjust to match your board. Set
// LED_ACTIVE_LOW to 1 for active-LOW (common-anode) wiring.
#define LED_PWR_PIN     D1   // green  — power / firmware alive
#define LED_NET_PIN     D2   // blue   — network / join status
#define LED_FAULT_PIN   D3   // red    — fault
#define LED_ACTIVE_LOW   0

volatile bool     g_child            = false;  // attached as CHILD -> NETWORK solid
volatile uint32_t g_fault_blink_until = 0;     // ms; transient-error blink window

static inline void ledWrite(int pin, bool on) {
  digitalWrite(pin, (on ^ (LED_ACTIVE_LOW != 0)) ? HIGH : LOW);
}

static void setupLeds() {
  pinMode(LED_PWR_PIN,   OUTPUT);
  pinMode(LED_NET_PIN,   OUTPUT);
  pinMode(LED_FAULT_PIN, OUTPUT);
  ledWrite(LED_PWR_PIN,   true);    // power / alive
  ledWrite(LED_NET_PIN,   false);
  ledWrite(LED_FAULT_PIN, false);
}

// Non-blocking; call every loop(). Renders blink patterns from millis().
static void renderLeds() {
  const uint32_t t = millis();

  ledWrite(LED_PWR_PIN, true);                       // power: solid

  bool net_on;                                       // network / join status
  if (g_failed)      net_on = false;                 // (fault LED takes over)
  else if (g_child)  net_on = true;                  // solid: attached & reporting
  else               net_on = ((t / 400) % 2) == 0;  // ~1.25 Hz: joining/attaching
  ledWrite(LED_NET_PIN, net_on);

  bool fault_on;                                     // fault
  if (g_failed)                                    fault_on = true;                  // solid
  else if ((int32_t)(g_fault_blink_until - t) > 0) fault_on = ((t / 150) % 2) == 0;  // blink
  else                                             fault_on = false;
  ledWrite(LED_FAULT_PIN, fault_on);
}

// --- SETUP ---
void setup() {
  Serial.begin(115200);
  setupLeds();              // 3 status LEDs: power on, network/fault off
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
    g_attach_deadline = millis() + OLD_NET_TIMEOUT_MS;   // 60 s to (re)attach, else forget + search

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
    g_join_deadline = millis() + NEW_NET_TIMEOUT_MS;     // 180 s to find a new network, else reboot
  }

  esp_openthread_lock_release();
}


// --- MAIN LOOP ---
void loop() {
  // --- 0. NETWORK-RECOVERY WATCHDOGS ---------------------------------------
  // (a) Stored/old network: while we hold credentials but aren't attached, poll
  //     the role so we notice (re)attachment. If we never attach within the
  //     OLD_NET_TIMEOUT_MS window, forget the credentials and reboot to search
  //     for a new network. The deadline disarms on first attach, so a later brief
  //     dropout will NOT wipe a network we successfully joined.
  if (g_joined && !g_child) {
    static uint32_t attachPoll = 0;
    if (millis() - attachPoll > 2000) {
      attachPoll = millis();
      if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
        otInstance *inst = esp_openthread_get_instance();
        if (otThreadGetDeviceRole(inst) == OT_DEVICE_ROLE_CHILD) {
          g_child = true;
          g_attach_deadline = 0;                 // attached -> disarm the watchdog
        }
        esp_openthread_lock_release();
      }
    }
    if (g_attach_deadline && (int32_t)(millis() - g_attach_deadline) >= 0) {
      Serial.println("[NET] Stored network not found in 60s. Clearing credentials + rebooting to search for a new network.");
      leaveNetworkAndReboot();                   // does not return
    }
  }

  // (b) Joiner: if no new network is found within NEW_NET_TIMEOUT_MS, reboot to
  //     keep searching (credentials are already cleared, so we re-enter the joiner).
  if (!g_joined && !g_failed && g_join_deadline &&
      (int32_t)(millis() - g_join_deadline) >= 0) {
    Serial.println("[NET] No new network found in 180s. Rebooting to keep searching.");
    delay(200);
    ESP.restart();
  }

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
  // Gate on g_child: only read sensors + TX once actually attached as a CHILD
  // (avoids hammering the 1-Wire bus while still detached / searching).
  if (g_child && (millis() - last > 10000)) {
    
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
        g_fault_blink_until = millis() + 1500;   // FAULT LED: brief blink on a dead probe
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
      g_child = (currentRole == OT_DEVICE_ROLE_CHILD);  // NETWORK LED: solid when attached

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
            g_fault_blink_until = millis() + 1500;   // FAULT LED: brief blink on TX failure
            otMessageFree(msg);
          }
        }
      }
      esp_openthread_lock_release();
    }
  }

  renderLeds();   // update the 3 status LEDs (non-blocking)
  delay(10);
}