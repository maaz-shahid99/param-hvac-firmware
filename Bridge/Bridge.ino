#include <NimBLEDevice.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <nvs_flash.h>  // Added for full NVS wipe
#include "mbedtls/base64.h"

// Bump this on every C3 build you publish; OTA only applies a STRICTLY newer
// c3_version from the manifest.
#define BRIDGE_FW_VERSION 7
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

// True only on the unit whose C6 is the Thread Leader (the active gateway).
// Wi-Fi/uplink is brought up only on the active gateway; all others hold the
// replicated credentials in NVS on standby.
bool isActiveGateway = false;

// Stand-down hysteresis: a freshly-forming gateway and a brief partition blip
// both transiently report non-Leader. Don't drop Wi-Fi immediately on STANDBY —
// only after it persists past the grace period.
bool     standbyPending = false;
uint32_t standbyPendingSince = 0;
static const uint32_t STANDBY_GRACE_MS = 30000;

// BLE management endpoint gating: only the ACTIVE gateway (Thread Leader) — or a
// not-yet-networked unit in setup mode — advertises, so the app sees one device.
bool c6OnNetwork = false;   // true once the C6 has reported any GW_ROLE (it's a mesh member)
bool bleStackUp  = false;   // whether the NimBLE stack/advertising is currently active

// Forward declarations (definitions live further down)
void configureBLE();
void deinitBLE();
static bool bleShouldAdvertise();
static void updateBleAdvertising();
static void forwardReading(const String &eui, const String &data);

// --- Discovery / data-forwarding to the display node ---
// The cloud discovery server address is fixed infrastructure baked into the
// firmware (same for every unit, so it survives gateway failover). It can be
// overridden per-site by a "disc" field in the PROVISION payload (stored NVS).
#define DEFAULT_DISCOVERY_URL "http://10.14.98.109:8000"   // <-- set to your discovery server
static String   g_discoveryUrl = DEFAULT_DISCOVERY_URL;
static String   g_nodeUrl      = "";                        // discovered display node e.g. http://192.168.1.60:8001
static uint32_t g_lastDiscover = 0;
static uint32_t g_lastBeat     = 0;
static const uint32_t DISCOVER_INTERVAL_MS = 10000;
static const uint32_t BEAT_INTERVAL_MS     = 10000;

// OTA is requested from the BLE task but RUN from loop() so that Serial1 has a
// single reader during the C6 transfer. 0=none, 1=check/self-update C3, 2=push C6.
volatile int g_pendingOta = 0;
static void performOtaCheck();
static void performOtaC6();

// Fleet OTA: the C6 relays "OTA_NOW <baseurl>" (from the mesh broadcast). We
// run it staggered (gateway last) so the whole fleet doesn't reboot at once.
int      g_c6Version       = -1;     // reported by the C6 ("C6_VERSION n"); -1 = unknown
bool     g_fleetOtaPending = false;
String   g_fleetOtaBaseUrl;
uint32_t g_fleetOtaAt      = 0;
static void performFleetOta(const String &baseurl);

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

