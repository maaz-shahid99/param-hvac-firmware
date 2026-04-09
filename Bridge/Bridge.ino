#include <NimBLEDevice.h>
#include <WiFi.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <nvs_flash.h>  // Added for full NVS wipe
#include "bme_sensor.h"
#include "rtc_ds1307.h"
#include "logger.h"

#define SD_CS 3
#define SD_SCK 8
#define SD_MOSI 10
#define SD_MISO 4

// --- Configuration ---
static const int UART_BAUD_RATE = 115200;

//***This pinout is for Bridge when target MCU is ESP32C6***

// static const int UART_TX_PIN = 16;
// static const int UART_RX_PIN = 17;

//***This pinout is for Bridge when target MCU is ESP32C3***

static const int UART_TX_PIN = 21;
static const int UART_RX_PIN = 20;



static const int SWITCH_PIN = 2;
static const int RESET_BTN_PIN = 9;  // BOOT button for factory reset
// TOGGLE SWITCH: HIGH = Setup (Commissioner Mode), LOW = Secure (Gateway Mode)

// --- UUIDs ---
static const char *SERVICE_UUID = "4fafc201-1fb5-459e-8fcc-c5c9c331914b";
static const char *CHAR_UUID = "beb5483e-36e1-4688-b7f5-ea07361b26a8";
static const char *DEVICE_NAME = "ESP32C6-Thread-Bridge";

static const size_t UART_MAX_LINE_LEN = 512;
static const uint32_t ADD_RESULT_TIMEOUT_MS = 15000;  // Bridge-side failsafe

// --- Authentication Constants ---
static const char *AUTH_NAMESPACE = "auth_config";
static const char *DEFAULT_PIN = "123456";

// --- Globals ---
NimBLEServer *pServer = nullptr;
NimBLEService *pService = nullptr;
NimBLECharacteristic *pCharacteristic = nullptr;
Preferences preferences;

volatile bool bleClientConnected = false;
volatile bool bleClientSecured = false;        // OS-Level Encryption (Just Works)
volatile bool isSessionAuthenticated = false;  // App-Level Authentication
bool isCommissionerMode = false;
// State tracked via Switch

// Pending command tracking
static bool g_pendingAdd = false;
static String g_pendingEui64;
static uint32_t g_pendingDeadlineMs = 0;

// --- Reset Button Tracking ---
uint32_t resetBtnPressTime = 0;
bool resetBtnPressed = false;

Logger logger(SD_CS, SD_MISO, SD_MOSI, SD_SCK);

// --- BLE Notification Helper ---
static void bleNotifyLine(const String &line) {
  if (!bleClientConnected || !bleClientSecured || pCharacteristic == nullptr) return;
  std::string s(line.c_str());
  pCharacteristic->setValue(s);
  pCharacteristic->notify();
}

// --- Utils ---
static bool isHexChar(char c) {
  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static bool looksLikeEui64(const String &s) {
  if (s.length() != 16) return false;
  for (int i = 0; i < 16; i++)
    if (!isHexChar(s[i])) return false;
  return true;
}

// --- Provisioning Logic (JSON Parsing & Wi-Fi) ---
void handleProvisioning(const String &jsonPayload) {
  DynamicJsonDocument doc(512);
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error) {
    Serial.println("[JSON] Failed to parse provisioning payload");
    bleNotifyLine("ERR JSON_INVALID");
    return;
  }

  const char *ssid = doc["ssid"];
  const char *pass = doc["pass"];
  const char *zone = doc["zone"];
  const char *netName = doc["netName"];

  if (!ssid || !pass || !netName) {
    bleNotifyLine("ERR MISSING_FIELDS");
    return;
  }

  Serial.printf("[PROVISION] SSID: %s, Zone: %s, NetName: %s\n", ssid, zone, netName);

  // 1. Save to NVS
  preferences.begin("gateway_config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("zone", zone ? zone : "Default");
  preferences.end();

  // 2. Connect to Wi-Fi
  Serial.printf("[WIFI] Connecting to %s...\n", ssid);
  bleNotifyLine("STATUS CONNECTING_WIFI");

  WiFi.begin(ssid, pass);

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("[WIFI] Connected!");
    bleNotifyLine("WIFI_CONNECTED");

    // 3. Command Commissioner (Air-Gapped!)
    Serial1.printf("FORM_NET %s\n", netName);
    Serial1.flush();
    Serial.println("[UART] Sent FORM_NET command");
  } else {
    Serial.println("[WIFI] Failed to connect.");
    bleNotifyLine("ERR WIFI_AUTH");
  }
}