// Strip a trailing "|<64-hex HMAC>" signature (the app appends one to signed
// commands). Used for commands handled locally on the C3 (MAP, OTA) — they're
// gated by the authenticated session, so the signature isn't verified here.
static String stripTrailingSig(const String &line) {
  int bar = line.lastIndexOf('|');
  if (bar < 0 || (int)(line.length() - bar - 1) != 64) return line;
  for (int i = bar + 1; i < (int)line.length(); i++)
    if (!isHexChar(line[i])) return line;
  return line.substring(0, bar);
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
  const char *disc = doc["disc"];        // optional: discovery server URL override

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
  preferences.putString("net", netName);
  if (disc && strlen(disc) > 0) {
    preferences.putString("disc", disc);
    g_discoveryUrl = disc;
  }
  preferences.end();

  // 2. Connect to Wi-Fi (drop any prior association first so switching to a
  //    different SSID on re-provisioning is reliable).
  Serial.printf("[WIFI] Connecting to %s...\n", ssid);
  bleNotifyLine("STATUS CONNECTING_WIFI");

  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
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

    // 4. Replicate these creds to the whole fleet via the mesh (C6 signs +
    //    multicasts). Every router stores them on standby so any unit can
    //    become the gateway. Include the admin PIN if one has been set (else
    //    "-" = leave PIN unchanged). (Fields must not contain '|'.)
    preferences.begin(AUTH_NAMESPACE, true);
    bool setupDone = preferences.getBool("is_setup", false);
    String curPin  = preferences.getString("pin", DEFAULT_PIN);
    preferences.end();
    const char *pinField = setupDone ? curPin.c_str() : "-";

    Serial1.printf("cfg_publish %s|%s|%s|%s|%s\n",
                   ssid, pass, zone ? zone : "Default", netName, pinField);
    Serial1.flush();
    Serial.println("[UART] Sent cfg_publish for mesh-wide credential replication");

    // This unit just provisioned -> it will become the Leader/active gateway;
    // GW_ROLE LEADER from the C6 will (re)assert Wi-Fi after boot.
    isActiveGateway = true;
  } else {
    Serial.println("[WIFI] Failed to connect.");
    bleNotifyLine("ERR WIFI_AUTH");
  }
}

// --- Wi-Fi bring-up helper ---
// Cleanly switches APs: dropping any prior association first makes re-provisioning
// to a different SSID reliable on the ESP32.
static void applyWifi(const String &ssid, const String &pass) {
  if (ssid.length() == 0) return;
  Serial.printf("[WIFI] (Re)connecting to %s...\n", ssid.c_str());
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(ssid.c_str(), pass.c_str());
}

// --- C6 -> C3: store replicated credentials (standby) ---
// Payload: "ssid|pass|zone|net|pin". Never forwarded to BLE (carries secrets).
// pin == "-" means "leave the local admin PIN unchanged".
static void handleCfgSet(const String &payload) {
  int p1 = payload.indexOf('|');
  int p2 = payload.indexOf('|', p1 + 1);
  int p3 = payload.indexOf('|', p2 + 1);
  int p4 = payload.indexOf('|', p3 + 1);
  if (p1 < 0 || p2 < 0 || p3 < 0 || p4 < 0) return;

  String ssid = payload.substring(0, p1);
  String pass = payload.substring(p1 + 1, p2);
  String zone = payload.substring(p2 + 1, p3);
  String net  = payload.substring(p3 + 1, p4);
  String pin  = payload.substring(p4 + 1);

  preferences.begin("gateway_config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("zone", zone);
  preferences.putString("net",  net);
  preferences.end();

  Serial.printf("[CFG] Stored replicated creds (ssid=%s, net=%s)\n", ssid.c_str(), net.c_str());

  // Replicated admin PIN: store it so this unit accepts the same fleet PIN.
  if (pin.length() > 0 && pin != "-") {
    preferences.begin(AUTH_NAMESPACE, false);
    preferences.putString("pin", pin);
    preferences.putBool("is_setup", true);
    preferences.end();
    Serial.println("[CFG] Replicated admin PIN stored (fleet PIN updated).");
  }

  // If this unit is currently the active gateway, re-apply with the new creds.
  if (isActiveGateway) applyWifi(ssid, pass);
}

// --- C6 -> C3: gateway-role signal tied to Thread leadership ---
// Payload: "LEADER" (this unit is the active gateway) or "STANDBY".
static void handleGatewayRole(const String &role) {
  bool wantGateway = role.startsWith("LEADER");
  c6OnNetwork = true;  // any GW_ROLE means our C6 is a network member now

  if (wantGateway) {
    // Leader -> we are (or remain) the active gateway. Cancel any pending
    // stand-down and make sure Wi-Fi + the management BLE are up.
    standbyPending = false;
    if (!isActiveGateway) {
      isActiveGateway = true;
      Serial.println("[GW] Now ACTIVE gateway (Leader).");
    }
    if (WiFi.status() != WL_CONNECTED) {
      preferences.begin("gateway_config", true);
      String ssid = preferences.getString("ssid", "");
      String pass = preferences.getString("pass", "");
      preferences.end();
      applyWifi(ssid, pass);
    }
    updateBleAdvertising();
  } else {
    // STANDBY -> don't drop Wi-Fi immediately; start the grace timer. A
    // freshly-forming gateway reports non-Leader for ~15-20s before promotion,
    // and we must not tear down the uplink we just brought up. loop() drops
    // Wi-Fi only if STANDBY is still in effect after STANDBY_GRACE_MS.
    if (isActiveGateway && !standbyPending) {
      standbyPending = true;
      standbyPendingSince = millis();
      Serial.println("[GW] STANDBY received — grace timer started before dropping Wi-Fi.");
    } else if (!isActiveGateway) {
      // Plain joined router (never the gateway): ensure management BLE is off.
      updateBleAdvertising();
    }
  }
}

// --- Discovery: ask the discovery server for the display node's LAN ip ---
static void discoverNode() {
  if (g_discoveryUrl.isEmpty()) return;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(g_discoveryUrl + "/discover")) return;
  int code = http.GET();
  if (code == 200) {
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getString())) {
      JsonArray fwd = doc["forwarders"].as<JsonArray>();
      if (fwd.size() > 0) {
        String ip = fwd[0]["local_ip"] | "";
        int port = fwd[0]["port"] | 8001;
        if (ip.length()) {
          String nu = "http://" + ip + ":" + String(port);
          if (nu != g_nodeUrl) { g_nodeUrl = nu; Serial.printf("[DISC] display node -> %s\n", g_nodeUrl.c_str()); }
        }
      } else {
        Serial.println("[DISC] no display node registered yet (run display_node.py)");
      }
    }
  } else {
    Serial.printf("[DISC] /discover failed (HTTP %d) @ %s\n", code, g_discoveryUrl.c_str());
  }
  http.end();
}