// --- Parsing Pending Adds ---
static void parsePendingFromCommand(const String &cmdLine) {
  String line = cmdLine;
  line.trim();
  if (!line.startsWith("add ")) return;

  int firstSpace = line.indexOf(' ');
  int secondSpace = line.indexOf(' ', firstSpace + 1);
  if (secondSpace < 0) return;  // "add <EUI64> <Key>|<Sig>"

  String eui = line.substring(firstSpace + 1, secondSpace);
  eui.trim();

  if (!looksLikeEui64(eui)) return;

  g_pendingAdd = true;
  g_pendingEui64 = eui;
  g_pendingDeadlineMs = millis() + ADD_RESULT_TIMEOUT_MS;

  Serial.print("[STATE] Pending add set for EUI64=");
  Serial.println(g_pendingEui64);
}

// --- UART Handlers (UPDATED V1.2 LOGIC) ---
static void handleCommissionerLine(const String &line) {
  // 0. FILTER: Ignore self-echoed commands
  if (line.startsWith("CMD:")) return;

  // Forward raw logs for debug
  bleNotifyLine(line);

  // 1. Check for Network Formation
  if (line.indexOf("NETWORK_FORMED") >= 0) {
    bleNotifyLine("ACK PROVISION SUCCESS");
    return;
  }

  // If we aren't waiting for a specific joiner, we are done
  if (!g_pendingAdd) return;

  // 2. Check for Joiner Success (Standard OpenThread log or Custom)
  if (line.indexOf("JOINER_ADDED") >= 0 || line.indexOf("JOINER_EVENT CONNECTED") >= 0) {
    String ack = "ACK ADD " + g_pendingEui64;
    Serial.print("[PROTO] ");
    Serial.println(ack);
    bleNotifyLine(ack);

    g_pendingAdd = false;
    g_pendingEui64 = "";
    return;
  }

  // --- NEW: Check for Joiner Removal (Timeout) ---
  // Matches the new "JOINER_EVENT REMOVED" log we added in commissioner.c
  if (line.indexOf("JOINER_EVENT REMOVED") >= 0) {
    String err = "ERR ADD " + g_pendingEui64 + " timeout";
    Serial.print("[PROTO] ");
    Serial.println(err);
    bleNotifyLine(err);

    g_pendingAdd = false;
    g_pendingEui64 = "";
    return;
  }

  // 3. Check for Generic Errors
  if (line.indexOf("ERROR") >= 0 || line.indexOf("FAILED") >= 0) {
    String err = "ERR ADD " + g_pendingEui64 + " commissioner_error";
    Serial.print("[PROTO] ");
    Serial.println(err);
    bleNotifyLine(err);

    g_pendingAdd = false;
    g_pendingEui64 = "";
    return;
  }
}

// --- BLE Callbacks ---
class BridgeServerCallbacks : public NimBLEServerCallbacks {
  void onConnect(NimBLEServer *server, NimBLEConnInfo &connInfo) override {
    bleClientConnected = true;
    bleClientSecured = connInfo.isEncrypted();
    isSessionAuthenticated = false;  // Reset app-level auth on new connection
    Serial.printf("[BLE] Connected: %s\n", connInfo.getAddress().toString().c_str());
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    bleClientConnected = false;
    bleClientSecured = false;
    isSessionAuthenticated = false;  // Clear session state
    Serial.println("[BLE] Disconnected.");

    if (isCommissionerMode) {
      NimBLEDevice::startAdvertising();
      Serial.println("[BLE] Restarted Advertising (Setup Mode Active).");
    }
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    if (!connInfo.isEncrypted()) {
      Serial.println("[BLE] Auth failed/unencrypted. Disconnecting.");
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      bleClientSecured = false;
      return;
    }
    bleClientSecured = true;
    Serial.println("[BLE] Secured Link Established (OS-Level).");
    bleNotifyLine("BRIDGE READY");
  }
};

class BridgeCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    // 1. OS-Level Security Check
    if (!bleClientSecured) {
      Serial.println("[BLE] Rejected write (Link Not Secured)");
      return;
    }

    std::string value = pChar->getValue();
    if (value.empty()) return;
    String cmdLine(value.c_str());
    cmdLine.trim();  // Sanitize input

    // ==========================================
    // 2. APP-LEVEL AUTHENTICATION LOGIC
    // ==========================================

    // Command: STATUS?
    if (cmdLine == "STATUS?") {
      preferences.begin(AUTH_NAMESPACE, true);
      bool isSetup = preferences.getBool("is_setup", false);
      preferences.end();

      if (isSetup) {
        bleNotifyLine("STATUS|SECURED");
      } else {
        bleNotifyLine("STATUS|SETUP_PENDING");
      }
      return;
    }

    // Command: AUTH|<Pin>
    if (cmdLine.startsWith("AUTH|")) {
      String attemptPin = cmdLine.substring(5);

      preferences.begin(AUTH_NAMESPACE, true);
      String savedPin = preferences.getString("pin", DEFAULT_PIN);
      preferences.end();

      if (attemptPin == savedPin) {
        isSessionAuthenticated = true;
        bleNotifyLine("ACK AUTH SUCCESS");
        Serial.println("[AUTH] Session Unlocked");
      } else {
        bleNotifyLine("ERR AUTH FAILED");
        Serial.println("[AUTH] Failed login attempt");
      }
      return;
    }

    // Command: SETPIN|<OldPin>|<NewPin>
    if (cmdLine.startsWith("SETPIN|")) {
      int firstPipe = cmdLine.indexOf('|');
      int secondPipe = cmdLine.indexOf('|', firstPipe + 1);

      if (firstPipe > 0 && secondPipe > firstPipe) {
        String oldPin = cmdLine.substring(firstPipe + 1, secondPipe);
        String newPin = cmdLine.substring(secondPipe + 1);

        preferences.begin(AUTH_NAMESPACE, false);
        String savedPin = preferences.getString("pin", DEFAULT_PIN);

        if (oldPin == savedPin) {
          preferences.putString("pin", newPin);
          preferences.putBool("is_setup", true);
          isSessionAuthenticated = true;  // Auto-login after setup
          bleNotifyLine("ACK SETPIN SUCCESS");
          Serial.println("[AUTH] PIN updated and session unlocked");
        } else {
          bleNotifyLine("ERR SETPIN FAILED");
          Serial.println("[AUTH] SETPIN failed: Old PIN mismatch");
        }
        preferences.end();
      } else {
        bleNotifyLine("ERR SETPIN FORMAT");
      }
      return;
    }

    // ==========================================
    // 3. THE GATEKEEPER
    // ==========================================
    // Any commands beyond this point require the session to be authenticated.
    if (!isSessionAuthenticated) {
      Serial.println("[BLE] Rejected write (App-Level Unauthenticated)");
      bleNotifyLine("ERR UNAUTHENTICATED");
      return;
    }

    // ==========================================
    // 4. SECURED COMMANDS
    // ==========================================

    // A. PROVISION
    if (cmdLine.startsWith("PROVISION|")) {
      Serial.println("[BLE] Received Provisioning Payload");
      String jsonPart = cmdLine.substring(10);
      handleProvisioning(jsonPart);
      return;
    }

    // B. ADD (Busy Check)
    if (g_pendingAdd && cmdLine.startsWith("add ")) {
      Serial.println("[BLE] Rejecting add: Busy");
      bleNotifyLine("ERR BUSY");
      return;
    }

    // C. Forward to UART
    // Forward the FULL command (including the |hash) to the Commissioner
    if (!cmdLine.endsWith("\n")) {
      cmdLine += "\n";
    }

    parsePendingFromCommand(cmdLine);

    Serial1.print(cmdLine);
    Serial1.flush();
    Serial.printf("[UART] Forwarded full command (%d bytes)\n", cmdLine.length());
  }
};