// --- Presence heartbeat: tells the discovery server the site has internet ---
static void heartbeatPresence() {
  if (g_discoveryUrl.isEmpty()) return;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(g_discoveryUrl + "/register/sensor")) return;
  http.addHeader("Content-Type", "application/json");
  char id[24];
  snprintf(id, sizeof(id), "gateway-%04X", (uint16_t)(ESP.getEfuseMac() & 0xFFFF));
  String body = String("{\"sensor_id\":\"") + id + "\",\"meta\":{\"role\":\"gateway\"}}";
  http.POST(body);
  http.end();
}

// --- Forward one sensor reading to the display node (P2P on the LAN) ---
static void forwardReading(const String &eui, const String &data) {
  if (g_nodeUrl.isEmpty()) return;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(g_nodeUrl + "/ingest")) return;
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"sensor_id\":\"") + eui + "\",\"data\":\"" + data + "\"}";
  int code = http.POST(body);
  http.end();
  if (code > 0) {
    Serial.printf("[FWD] %s -> %s/ingest (%d)\n", eui.c_str(), g_nodeUrl.c_str(), code);
  } else {
    Serial.printf("[FWD] %s -> /ingest FAILED (%d) — re-discovering\n", eui.c_str(), code);
    g_nodeUrl = "";   // node unreachable -> force a re-discover
  }
}

// --- Register an EUI -> box/slot mapping on the display node (commissioning) ---
static bool registerSensorMap(const String &eui, int box, const String &slot) {
  if (g_nodeUrl.isEmpty()) discoverNode();
  if (g_nodeUrl.isEmpty()) return false;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(g_nodeUrl + "/map")) return false;
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"eui\":\"") + eui + "\",\"box\":" + String(box) + ",\"slot\":\"" + slot + "\"}";
  int code = http.POST(body);
  http.end();
  return code == 200;
}

// --- OTA: self-update the C3 from a firmware URL over Wi-Fi ---
static void otaC3FromUrl(const String &url) {
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(20000);
  if (!http.begin(url)) { bleNotifyLine("ERR OTA BEGIN_URL"); return; }

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    bleNotifyLine("ERR OTA HTTP " + String(code));
    http.end();
    return;
  }

  int len = http.getSize();                 // -1 if chunked/unknown
  if (!Update.begin(len > 0 ? (size_t)len : UPDATE_SIZE_UNKNOWN)) {
    bleNotifyLine("ERR OTA NOSPACE");
    http.end();
    return;
  }

  Serial.printf("[OTA] Downloading C3 image (%d bytes)...\n", len);
  bleNotifyLine("OTA DOWNLOADING");

  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  if (!Update.end(true)) {
    Serial.printf("[OTA] failed: %s\n", Update.errorString());
    bleNotifyLine("ERR OTA " + String(Update.getError()));
    return;
  }

  Serial.printf("[OTA] C3 update OK (%u bytes). Rebooting...\n", (unsigned)written);
  bleNotifyLine("OTA SUCCESS REBOOTING");
  delay(500);
  ESP.restart();
}

// Check the manifest on the display node and self-update if a newer C3 build
// is published. (Phase 1: this unit only. Fleet rollout = Phase 3.)
static void performOtaCheck() {
  if (g_nodeUrl.isEmpty()) { bleNotifyLine("ERR OTA NO_NODE"); return; }

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(g_nodeUrl + "/firmware/manifest.json")) { bleNotifyLine("ERR OTA MANIFEST"); return; }
  int code = http.GET();
  if (code != HTTP_CODE_OK) { bleNotifyLine("ERR OTA MANIFEST " + String(code)); http.end(); return; }

  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) { bleNotifyLine("ERR OTA MANIFEST_JSON"); return; }

  int c3ver = doc["c3_version"] | 0;
  String c3file = doc["c3_file"] | "";
  Serial.printf("[OTA] manifest c3_version=%d (running %d)\n", c3ver, BRIDGE_FW_VERSION);

  if (c3ver <= BRIDGE_FW_VERSION || c3file.isEmpty()) {
    bleNotifyLine("OTA UP_TO_DATE");
    return;
  }

  bleNotifyLine("OTA UPDATING v" + String(c3ver));
  otaC3FromUrl(g_nodeUrl + "/firmware/" + c3file);
}

// ===== Phase 2: stream a C6 firmware image to the C6 over UART =====

static void flushSerial1() { while (Serial1.available()) Serial1.read(); }

// Wait for a UART line from the C6 that starts with `expected`. Ignores other
// lines (logs, [UDP_RX], GW_ROLE, …). Returns false on timeout or OTA_ERR.
static bool waitForUart(const String &expected, uint32_t timeoutMs) {
  uint32_t start = millis();
  static char buf[600];
  int pos = 0;
  while (millis() - start < timeoutMs) {
    while (Serial1.available()) {
      int c = Serial1.read();
      if (c == '\n') {
        buf[pos] = '\0';
        String line(buf);
        line.trim();
        pos = 0;
        if (line.startsWith(expected)) return true;
        if (line.startsWith("OTA_ERR")) {
          Serial.println("[OTAC6] C6 reported: " + line);
          return false;
        }
      } else if (c != '\r' && pos < (int)sizeof(buf) - 1) {
        buf[pos++] = (char)c;
      }
    }
    delay(1);
  }
  return false;
}

static String base64Chunk(const uint8_t *data, size_t len) {
  unsigned char out[720];          // 512 raw -> 684 b64 chars
  size_t olen = 0;
  if (mbedtls_base64_encode(out, sizeof(out), &olen, data, len) != 0) return String();
  out[olen] = '\0';
  return String((char *)out);
}