// --- BLE Lifecycle ---
void configureBLE() {
  NimBLEDevice::init(DEVICE_NAME);
  NimBLEDevice::setPower(ESP_PWR_LVL_P9);
  NimBLEDevice::setSecurityAuth(true, false, true);
  NimBLEDevice::setSecurityIOCap(BLE_HS_IO_NO_INPUT_OUTPUT);

  pServer = NimBLEDevice::createServer();
  pServer->setCallbacks(new BridgeServerCallbacks());

  pService = pServer->createService(SERVICE_UUID);
  pCharacteristic = pService->createCharacteristic(
    CHAR_UUID,
    NIMBLE_PROPERTY::WRITE | NIMBLE_PROPERTY::WRITE_ENC | NIMBLE_PROPERTY::NOTIFY);
  pCharacteristic->setCallbacks(new BridgeCharacteristicCallbacks());

  pService->start();

  NimBLEAdvertising *pAdvertising = NimBLEDevice::getAdvertising();
  pAdvertising->addServiceUUID(SERVICE_UUID);

  NimBLEDevice::startAdvertising();
  Serial.println("[BLE] Stack Initialized & Advertising.");
}

void deinitBLE() {
  if (pServer) {
    auto peers = pServer->getPeerDevices();
    for (auto &peer : peers) {
      pServer->disconnect(peer);
    }
  }
  NimBLEDevice::stopAdvertising();
  NimBLEDevice::deinit(true);
  pServer = nullptr;
  pService = nullptr;
  pCharacteristic = nullptr;
  Serial.println("[BLE] Stack De-initialized (Secure Mode).");
}

// --- Main ---
void setup() {
  Serial.begin(115200);
  Serial1.begin(UART_BAUD_RATE, SERIAL_8N1, UART_RX_PIN, UART_TX_PIN);

  bmeInit();
  rtcInit();

  logger.begin();
  logger.setFilename("/env_log.csv");
  logger.writeHeader("Date,Time,TempC,Humidity,Pressure,VOC");

  pinMode(SWITCH_PIN, INPUT_PULLUP);
  pinMode(RESET_BTN_PIN, INPUT_PULLUP);

  Serial.println("\n[BOOT] Bridge Starting...");

  // Initialize Authentication Defaults if first boot
  preferences.begin(AUTH_NAMESPACE, false);
  if (!preferences.isKey("is_setup")) {
    Serial.println("[BOOT] First boot detected. Initializing Auth NVS.");
    preferences.putBool("is_setup", false);
    preferences.putString("pin", DEFAULT_PIN);
  }
  preferences.end();

  // Load Wi-Fi Gateway Config
  preferences.begin("gateway_config", true);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  preferences.end();

  if (savedSSID.length() > 0) {
    Serial.printf("[BOOT] Auto-connecting to WiFi: %s\n", savedSSID.c_str());
    WiFi.begin(savedSSID.c_str(), savedPass.c_str());
  }
}

void loop() {
  // ==========================================
  // 0. FACTORY RESET LOGIC (1-Second Hold)
  // ==========================================
  if (digitalRead(RESET_BTN_PIN) == LOW) {  // Button is pressed (pulled to ground)
    if (!resetBtnPressed) {
      resetBtnPressed = true;
      resetBtnPressTime = millis();
      Serial.println("[SYSTEM] Reset button pressed. Hold for 1s to factory reset...");
    } else if (millis() - resetBtnPressTime >= 10000) {
      // Button held for 1 second
      Serial.println("\n[SYSTEM] === FACTORY RESET INITIATED ===");

      // 1. Wipe the ESP32 Bridge NVS
      nvs_flash_erase();
      nvs_flash_init();

      // 2. Wipe the Commissioner via UART
      Serial1.println("factoryreset");
      Serial1.flush();

      Serial.println("[SYSTEM] NVS Cleared and Commissioner Reset command sent.");
      Serial.println("[SYSTEM] Rebooting in 2 seconds...\n");

      // 3. Disconnect and Restart gracefully
      deinitBLE();
      WiFi.disconnect(true);
      delay(2000);
      ESP.restart();
    }
  } else {
    if (resetBtnPressed) {
      resetBtnPressed = false;  // Reset the timer if released early
      Serial.println("[SYSTEM] Reset button released. Reset aborted.");
    }
  }

  static char lineBuf[UART_MAX_LINE_LEN];
  static size_t lineLen = 0;

  // 1. Switch Logic

  bool switchState = digitalRead(SWITCH_PIN);

  if (switchState == HIGH && !isCommissionerMode) {
    isCommissionerMode = true;

    Serial.println("[MODE] Switch ON -> Enter SETUP/COMMISSIONER Mode");
    configureBLE();
    Serial1.println("commissioner_start");
  } else if (switchState == LOW && isCommissionerMode) {
    isCommissionerMode = false;

    Serial.println("[MODE] Switch OFF -> Enter SECURE Mode");
    deinitBLE();
    Serial1.println("commissioner_stop");
  }

  // 2. UART Reading
  while (Serial1.available() > 0) {
    int ch = Serial1.read();

    if (ch < 0) break;
    if (ch == '\r') continue;

    if (ch == '\n') {
      if (lineLen == 0) continue;

      lineBuf[lineLen] = '\0';
      String line(lineBuf);

      Serial.printf("[UART Rx] %s\n", lineBuf);
      handleCommissionerLine(line);

      lineLen = 0;

    } else {
      if (lineLen < UART_MAX_LINE_LEN - 1) {
        lineBuf[lineLen++] = (char)ch;

      } else {
        Serial.println("[UART] Overflow dropped");
        lineLen = 0;
      }
    }
  }

  // 3. Pending Timeout Check (Bridge failsafe)
  // Only fires if we never got a "REMOVED" or "ADDED" message from Comm.

  if (g_pendingAdd && (int32_t)(millis() - g_pendingDeadlineMs) >= 0) {
    bleNotifyLine("ERR ADD TIMEOUT");

    Serial.println("[PROTO] Timed out waiting for JOINER_ADDED");
    g_pendingAdd = false;
  }


  //Update BME and log
  if (bmeUpdate()) {
    BMEData data        = bmeGetData();
    RTCDateTime dt      = rtcGetDateTime();

    String dateStr = dt.valid
        ? (String(dt.day)   + "-" + String(dt.month)  + "-" + String(dt.year))
        : "N/A";

    String timeStr = dt.valid
        ? (String(dt.hour)  + ":" + String(dt.minute) + ":" + String(dt.second))
        : String(millis() / 1000) + "s";

    String dataLine = String(data.temperature, 2) + "," +
                      String(data.humidity,    2) + "," +
                      String(data.pressure,    2) + "," +
                      String(data.gas,         2);

    if (logger.log(dateStr + "," + timeStr + "," + dataLine)) {
        Serial.println("Logged: " + dateStr + " " + timeStr);
    } else {
        Serial.println("Log Failed");
    }
}

  delay(5);
}