// Download the C6 image over Wi-Fi and stream it to the C6 via UART OTA.
static bool otaC6FromUrl(const String &url) {
  HTTPClient http;
  http.setConnectTimeout(5000);
  http.setTimeout(20000);
  if (!http.begin(url)) { bleNotifyLine("ERR OTAC6 URL"); return false; }
  if (http.GET() != HTTP_CODE_OK) { bleNotifyLine("ERR OTAC6 HTTP"); http.end(); return false; }

  int total = http.getSize();
  if (total <= 0) { bleNotifyLine("ERR OTAC6 NOSIZE"); http.end(); return false; }
  WiFiClient *stream = http.getStreamPtr();

  flushSerial1();
  Serial1.printf("OTA_BEGIN %d\n", total);
  Serial1.flush();
  if (!waitForUart("OTA_READY", 8000)) { bleNotifyLine("ERR OTAC6 NOREADY"); http.end(); return false; }

  Serial.printf("[OTAC6] streaming %d bytes to C6...\n", total);
  bleNotifyLine("OTAC6 START " + String(total));

  const size_t CHUNK = 512;
  uint8_t raw[CHUNK];
  int seq = 0;
  size_t sent = 0;
  while (sent < (size_t)total) {
    size_t want = ((size_t)total - sent) < CHUNK ? ((size_t)total - sent) : CHUNK;
    int n = stream->readBytes(raw, want);
    if (n <= 0) { bleNotifyLine("ERR OTAC6 STREAM"); Serial1.println("OTA_ABORT"); http.end(); return false; }

    Serial1.printf("OTA_DATA %d %s\n", seq, base64Chunk(raw, n).c_str());
    Serial1.flush();
    if (!waitForUart("OTA_ACK " + String(seq), 8000)) {
      bleNotifyLine("ERR OTAC6 ACK " + String(seq));
      Serial1.println("OTA_ABORT");
      http.end();
      return false;
    }
    sent += n;
    seq++;
    if ((seq % 64) == 0) { bleNotifyLine("OTAC6 " + String(sent) + "/" + String(total)); delay(1); }
  }
  http.end();

  Serial1.println("OTA_END");
  Serial1.flush();
  if (!waitForUart("OTA_DONE", 15000)) { bleNotifyLine("ERR OTAC6 NODONE"); return false; }

  Serial.println("[OTAC6] C6 update complete; C6 is rebooting.");
  bleNotifyLine("OTAC6 SUCCESS");
  return true;
}

// Read the manifest for the C6 image and stream it to the C6. Runs in loop().
static void performOtaC6() {
  if (g_nodeUrl.isEmpty()) { bleNotifyLine("ERR OTAC6 NO_NODE"); return; }
  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(g_nodeUrl + "/firmware/manifest.json")) { bleNotifyLine("ERR OTAC6 MANIFEST"); return; }
  String c6file;
  if (http.GET() == HTTP_CODE_OK) {
    DynamicJsonDocument doc(512);
    if (!deserializeJson(doc, http.getString())) c6file = (const char *)(doc["c6_file"] | "");
  }
  http.end();
  if (c6file.isEmpty()) { bleNotifyLine("ERR OTAC6 NO_FILE"); return; }
  otaC6FromUrl(g_nodeUrl + "/firmware/" + c6file);
}

// Self-update this unit (C6 then C3) from <baseurl>/firmware, version-gated.
// Runs in loop() context. Brings up Wi-Fi first if this is a standby unit.
static void performFleetOta(const String &baseurl) {
  // Ensure Wi-Fi (standby routers keep it off until they need it).
  if (WiFi.status() != WL_CONNECTED) {
    preferences.begin("gateway_config", true);
    String s = preferences.getString("ssid", "");
    String p = preferences.getString("pass", "");
    preferences.end();
    if (s.isEmpty()) { Serial.println("[FLEETOTA] no Wi-Fi creds; abort"); return; }
    applyWifi(s, p);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    if (WiFi.status() != WL_CONNECTED) { Serial.println("[FLEETOTA] Wi-Fi failed; abort"); return; }
  }

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(baseurl + "/firmware/manifest.json")) { Serial.println("[FLEETOTA] manifest begin fail"); return; }
  if (http.GET() != HTTP_CODE_OK) { Serial.println("[FLEETOTA] manifest HTTP fail"); http.end(); return; }
  DynamicJsonDocument doc(512);
  DeserializationError e = deserializeJson(doc, http.getString());
  http.end();
  if (e) { Serial.println("[FLEETOTA] manifest json fail"); return; }

  int c3ver = doc["c3_version"] | 0; String c3file = doc["c3_file"] | "";
  int c6ver = doc["c6_version"] | 0; String c6file = doc["c6_file"] | "";
  Serial.printf("[FLEETOTA] manifest c3=%d (run %d), c6=%d (run %d)\n",
                c3ver, BRIDGE_FW_VERSION, c6ver, g_c6Version);

  // C6 first (it reboots independently; the C3 stays up to stream it).
  if (!c6file.isEmpty() && c6ver > g_c6Version) {
    otaC6FromUrl(baseurl + "/firmware/" + c6file);
  }
  // C3 last (this reboots us).
  if (!c3file.isEmpty() && c3ver > BRIDGE_FW_VERSION) {
    otaC3FromUrl(baseurl + "/firmware/" + c3file);
  }
  Serial.println("[FLEETOTA] complete (or already up-to-date)");
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

  // 0b. Control lines from the Commissioner (C6). Handle locally and DO NOT
  //     forward to BLE — CFG_SET carries the Wi-Fi password.
  if (line.startsWith("CFG_SET ")) {
    handleCfgSet(line.substring(8));
    return;
  }
  if (line.startsWith("GW_ROLE ")) {
    handleGatewayRole(line.substring(8));
    return;
  }
  if (line.startsWith("C6_VERSION ")) {
    g_c6Version = line.substring(11).toInt();
    return;
  }
  // Fleet OTA trigger relayed from the mesh by our C6. Schedule it staggered
  // (gateway goes last) so the whole fleet doesn't reboot simultaneously.
  if (line.startsWith("OTA_NOW ")) {
    g_fleetOtaBaseUrl = line.substring(8);
    g_fleetOtaBaseUrl.trim();
    uint32_t delayMs = isActiveGateway ? 90000UL : (5000UL + (uint32_t)random(0, 40000));
    g_fleetOtaAt = millis() + delayMs;
    g_fleetOtaPending = true;
    Serial.printf("[FLEETOTA] scheduled in %lus (gateway=%d) from %s\n",
                  (unsigned long)(delayMs / 1000), isActiveGateway, g_fleetOtaBaseUrl.c_str());
    return;
  }

  // Forward raw logs for debug
  bleNotifyLine(line);

  // --- Thread sensor data relayed from the C6 ---
  //   "[UDP_RX] From [addr]:port -> EUI=<hex>;t=23.1,24.0,err,..."
  // Extract the sensor EUI + temps and forward to the display node /ingest.
  if (line.indexOf("[UDP_RX]") >= 0) {
    int arrow = line.indexOf("-> ");
    if (arrow >= 0 && isActiveGateway) {
      String payload = line.substring(arrow + 3);
      payload.trim();                                   // "EUI=<hex>;t=..."
      if (payload.startsWith("EUI=")) {
        int semi = payload.indexOf(';');
        if (semi > 4) {
          String eui  = payload.substring(4, semi);
          String data = payload.substring(semi + 1);    // "t=23.1,24.0,..."
          forwardReading(eui, data);
        }
      }
    }
    return; // handled
  }

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

    if (bleShouldAdvertise()) {
      NimBLEDevice::startAdvertising();
      Serial.println("[BLE] Restarted Advertising.");
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

        bool pinChanged = false;
        if (oldPin == savedPin) {
          preferences.putString("pin", newPin);
          preferences.putBool("is_setup", true);
          isSessionAuthenticated = true;  // Auto-login after setup
          pinChanged = true;
          bleNotifyLine("ACK SETPIN SUCCESS");
          Serial.println("[AUTH] PIN updated and session unlocked");
        } else {
          bleNotifyLine("ERR SETPIN FAILED");
          Serial.println("[AUTH] SETPIN failed: Old PIN mismatch");
        }
        preferences.end();

        // Replicate the new PIN fleet-wide via the mesh (C6 signs + multicasts),
        // so every router accepts the same admin PIN. Only if Wi-Fi is already
        // provisioned (non-empty SSID); otherwise it goes out with provisioning.
        if (pinChanged) {
          preferences.begin("gateway_config", true);
          String s = preferences.getString("ssid", "");
          String p = preferences.getString("pass", "");
          String z = preferences.getString("zone", "Default");
          String n = preferences.getString("net", "");
          preferences.end();

          if (s.length() > 0) {
            Serial1.printf("cfg_publish %s|%s|%s|%s|%s\n",
                           s.c_str(), p.c_str(), z.c_str(), n.c_str(), newPin.c_str());
            Serial1.flush();
            Serial.println("[AUTH] Published new fleet PIN via mesh.");
          } else {
            Serial.println("[AUTH] PIN set; will replicate once Wi-Fi is provisioned.");
          }
        }
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

    // Commands handled locally on the C3 (MAP, OTA) may arrive signed (the app
    // appends "|<hmac>"); strip it — the authenticated session is the gate here.
    String sCmd = stripTrailingSig(cmdLine);

    // A2. MAP — assign a sensor's EUI to a physical box/slot at commissioning.
    //     Format: "MAP|<EUI16hex>|<box>|<slot>"  e.g. "MAP|58e6c5fffe164ec0|3|A"
    //     Forwarded to the display node (the central EUI->box/slot table).
    if (sCmd.startsWith("MAP|")) {
      int p1 = sCmd.indexOf('|');
      int p2 = sCmd.indexOf('|', p1 + 1);
      int p3 = sCmd.indexOf('|', p2 + 1);
      if (p1 > 0 && p2 > p1 && p3 > p2) {
        String eui  = sCmd.substring(p1 + 1, p2);
        int    box  = sCmd.substring(p2 + 1, p3).toInt();
        String slot = sCmd.substring(p3 + 1);
        eui.toLowerCase();
        slot.toUpperCase();
        bool ok = registerSensorMap(eui, box, slot);
        bleNotifyLine(ok ? "ACK MAP " + eui : "ERR MAP NODE_UNREACHABLE");
        Serial.printf("[MAP] %s -> box%d-%s (%s)\n", eui.c_str(), box, slot.c_str(), ok ? "ok" : "failed");
      } else {
        bleNotifyLine("ERR MAP FORMAT");
      }
      return;
    }

    // A3. OTA — self-update this gateway's C3. A4. OTAC6 — push the C6 image.
    //     Both are QUEUED here and executed in loop() so Serial1 has one reader.
    if (sCmd == "OTA")   { g_pendingOta = 1; bleNotifyLine("OTA QUEUED");   return; }
    if (sCmd == "OTAC6") { g_pendingOta = 2; bleNotifyLine("OTAC6 QUEUED"); return; }

    // A5. OTA_FLEET — roll OTA out to the WHOLE fleet via a signed mesh broadcast.
    if (sCmd == "OTA_FLEET") {
      if (g_nodeUrl.isEmpty()) { bleNotifyLine("ERR OTAFLEET NO_NODE"); return; }
      Serial1.println("ota_broadcast " + g_nodeUrl);   // C6 signs + multicasts it
      Serial1.flush();
      bleNotifyLine("OTA_FLEET BROADCASTING");
      Serial.println("[FLEETOTA] broadcast requested -> " + g_nodeUrl);
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
  // Per-unit name so a human can identify which physical box is the gateway.
  char devName[32];
  uint64_t mac = ESP.getEfuseMac();
  snprintf(devName, sizeof(devName), "%s-%04X", DEVICE_NAME, (uint16_t)(mac & 0xFFFF));

  NimBLEDevice::init(devName);
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
  Serial.printf("[BLE] Stack Initialized & Advertising as %s.\n", devName);
}

// --- BLE management-endpoint gating ---
// Only the ACTIVE gateway (Thread Leader) advertises for management, so the app
// sees a single device. A unit not yet on a network advertises only when placed
// in setup mode (switch), so a fresh unit can still be provisioned.
static bool bleShouldAdvertise() {
  return isActiveGateway || (isCommissionerMode && !c6OnNetwork);
}

static void updateBleAdvertising() {
  bool want = bleShouldAdvertise();
  if (want && !bleStackUp) {
    configureBLE();
    bleStackUp = true;
  } else if (!want && bleStackUp) {
    deinitBLE();
    bleStackUp = false;
  }
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

  // Per-unit seed so fleet-OTA stagger delays differ across devices.
  randomSeed((uint32_t)ESP.getEfuseMac());

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
  String savedDisc = preferences.getString("disc", "");
  preferences.end();
  if (savedDisc.length() > 0) g_discoveryUrl = savedDisc;
  Serial.printf("[BOOT] Discovery server: %s\n", g_discoveryUrl.c_str());

  if (savedSSID.length() > 0) {
    // Do NOT auto-connect here. Wi-Fi is brought up only when the C6 signals
    // GW_ROLE LEADER (this unit is the active gateway). This prevents multiple
    // units from all claiming the uplink. The C6 re-signals role every ~10s.
    Serial.printf("[BOOT] Wi-Fi creds present (SSID: %s). Waiting for GW_ROLE from Commissioner...\n",
                  savedSSID.c_str());
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

  // 0b. Run any queued OTA here (loop context = single Serial1 owner). The C6
  //     push reads OTA_ACK lines synchronously, so it must not race loop()'s read.
  if (g_pendingOta != 0) {
    int op = g_pendingOta;
    g_pendingOta = 0;
    if (op == 1) performOtaCheck();
    else if (op == 2) performOtaC6();
  }

  // 0c. Scheduled fleet OTA (staggered) — run in loop() (single Serial1 owner).
  if (g_fleetOtaPending && (int32_t)(millis() - g_fleetOtaAt) >= 0) {
    g_fleetOtaPending = false;
    Serial.println("[FLEETOTA] starting self-update...");
    performFleetOta(g_fleetOtaBaseUrl);
  }

  // 1. Switch Logic

  bool switchState = digitalRead(SWITCH_PIN);

  if (switchState == HIGH && !isCommissionerMode) {
    isCommissionerMode = true;

    Serial.println("[MODE] Switch ON -> Enter SETUP/COMMISSIONER Mode");
    updateBleAdvertising();   // advertises if fresh/unprovisioned (or already gateway)
    Serial1.println("commissioner_start");
  } else if (switchState == LOW && isCommissionerMode) {
    isCommissionerMode = false;

    Serial.println("[MODE] Switch OFF -> Enter SECURE Mode");
    updateBleAdvertising();   // keeps BLE up only if this unit is the active gateway
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

  // 2b. Gateway stand-down hysteresis: only drop Wi-Fi if STANDBY persisted
  //     past the grace period (a real demotion, not a formation/partition blip).
  if (standbyPending && (millis() - standbyPendingSince >= STANDBY_GRACE_MS)) {
    standbyPending = false;
    isActiveGateway = false;
    Serial.println("[GW] STANDBY confirmed — dropping Wi-Fi + management BLE (no longer the gateway).");
    WiFi.disconnect(true);
    updateBleAdvertising();   // stop advertising; this unit is now a plain router
  }

  // 2c. Active gateway: keep the display-node endpoint fresh and heartbeat the
  //     discovery server (presence == site has internet).
  if (isActiveGateway) {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (now - g_lastDiscover >= DISCOVER_INTERVAL_MS) { g_lastDiscover = now; discoverNode(); }
      if (now - g_lastBeat     >= BEAT_INTERVAL_MS)     { g_lastBeat = now;     heartbeatPresence(); }
    } else if (now - g_lastDiscover >= DISCOVER_INTERVAL_MS) {
      g_lastDiscover = now;
      Serial.printf("[GW] active gateway but Wi-Fi NOT connected (status=%d) — not forwarding\n",
                    WiFi.status());
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