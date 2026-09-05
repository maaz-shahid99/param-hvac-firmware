#include <NimBLEDevice.h>
#include <WiFi.h>
#include <esp_wifi.h>          // esp_wifi_get_ps(): verify power-save actually applied
#include <HTTPClient.h>
#include <WiFiClientSecure.h>   // TLS for HTTPS cloud endpoints
#include <Update.h>
#include <Preferences.h>
#include <ArduinoJson.h>
#include <nvs_flash.h>  // Added for full NVS wipe
#include "mbedtls/base64.h"
#include "esp_task_wdt.h"   // hardware task watchdog (hang protection)
#include "esp_core_dump.h"  // read a saved panic core dump on boot (crash reporting)

// Hardware task-watchdog toggle. If the unit boot-loops right after flashing
// this firmware (resets ~every 30s with a "Task watchdog got triggered" panic),
// set this to 0 and report it — the memory watchdog below still protects you.
#define BRIDGE_ENABLE_TWDT 1
#define BRIDGE_TWDT_TIMEOUT_S 30

// Root CA (PEM) used to validate the cloud server's TLS certificate. Leave
// empty to use setInsecure() — traffic is still encrypted, but the cert is not
// verified (fine for bring-up; PASTE YOUR CA HERE before shipping to customers
// so the gateway can't be MITM'd). Only used when the cloud URL is https://.
static const char *CLOUD_ROOT_CA = "";

// Set to 1 ONLY for bench debugging — gates logging of identifying creds (the
// Wi-Fi SSID) to the serial console. MUST stay 0 in production. Passwords/PINs
// are never logged regardless.
#define LOG_SENSITIVE 0
#if LOG_SENSITIVE
  #define SSID_LOG(s) (s)
#else
  #define SSID_LOG(s) "<redacted>"
#endif

// Bump this on every C3 build you publish; OTA only applies a STRICTLY newer
// c3_version from the manifest. Publishing a build without bumping it means the
// fleet politely refuses the update and reports itself up-to-date.
#define BRIDGE_FW_VERSION 28

// --- Logging ----------------------------------------------------------------
// Severity levels with compile-time filtering: the standard embedded shape, and
// the same one ESP-IDF (esp_log), Zephyr (LOG_*) and the kernel (KERN_*) use.
// Anything above BRIDGE_LOG_LEVEL is removed by the preprocessor, so a disabled
// call costs nothing at all -- no flash for the format string, no CPU, and no
// chance of a log in a hot path perturbing timing-sensitive code.
//
// Deliberately NOT built on Arduino's log_e()/log_i(): those key off the IDE's
// "Core Debug Level" menu, which defaults to None. A build made on a fresh
// machine would ship silent -- errors included -- with nothing in the source to
// explain why. The level belongs in the file, where it is reviewable.
//
// Production ships at INFO, matching ESP-IDF's own default. Errors, warnings and
// state changes (Wi-Fi, gateway role, OTA, auth, boot) stay on the wire, because
// a serial console is the first thing anyone attaches to a misbehaving unit in
// the field; a fully mute build leaves a tech with nothing to read unless the
// unit happens to panic. Per-reading and per-packet chatter compiles out.
//
// Raise to DEBUG for bring-up, VERBOSE for the full C6 UART firehose.
//
// Never pass a credential to these macros at any level. SSIDs go through
// SSID_LOG(); Wi-Fi passwords and the admin PIN are never logged.
#define BRIDGE_LOG_NONE     0
#define BRIDGE_LOG_ERROR    1
#define BRIDGE_LOG_WARN     2
#define BRIDGE_LOG_INFO     3
#define BRIDGE_LOG_DEBUG    4
#define BRIDGE_LOG_VERBOSE  5

#define BRIDGE_LOG_LEVEL    BRIDGE_LOG_INFO

// #if rather than `if (level <= BRIDGE_LOG_LEVEL)`: a dead branch still leaves
// the compiler free to keep the format string in flash, and the point of this is
// that a disabled level leaves no trace in the image.
#if BRIDGE_LOG_LEVEL >= BRIDGE_LOG_ERROR
  #define LOGE(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGE(...) do {} while (0)
#endif
#if BRIDGE_LOG_LEVEL >= BRIDGE_LOG_WARN
  #define LOGW(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGW(...) do {} while (0)
#endif
#if BRIDGE_LOG_LEVEL >= BRIDGE_LOG_INFO
  #define LOGI(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGI(...) do {} while (0)
#endif
#if BRIDGE_LOG_LEVEL >= BRIDGE_LOG_DEBUG
  #define LOGD(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGD(...) do {} while (0)
#endif
#if BRIDGE_LOG_LEVEL >= BRIDGE_LOG_VERBOSE
  #define LOGV(...) Serial.printf(__VA_ARGS__)
#else
  #define LOGV(...) do {} while (0)
#endif

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

// Gateway/Uplink status LED — the one C3-driven LED of the router's four (the
// other three are Power [3V3 rail] + Commissioner & System [C6 GPIO15/GPIO2]).
// External LED on the C3's only free pin, GPIO5 / silk "D3", active-HIGH:
//   off        = standby (not the active gateway)
//   fast blink = active gateway, Wi-Fi connecting
//   slow blink = Wi-Fi up but the cloud is unreachable
//   solid      = active gateway, Wi-Fi + cloud up
static const int UPLINK_LED_PIN = 5;

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

// BLE connect/disconnect flap de-spam: a misbehaving central (e.g. a stale bond
// after a reflash) can connect+drop ~1/s. We print the first cycle (with the
// disconnect reason) then collapse the repeats into one summary every 5s.
static String   g_bleLastCentral;
static uint32_t g_bleFlapCount  = 0;
static uint32_t g_bleLastConnMs = 0;
static uint32_t g_bleSummaryMs  = 0;
static int      g_bleLastReason = 0;
static const uint32_t BLE_FLAP_GAP_MS    = 2500;   // reconnect faster than this from same addr = flapping
static const uint32_t BLE_FLAP_SUMMARY_MS = 5000;  // at most one flap summary this often
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
static void forwardReadingCloud(const String &eui, const String &data);
static void forwardEnvCloud(const String &eui, const String &csv);
static void forwardCrashCloud(const String &eui, const String &payload);

// Turn OFF Wi-Fi modem sleep, and do it after every association.
//
// Arduino defaults a STA to WIFI_PS_MIN_MODEM: the radio powers down between
// DTIM beacons and wakes to listen. That puts power-state transitions right in
// the middle of the MAC's transmit-completion bookkeeping -- which is precisely
// where this unit keeps dying. Five panics, all identical: a load fault in
// lmac_record_txtime called from lmacTxDone, faulting on a pointer that is a
// VALID SRAM address with its top byte cleared (0x3fcd2ffc read as 0x00cd2ffc).
// That is a torn or half-updated pointer, not a wild one -- the signature of
// something reading a word while another context is writing it.
//
// This unit is mains-powered through the gateway, so the ~30 mA that modem sleep
// saves buys us nothing and costs us the entire uplink every hour or so.
//
// Must be re-applied after each connect: esp_wifi_set_ps() does not survive a
// re-init of the driver, and a silently re-enabled power save would look exactly
// like "the fix didn't work".
static void wifiDisableModemSleep() {
  WiFi.setSleep(false);
  // Read it back. A setting that silently failed to apply would be indis-
  // tinguishable from "the theory was wrong", and that is the one confusion
  // this experiment cannot afford.
  wifi_ps_type_t ps = WIFI_PS_NONE;
  if (esp_wifi_get_ps(&ps) == ESP_OK && ps != WIFI_PS_NONE)
    LOGW("[WIFI] modem sleep STILL ON (ps=%d) -- power-save disable did not take\n", (int)ps);
}


// --- v27 experiment: stop the FTM transmit-timestamp hook from running -------
//
// Every panic on this unit that we can map to a build faults on the SAME
// instruction: the halfword load at lmac_record_txtime+0x16. Disassembling the
// libpp.a that ships with core 3.3.8 says what that function is -- the FTM
// (Fine Timing Measurement, 802.11mc ranging) transmit-timestamp hook. Given
// the descriptor of a frame that has just finished transmitting, it walks
//
//     hdr = *(uint32_t *)( *(uint32_t *)(desc + 4) + 4 );
//     if ((hdr[0] & 0xf0) == 0xd0 && ...)      <-- the faulting load
//
// to decide whether the frame was an FTM Action frame, and returns for anything
// else. mtval is always that hdr pointer with its top byte cleared, which is why
// the address tracks the heap: it is a frame buffer.
//
// lmacTxDone only calls the hook when bit 3 of the low word of
// wifi_init_config_t.feature_caps is set -- WIFI_FTM_RESPONDER. Verified
// against this exact library: ftm_is_responder_supported() tests the same word
// and the same bit, ftm_is_initiator_supported() tests bit 2, and the remaining
// readers of that word are the WPA3, 11R, GCMP and enterprise checks.
//
// We are a plain station on a campus AP. We never initiate FTM and never respond
// to it. The hook runs on every frame we transmit purely because the Arduino C3
// sdkconfig ships CONFIG_ESP_WIFI_FTM_RESPONDER_SUPPORT=y, and it is the ONLY
// reader of the field that keeps arriving torn -- nothing on the buffer-recycle
// path touches it, which is why heap poisoning (light, enabled in this build)
// never fires and the heap always looks clean.
//
// So: remove the reader. If the panics stop, the fault was never in the frames
// we send, only in who was inspecting them afterwards.
//
// g_wifi_menuconfig is the Wi-Fi blob's own copy of the init config, filled in
// by esp_wifi_init(). Offset 64 is the low word of feature_caps -- confirmed by
// objdump against THIS library only. Re-check it before carrying this forward to
// any other core version: a silent write to the wrong offset would corrupt an
// unrelated setting with no visible symptom.
// Declared as uint32_t, not uint8_t: a byte array carries alignment 1, so the
// compiler splits the read-modify-write into four byte accesses. This word is
// read by the Wi-Fi task, and a non-atomic write to it is exactly the class of
// bug this firmware is chasing. uint32_t forces a single lw/sw pair.
extern "C" uint32_t g_wifi_menuconfig[];

#define FTM_CAP_BITS  0x0Cu   // WIFI_FTM_INITIATOR (1<<2) | WIFI_FTM_RESPONDER (1<<3)

static bool g_ftmCapsCleared = false;

static void wifiDropFtmCaps() {
  if (g_ftmCapsCleared) return;   // esp_wifi_init() runs once, so this needs to too
  uint32_t *caps = &g_wifi_menuconfig[16];   // byte offset 64 / 4
  const uint32_t before = *caps;
  // Bits already clear means either the driver is not up yet or the offset is
  // wrong. Change nothing and say so: a wrong write is far worse than a failed
  // experiment, and the next connect will try again.
  if ((before & FTM_CAP_BITS) == 0) {
    LOGW("[WIFI] feature_caps=0x%08x -- FTM bits not set, not writing (driver not up, or wrong offset)\n",
         (unsigned)before);
    return;
  }
  *caps = before & ~FTM_CAP_BITS;
  g_ftmCapsCleared = true;
  LOGI("[WIFI] feature_caps 0x%08x -> 0x%08x (FTM tx-time hook disabled)\n",
       (unsigned)before, (unsigned)*caps);
  blog("ftm.off");   // once only -- the breadcrumb ring is a time budget
}


// --- v28 experiment: take 802.11n aggregation out of the TX-done path -------
//
// What v27 proved: clearing the FTM capability bits stopped lmac_record_txtime
// from running, and the panic did not stop -- it MOVED, to pp_coex_tx_release,
// which reads the very same field through the very same pointer chain:
//
//     lw a5,4(a0) ; lw a5,4(a5) ; <byte or halfword load> 0(a5)
//
// Two independent consumers, one corrupt word. So the frame-buffer pointer
// inside the completed TX descriptor is already wrong before anybody reads it.
// Removing readers one at a time is whack-a-mole, and the readers are in a
// binary blob we cannot patch. Confirmed by disassembly that this code is byte
// for byte identical in core 3.3.8 and 3.3.11, so a newer library is not a fix
// either.
//
// So stop removing readers and remove the machinery that owns the descriptors.
// Block-ack aggregation (A-MPDU) is an 802.11n feature: with 11n off the driver
// never aggregates, and ppProcTxDone retires one plain frame at a time instead
// of walking a batch of descriptors whose lifetimes are managed by the
// aggregation and reorder logic. That batching is the most plausible remaining
// source of a descriptor being read after it was recycled.
//
// The cost is a 54 Mbps link ceiling. This gateway posts a few hundred bytes of
// JSON every 30 seconds, so it is free.
//
// Called right after WiFi.mode() has brought the driver up and before we
// associate, and re-applied on every connect for the same reason modem sleep is
// -- a driver re-init would otherwise silently restore the default bitmap and
// make a failed experiment look like a disproved theory.
static bool g_11nOff = false;

static void wifiDisable11n() {
  const uint8_t want = WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G;   // no 11N => no A-MPDU
  esp_err_t e = esp_wifi_set_protocol(WIFI_IF_STA, want);
  if (e != ESP_OK) {
    LOGW("[WIFI] could not drop 11n: %s\n", esp_err_to_name(e));
    return;
  }
  // Read it back. A setting that silently failed to apply is indistinguishable
  // from "the theory was wrong", and that is the one confusion this experiment
  // cannot afford.
  uint8_t got = 0;
  if (esp_wifi_get_protocol(WIFI_IF_STA, &got) == ESP_OK) {
    if (got & WIFI_PROTOCOL_11N) {
      LOGW("[WIFI] 11n STILL ON (bitmap=0x%02x) -- aggregation not disabled\n",
           (unsigned)got);
      return;
    }
    LOGI("[WIFI] 11n/AMPDU disabled (bitmap=0x%02x)\n", (unsigned)got);
  }
  if (!g_11nOff) { g_11nOff = true; blog("11n.off"); }   // once -- the ring is a time budget
}

// --- Discovery / data-forwarding to the display node ---
// The discovery service is now merged onto the cloud server at "<cloud>/discovery",
// so by default we derive it from g_cloudUrl (see deriveDiscoveryUrl()) rather than
// pointing at separate infrastructure — one URL to provision. A site running a
// standalone discovery server can still override this with a "disc" field in the
// PROVISION payload (stored NVS); an explicit override always wins over derivation.
#define DEFAULT_DISCOVERY_URL ""
static String   g_discoveryUrl = DEFAULT_DISCOVERY_URL;
static String   g_nodeUrl      = "";                        // discovered display node e.g. http://192.168.1.60:8001

// Derive the merged discovery URL from the cloud URL ("<cloud>/discovery"). Used
// whenever a site hasn't explicitly provisioned a standalone "disc" override.
static String deriveDiscoveryUrl(const String &cloud) {
  if (cloud.isEmpty()) return "";
  String base = cloud;
  while (base.endsWith("/")) base.remove(base.length() - 1);
  return base + "/discovery";
}

// --- Cloud alerting service (AWS) ---
// Readings are ALSO posted here (in addition to the LAN display node) so the
// cloud threshold engine can alert the customer when racks overheat. Both the
// base URL and the per-site API key are provisioned over BLE (PROVISION payload
// "cloud"/"cloudKey" fields) and stored in NVS. Empty key => cloud POST skipped.
#define DEFAULT_CLOUD_URL ""                                // e.g. https://api.yourdomain.com
static String   g_cloudUrl     = DEFAULT_CLOUD_URL;
static String   g_cloudKey     = "";                        // X-API-Key -> tenant on the cloud
static volatile bool g_cloudOk = true;                      // last cloud POST reached the server (drives the uplink LED)
static uint32_t g_lastDiscover = 0;
static uint32_t g_lastBeat     = 0;
static uint32_t g_lastMesh     = 0;
static uint32_t g_lastWifiWarn = 0;                         // rate-limit the "Wi-Fi NOT connected" line
static const uint32_t DISCOVER_INTERVAL_MS = 10000;
static const uint32_t WIFI_WARN_INTERVAL_MS = 60000;       // warn at most once/min while Wi-Fi is down
static const uint32_t BEAT_INTERVAL_MS     = 10000;
static const uint32_t MESH_PUSH_INTERVAL_MS = 30000;   // push router roster to cloud every 30s

// Exponential backoff for the LAN display-node discovery when it keeps failing
// (a down :8000 used to be re-hit every 10s forever, churning the heap). The
// interval grows 10s->20->40->...->5min on consecutive failures and snaps back
// to 10s on a success. The cloud path (:8002) is independent and unaffected.
static uint32_t g_discoverIntervalMs = DISCOVER_INTERVAL_MS;
static uint8_t  g_discFails          = 0;
static const uint32_t DISCOVER_MAX_INTERVAL_MS = 300000;

// Feature 1: how often this unit forwards its own BME sample over the mesh
// toward the gateway/cloud. Defaults to 60s (the cloud collection interval).
static uint32_t g_lastEnvSend = 0;
static const uint32_t ENV_SEND_INTERVAL_MS = 60000;

// --- Live "seen" sensor table -------------------------------------------------
// Populated from the [UDP_RX] EUI stream we already relay to the display node.
// Answers the app's "NODES?" query so it can offer a dropdown of live sensors
// instead of making the operator type a 16-hex EUI by hand. Mesh-wide, because
// every sensor reading routes to the gateway regardless of which router it
// attached to.
struct SeenNode { String eui; uint32_t lastMs; };
static const int      SEEN_MAX        = 64;
static const uint32_t SEEN_WINDOW_MS  = 30000;    // 30s "live" window (~3 missed 10s reports) — fast offline detect
static SeenNode       g_seen[SEEN_MAX];
static void noteSeenEui(const String &eui);

// --- Per-sensor probe cache ---------------------------------------------------
// Latest probe CSV (after "t=") heard from each sensor, so the app can ask
// "PROBES?<eui>" and get the probe ROMs + temps to build a per-probe assign
// dropdown — without waiting on the (possibly offline) cloud. ROM-tagged for new
// SED firmware ("<rom>:<temp>,..."); a legacy bare-temp CSV is synthesized into
// position roms (idx0,idx1,...) when answered.
struct ProbeCache { String eui; String csv; uint32_t lastMs; };
static const int      PROBE_MAX = 16;
static ProbeCache     g_probes[PROBE_MAX];
static void noteProbes(const String &eui, const String &data);

// --- Live mesh-node table (C6 gateways + routers) -----------------------------
// Populated from the C6 leader's "MESH_NODE <eui> <G|R>" roster lines (the leader
// aggregates self-announced identities mesh-wide). EUIs are real factory EUI-64s
// (match commissioning). role: 'G' = active gateway/leader, 'R' = (standby)
// router. Answers the app's "ROUTERS?" and is forwarded to the cloud (/v1/mesh)
// so the web dashboard shows them too. Survives gateway failover (G moves).
static const uint32_t MESH_WINDOW_MS = 60000;   // 1 min "online" window (roster re-sent ~10s)
struct MeshNode { String eui; char role; uint32_t lastMs; };
static MeshNode       g_mesh[SEEN_MAX];
static void noteMeshNode(const String &eui, char role);

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

// Gateway OTA poll: ask the cloud for a firmware job. Canary builds self-update
// the gateway first; full builds broadcast to the fleet. g_bcast* remembers the
// version we last broadcast so we don't re-trigger every poll (version-gating on
// each unit prevents re-applying anyway).
uint32_t g_lastOtaPoll = 0;
int      g_bcastC3     = 0;   // highest c3 version broadcast to the fleet
int      g_bcastC6     = 0;   // highest c6 version broadcast to the fleet
static const uint32_t OTA_POLL_INTERVAL_MS = 300000;   // 5 min
static void performFleetOta(const String &baseurl);

// Latest commissioner state, tracked from the C6's "COMMISSIONER STATE UPDATE"
// lines, reported to the app via SYS?.  0=unknown, 1=active, 2=disabled.
int g_commState = 0;
volatile bool g_pendingReset = false;   // FACTORY_RESET requested from the BLE task
// Restart requested from the dashboard, collected on the 30s /v1/mesh post.
// 0 = none, 1 = C3 only, 2 = C6 only, 3 = both. Executed from loop(), never
// inline, so an HTTP call is never torn down mid-flight.
volatile int g_pendingReboot = 0;
volatile bool g_pendingScan  = false;   // SCAN? requested from the BLE task (run in loop)
static uint32_t g_pendingScanSince = 0;  // millis of the SCAN? request (0 = none pending)
// millis() of the last association attempt (0 = none yet). A scan started while
// an association is still in flight puts the driver through a channel sweep in
// the middle of the auth/assoc handshake, so the blocking scan waits this out.
static uint32_t g_wifiConnectStartedMs = 0;
// Feature 3: a panic report captured at boot from the core-dump partition,
// "<reset>|<pc>|<bt>". Relayed to the C6 (which tags our EUI) once on-network.
static String g_crashPayload = "";

// --- Crash breadcrumbs (survive a panic reset) ------------------------------
// 123 crash reports so far have told us "pc=0x420ec86e task=wifi" and nothing
// else: `detail` was empty in every single one, and on RISC-V the core-dump
// summary carries no backtrace array (that field is Xtensa-only). So we know
// WHERE it died and nothing about how it got there.
//
// RTC_NOINIT memory is not cleared by a panic/SW reset (only by power-on), so a
// short ring of breadcrumbs written before the fault is still readable on the
// next boot. Kept deliberately small: the whole crash payload has to fit the
// C6's 400-byte relay line, minus the "CRASH=<eui>;c=" prefix.
#define BLOG_SZ     320
#define BLOG_MAGIC  0x424C4F47u          // "BLOG"
RTC_NOINIT_ATTR static char     g_blog[BLOG_SZ];
RTC_NOINIT_ATTR static uint16_t g_blogLen;
RTC_NOINIT_ATTR static uint32_t g_blogMagic;

static void blogReset() {
  g_blogLen = 0;
  g_blog[0] = '\0';
  g_blogMagic = BLOG_MAGIC;
}

// Append one breadcrumb. Always mirrors to Serial, so a bench session sees the
// same trail as a crash report. Not ISR-safe; call from task context only.
static void blog(const char *fmt, ...) {
  char tmp[80];
  va_list ap;
  va_start(ap, fmt);
  int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
  va_end(ap);
  if (n <= 0) return;
  LOGI("[LOG] %s\n", tmp);

  if (g_blogMagic != BLOG_MAGIC || g_blogLen >= BLOG_SZ) blogReset();
  size_t want = strlen(tmp) + 1;                 // +1 for the ';' separator
  if (want >= BLOG_SZ) return;
  // Ring: drop from the FRONT until the new entry fits, so what survives is
  // always the most recent activity before the fault.
  while (g_blogLen + want >= BLOG_SZ) {
    char *cut = (char *) memchr(g_blog, ';', g_blogLen);
    size_t drop = cut ? (size_t)(cut - g_blog) + 1 : g_blogLen;
    memmove(g_blog, g_blog + drop, g_blogLen - drop);
    g_blogLen -= drop;
  }
  if (g_blogLen) g_blog[g_blogLen++] = ';';
  memcpy(g_blog + g_blogLen, tmp, strlen(tmp));
  g_blogLen += strlen(tmp);
  g_blog[g_blogLen] = '\0';
}
static bool   g_crashSent    = false;
static uint32_t g_lastCrashTry = 0;
static uint8_t  g_crashTries   = 0;
static void doFactoryReset();

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
  DynamicJsonDocument doc(768);
  DeserializationError error = deserializeJson(doc, jsonPayload);

  if (error) {
    LOGE("[JSON] Failed to parse provisioning payload\n");
    bleNotifyLine("ERR JSON_INVALID");
    return;
  }

  const char *ssid = doc["ssid"];
  const char *pass = doc["pass"];
  const char *zone = doc["zone"];
  const char *netName = doc["netName"];
  const char *disc = doc["disc"];        // optional: discovery server URL override
  const char *cloud = doc["cloud"];      // optional: cloud alerting service base URL
  const char *cloudKey = doc["cloudKey"];// optional: per-site cloud API key
  const char *wauth = doc["wauth"];      // optional: "psk" (default) | "peap"
  const char *euser = doc["euser"];      // WPA2-Enterprise username (PEAP)
  const char *eid = doc["eid"];          // WPA2-Enterprise outer identity (defaults to euser)

  if (!ssid || !pass || !netName) {
    bleNotifyLine("ERR MISSING_FIELDS");
    return;
  }
  String authMode = (wauth && strcmp(wauth, "peap") == 0) ? "peap" : "psk";

  LOGI("[PROVISION] SSID: %s, Zone: %s, NetName: %s\n", SSID_LOG(ssid), zone, netName);

  // 1. Save to NVS
  preferences.begin("gateway_config", false);
  preferences.putString("ssid", ssid);
  preferences.putString("pass", pass);
  preferences.putString("zone", zone ? zone : "Default");
  preferences.putString("net", netName);
  // Wi-Fi auth mode + enterprise credentials (PEAP/MSCHAPv2).
  preferences.putString("wauth", authMode);
  preferences.putString("euser", (authMode == "peap" && euser) ? euser : "");
  preferences.putString("eid", (authMode == "peap" && eid) ? eid : "");
  if (disc && strlen(disc) > 0) {
    preferences.putString("disc", disc);
    g_discoveryUrl = disc;
  }
  if (cloud && strlen(cloud) > 0) {
    preferences.putString("cloud", cloud);
    g_cloudUrl = cloud;
    if (!disc || strlen(disc) == 0) {          // no explicit override -> derive + persist
      g_discoveryUrl = deriveDiscoveryUrl(g_cloudUrl);
      preferences.putString("disc", g_discoveryUrl);
    }
  }
  if (cloudKey && strlen(cloudKey) > 0) {
    preferences.putString("cloudKey", cloudKey);
    g_cloudKey = cloudKey;
  }
  preferences.end();

  // 2. Connect to Wi-Fi (drop any prior association first so switching to a
  //    different SSID on re-provisioning is reliable).
  LOGI("[WIFI] Connecting to %s...\n", SSID_LOG(ssid));
  bleNotifyLine("STATUS CONNECTING_WIFI");

  // NOTE: this runs on the NimBLE host task (onWrite -> handleProvisioning).
  // Wi-Fi and BLE share one radio on the C3, so reconfiguring the Wi-Fi driver
  // from the BLE task is a genuine hazard; the breadcrumb makes it obvious in
  // the next crash report if that is what we were doing.
  blog("prov.wifi ble-task st=%d", (int)WiFi.status());
  WiFi.mode(WIFI_STA);
  wifiDropFtmCaps();   // before the first frame goes out; see the note above
  wifiDisable11n();    // and no A-MPDU on the tx-done path; see the note above
  WiFi.setAutoReconnect(false);   // we own reconnection; see applyWifi()
  WiFi.disconnect(false, true);   // disassociate + clear stored AP, radio stays up
  delay(100);
  g_wifiConnectStartedMs = millis();
  wifiBeginAuto(ssid, pass, authMode,
                (euser ? String(euser) : String("")), (eid ? String(eid) : String("")));
  wifiDisableModemSleep();

  int retries = 0;
  while (WiFi.status() != WL_CONNECTED && retries < 20) {
    delay(500);
    retries++;
  }

  if (WiFi.status() == WL_CONNECTED) {
    LOGI("[WIFI] Connected!\n");
    bleNotifyLine("WIFI_CONNECTED");

    // 3. Command Commissioner (Air-Gapped!)
    Serial1.printf("FORM_NET %s\n", netName);
    Serial1.flush();
    LOGI("[UART] Sent FORM_NET command\n");

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
    LOGI("[UART] Sent cfg_publish for mesh-wide credential replication\n");

    // This unit just provisioned -> it will become the Leader/active gateway;
    // GW_ROLE LEADER from the C6 will (re)assert Wi-Fi after boot.
    isActiveGateway = true;
  } else {
    LOGE("[WIFI] Failed to connect.\n");
    bleNotifyLine("ERR WIFI_AUTH");
  }
}

// Connect with either WPA2-PSK or WPA2-Enterprise (PEAP/MSCHAPv2), per `wauth`.
// For PEAP the password field carries the EAP password; `euser` is the username
// and `eid` the (optional) outer identity (defaults to the username).
static void wifiBeginAuto(const String &ssid, const String &pass,
                          const String &wauth, const String &euser, const String &eid) {
  if (wauth == "peap" && euser.length() > 0) {
    String id = eid.length() ? eid : euser;
    WiFi.begin(ssid.c_str(), WPA2_AUTH_PEAP, id.c_str(), euser.c_str(), pass.c_str());
  } else {
    WiFi.begin(ssid.c_str(), pass.c_str());
  }
}

// --- Wi-Fi bring-up helper ---
// Cleanly switches APs: dropping any prior association first makes re-provisioning
// to a different SSID reliable on the ESP32. Honours the stored auth type
// (WPA2-PSK or WPA2-Enterprise PEAP) from NVS.
static void applyWifi(const String &ssid, const String &pass) {
  if (ssid.length() == 0) return;
  preferences.begin("gateway_config", true);
  String wauth = preferences.getString("wauth", "psk");
  String euser = preferences.getString("euser", "");
  String eid   = preferences.getString("eid", "");
  preferences.end();
  LOGI("[WIFI] (Re)connecting to %s [%s]...\n", SSID_LOG(ssid.c_str()), wauth.c_str());
  // Breadcrumb either side of the teardown: 119 of 123 panics landed in
  // task=wifi, so knowing whether we were mid-reassociate when it died is the
  // single most useful thing the next crash report can carry.
  blog("wifi.apply st=%d heap=%u", (int)WiFi.status(), (unsigned)ESP.getFreeHeap());
  WiFi.mode(WIFI_STA);
  wifiDropFtmCaps();   // before the first frame goes out; see the note above
  wifiDisable11n();    // and no A-MPDU on the tx-done path; see the note above

  // Own the reconnect policy. The Arduino core re-associates by itself
  // (_autoReconnect defaults to true, STA.cpp), which means the core's network
  // event task can be driving a reconnect at the same instant this function is
  // tearing the association down -- two writers, one driver state. Turn it off
  // so wifiRetryIfDown() below is the single owner.
  WiFi.setAutoReconnect(false);

  // Disassociate WITHOUT stopping the radio. WiFi.disconnect(true) means
  // wifioff=true, i.e. esp_wifi_stop(): the whole driver and its netif are torn
  // down and rebuilt on every retry. That is far more state churn than a
  // reconnect needs, and it is exactly the window in which a stale pointer into
  // freed connection state gets dereferenced. eraseap=true still clears the
  // stored AP config, so re-provisioning to a different SSID stays reliable.
  WiFi.disconnect(false, true);
  delay(100);
  wifiBeginAuto(ssid, pass, wauth, euser, eid);
  wifiDisableModemSleep();
  g_wifiConnectStartedMs = millis();
  blog("wifi.begun");
}

// Scan nearby Wi-Fi APs and reply in one notification for the app's picker:
//   "WIFI|<ssid>:<rssi>:<enc>,..."   enc: 0=open, 1=secured(PSK), 2=enterprise.
// Strongest-first, capped to the 256-byte MTU. Blocking — call only from loop().
static void doWifiScan() {
  WiFi.mode(WIFI_STA);
  blog("wifi.scan start");
  int n = WiFi.scanNetworks(false, true);   // sync, include hidden
  blog("wifi.scan n=%d", n);
  String out = "WIFI|";
  bool first = true;
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    ssid.replace(",", " ");
    ssid.replace(":", " ");
    if (ssid.length() == 0) continue;
    wifi_auth_mode_t m = WiFi.encryptionType(i);
    int enc = (m == WIFI_AUTH_OPEN) ? 0 : (m == WIFI_AUTH_WPA2_ENTERPRISE ? 2 : 1);
    String item = ssid + ":" + String((int)WiFi.RSSI(i)) + ":" + String(enc);
    if (out.length() + item.length() + 2 > 250) break;
    if (!first) out += ",";
    out += item;
    first = false;
  }
  WiFi.scanDelete();
  bleNotifyLine(out);
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

  LOGI("[CFG] Stored replicated creds (ssid=%s, net=%s)\n", SSID_LOG(ssid.c_str()), net.c_str());

  // Replicated admin PIN: store it so this unit accepts the same fleet PIN.
  if (pin.length() > 0 && pin != "-") {
    preferences.begin(AUTH_NAMESPACE, false);
    preferences.putString("pin", pin);
    preferences.putBool("is_setup", true);
    preferences.end();
    LOGI("[CFG] Replicated admin PIN stored (fleet PIN updated).\n");
  }

  // If this unit is currently the active gateway, re-apply with the new creds.
  if (isActiveGateway) applyWifi(ssid, pass);
}

// --- C6 -> C3: gateway-role signal tied to Thread leadership ---
// Payload: "LEADER" (this unit is the active gateway) or "STANDBY".
//
// Re-entrancy guard for the Wi-Fi retry below. The C6 re-sends GW_ROLE about
// every 10s, and WiFi.status() stays non-CONNECTED for several seconds while an
// association is in flight — so the NEXT LEADER line used to see "still down"
// and call applyWifi() again on top of the one already running. applyWifi()
// opens with WiFi.disconnect(true), so the second call tore down the association
// the first was still building, and the wifi task then faulted reading a pointer
// into the freed connection state.
//
// This is not a theory: the first v20 crash trail showed exactly two "wifi.apply"
// breadcrumbs one role-message apart, immediately before a load access fault
// (mcause=0x5) on 0x00cd5a2c — an unmapped address, i.e. a stale pointer. Heap
// was 101 KB at the time, so it was never an exhaustion problem.
//
// A single reconnect attempt is allowed per cooldown; the timer clears the moment
// we observe a live link, so a genuine drop still reconnects promptly.
static const uint32_t WIFI_RETRY_COOLDOWN_MS = 30000;
static uint32_t g_lastWifiRetry = 0;          // millis of the last attempt (0 = none yet)

// The ONE place this unit re-associates from. Both callers (the GW_ROLE handler
// and the gateway service block in loop()) go through here, so the cooldown is
// global rather than per-call-site: previously the role handler held its own
// timer while loop() had no retry at all, which meant a gateway whose C6 stopped
// sending GW_ROLE never reconnected at all.
// Returns true if an attempt was actually started.
static bool wifiRetryIfDown(const char *why) {
  if (WiFi.status() == WL_CONNECTED) {
    g_lastWifiRetry = 0;        // link is up: let the next real drop retry at once
    return false;
  }
  const uint32_t nowMs = millis();
  if (g_lastWifiRetry != 0 && nowMs - g_lastWifiRetry < WIFI_RETRY_COOLDOWN_MS) {
    // Serial only -- deliberately NOT a breadcrumb. loop() calls this on every
    // iteration while the link is down, so a blog() here writes thousands of
    // identical entries and evicts the entire 320-byte ring: the v22 crash trail
    // was nothing but "wifi.retry held (loop.gw)" repeated, which told us
    // nothing. The ring is a time budget, and only state CHANGES may spend it.
    LOGD("[WIFI] retry held (%s)\n", why);
    return false;
  }
  preferences.begin("gateway_config", true);
  String ssid = preferences.getString("ssid", "");
  String pass = preferences.getString("pass", "");
  preferences.end();
  if (ssid.isEmpty()) return false;   // nothing provisioned; don't arm the cooldown
  g_lastWifiRetry = nowMs;
  blog("wifi.retry %s", why);
  applyWifi(ssid, pass);
  return true;
}

static void handleGatewayRole(const String &role) {
  bool wantGateway = role.startsWith("LEADER");
  c6OnNetwork = true;  // any GW_ROLE means our C6 is a network member now

  if (wantGateway) {
    // Leader -> we are (or remain) the active gateway. Cancel any pending
    // stand-down and make sure Wi-Fi + the management BLE are up.
    standbyPending = false;
    if (!isActiveGateway) {
      isActiveGateway = true;
      LOGI("[GW] Now ACTIVE gateway (Leader).\n");
    }
    // Only retry if nothing is already in flight: the ~10s role message rate
    // outruns association, and without the cooldown we re-tear-down mid-connect.
    wifiRetryIfDown("gw.leader");
    updateBleAdvertising();
  } else {
    // STANDBY -> don't drop Wi-Fi immediately; start the grace timer. A
    // freshly-forming gateway reports non-Leader for ~15-20s before promotion,
    // and we must not tear down the uplink we just brought up. loop() drops
    // Wi-Fi only if STANDBY is still in effect after STANDBY_GRACE_MS.
    if (isActiveGateway && !standbyPending) {
      standbyPending = true;
      standbyPendingSince = millis();
      blog("gw.standby grace st=%d", (int)WiFi.status());
      LOGI("[GW] STANDBY received — grace timer started before dropping Wi-Fi.\n");
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
    g_discFails = 0;                              // reachable again -> snap back to 10s
    g_discoverIntervalMs = DISCOVER_INTERVAL_MS;
    DynamicJsonDocument doc(1024);
    if (!deserializeJson(doc, http.getString())) {
      JsonArray fwd = doc["forwarders"].as<JsonArray>();
      if (fwd.size() > 0) {
        String ip = fwd[0]["local_ip"] | "";
        int port = fwd[0]["port"] | 8001;
        if (ip.length()) {
          String nu = "http://" + ip + ":" + String(port);
          if (nu != g_nodeUrl) { g_nodeUrl = nu; LOGI("[DISC] display node -> %s\n", g_nodeUrl.c_str()); }
        }
      } else {
        LOGW("[DISC] no display node registered yet (run display_node.py)\n");
      }
    }
  } else {
    if (g_discFails < 32) g_discFails++;
    uint32_t mult = 1u << (g_discFails > 5 ? 5 : g_discFails);   // 1,2,4,8,16,32
    g_discoverIntervalMs = DISCOVER_INTERVAL_MS * mult;
    if (g_discoverIntervalMs > DISCOVER_MAX_INTERVAL_MS) g_discoverIntervalMs = DISCOVER_MAX_INTERVAL_MS;
    LOGW("[DISC] /discover failed (HTTP %d) @ %s — backing off to %lus\n",
                  code, g_discoveryUrl.c_str(), (unsigned long)(g_discoverIntervalMs / 1000));
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
  // The cloud uplink is INDEPENDENT of the optional LAN display node — forward to
  // the cloud first so readings still flow when no display node is running (the
  // common appliance case). The display-node POST below is best-effort.
  forwardReadingCloud(eui, data);

  if (g_nodeUrl.isEmpty()) return;   // no display node discovered -> skip /ingest only
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(g_nodeUrl + "/ingest")) return;
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"sensor_id\":\"") + eui + "\",\"data\":\"" + data + "\"}";
  int code = http.POST(body);
  http.end();
  if (code > 0) {
    LOGD("[FWD] %s -> %s/ingest (%d)\n", eui.c_str(), g_nodeUrl.c_str(), code);
  } else {
    LOGE("[FWD] %s -> /ingest FAILED (%d) — re-discovering\n", eui.c_str(), code);
    g_nodeUrl = "";   // node unreachable -> force a re-discover (kept: it's an error)
  }
}

// --- Also forward the reading to the cloud alerting service (AWS), if set ---
// Independent of the LAN path: a cloud failure must NOT disturb g_nodeUrl, and a
// missing cloud config simply skips this (LAN dashboard keeps working alone).
static void forwardReadingCloud(const String &eui, const String &data) {
  if (g_cloudUrl.isEmpty() || g_cloudKey.isEmpty()) return;
  const String url  = g_cloudUrl + "/v1/readings";
  const String body = String("{\"sensor_id\":\"") + eui + "\",\"data\":\"" + data + "\"}";
  int code = 0;

  // Build the TLS client ONLY on the https path. Constructing a WiFiClientSecure
  // on every plain-http reading needlessly allocates an mbedTLS context and
  // fragments the heap over hours (a key cause of the C3's slow memory death).
  if (g_cloudUrl.startsWith("https://")) {
    WiFiClientSecure secure;
    if (strlen(CLOUD_ROOT_CA) > 0) secure.setCACert(CLOUD_ROOT_CA);  // verify cert
    else                           secure.setInsecure();             // encrypt only
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(secure, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", g_cloudKey);
    code = http.POST(body);
    http.end();
  } else {
    WiFiClient plain;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(plain, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", g_cloudKey);
    code = http.POST(body);
    http.end();
  }

  g_cloudOk = (code > 0);   // reached the server (any HTTP response) -> uplink LED solid
  if (code > 0) {
    LOGD("[CLOUD] %s -> %s/v1/readings (%d)\n", eui.c_str(), g_cloudUrl.c_str(), code);
  } else {
    LOGE("[CLOUD] %s -> /v1/readings FAILED (%d)\n", eui.c_str(), code);
  }
}

// --- Gateway/Uplink status LED (GPIO5). Non-blocking; self-throttled to ~40 Hz.
// Renders from the gateway role + Wi-Fi + cloud-reachability we already track.
static void updateUplinkLed() {
  static uint32_t lastTick = 0;
  const uint32_t now = millis();
  if (now - lastTick < 25) return;
  lastTick = now;

  bool on;
  if (!isActiveGateway) {
    on = false;                              // standby: off
  } else if (WiFi.status() != WL_CONNECTED) {
    on = ((now / 120) % 2) == 0;             // Wi-Fi connecting: fast blink
  } else if (!g_cloudOk) {
    on = ((now / 500) % 2) == 0;             // cloud unreachable: slow blink
  } else {
    on = true;                               // gateway + cloud up: solid
  }
  digitalWrite(UPLINK_LED_PIN, on ? HIGH : LOW);
}

// --- Forward a router/gateway BME sample to the cloud (Feature 1). Only the
// active gateway runs this (it's the only unit that hears [UDP_RX]). `csv` is
// "<temp>,<hum>,<pres>,<voc>". Memory-light: same http/https split as readings.
static void forwardEnvCloud(const String &eui, const String &csv) {
  if (g_cloudUrl.isEmpty() || g_cloudKey.isEmpty()) return;
  float t = 0, h = 0, p = 0, v = 0;
  int i1 = csv.indexOf(','), i2 = csv.indexOf(',', i1 + 1), i3 = csv.indexOf(',', i2 + 1);
  if (i1 > 0 && i2 > i1 && i3 > i2) {
    t = csv.substring(0, i1).toFloat();
    h = csv.substring(i1 + 1, i2).toFloat();
    p = csv.substring(i2 + 1, i3).toFloat();
    v = csv.substring(i3 + 1).toFloat();
  }
  const String url = g_cloudUrl + "/v1/env";
  const String body = String("{\"sensor_id\":\"") + eui + "\",\"temp\":" + String(t, 2) +
      ",\"hum\":" + String(h, 2) + ",\"pres\":" + String(p, 2) + ",\"voc\":" + String(v, 2) + "}";
  int code = 0;
  if (g_cloudUrl.startsWith("https://")) {
    WiFiClientSecure secure;
    if (strlen(CLOUD_ROOT_CA) > 0) secure.setCACert(CLOUD_ROOT_CA); else secure.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(secure, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", g_cloudKey);
    code = http.POST(body);
    http.end();
  } else {
    WiFiClient plain;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(plain, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", g_cloudKey);
    code = http.POST(body);
    http.end();
  }
  if (code <= 0) LOGE("[ENV] %s -> /v1/env FAILED (%d)\n", eui.c_str(), code);
  else LOGD("[ENV] %s -> /v1/env (%d)\n", eui.c_str(), code);
}

// --- Forward a firmware crash report to the cloud (Feature 3). Only the active
// gateway runs this. `payload` is "<reset>|<pc>|<bt>[|<detail>]".
// The 4th field is optional: a router still running older firmware sends three,
// and must keep working rather than having its report mangled. ---
static void forwardCrashCloud(const String &eui, const String &payload) {
  if (g_cloudUrl.isEmpty() || g_cloudKey.isEmpty()) return;
  int p1 = payload.indexOf('|');
  int p2 = (p1 >= 0) ? payload.indexOf('|', p1 + 1) : -1;
  int p3 = (p2 >= 0) ? payload.indexOf('|', p2 + 1) : -1;
  String reset = p1 > 0 ? payload.substring(0, p1) : payload;
  String pc = (p1 >= 0 && p2 > p1) ? payload.substring(p1 + 1, p2) : "";
  String bt = (p2 >= 0) ? (p3 > p2 ? payload.substring(p2 + 1, p3)
                                   : payload.substring(p2 + 1)) : "";
  String detail = (p3 >= 0) ? payload.substring(p3 + 1) : "";
  detail.replace("\"", "'");      // keep the hand-built JSON below well-formed
  bt.replace("\"", "'");
  const String url = g_cloudUrl + "/v1/crashes";
  const String body = String("{\"sensor_id\":\"") + eui + "\",\"reset_reason\":\"" + reset +
      "\",\"fw\":\"c3-v" + String(BRIDGE_FW_VERSION) + "\",\"pc\":\"" + pc +
      "\",\"backtrace\":\"" + bt + "\",\"detail\":\"" + detail + "\"}";
  int code = 0;
  if (g_cloudUrl.startsWith("https://")) {
    WiFiClientSecure secure;
    if (strlen(CLOUD_ROOT_CA) > 0) secure.setCACert(CLOUD_ROOT_CA); else secure.setInsecure();
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(secure, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", g_cloudKey);
    code = http.POST(body);
    http.end();
  } else {
    WiFiClient plain;
    HTTPClient http;
    http.setConnectTimeout(3000);
    http.setTimeout(3000);
    if (!http.begin(plain, url)) return;
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", g_cloudKey);
    code = http.POST(body);
    http.end();
  }
  LOGE("[CRASH] %s -> /v1/crashes (%d)\n", eui.c_str(), code);
}

// --- Record that we just heard from a sensor EUI (for the NODES? dropdown) ---
static void noteSeenEui(const String &eui) {
  if (eui.isEmpty()) return;
  uint32_t now = millis();
  int oldest = 0;
  for (int i = 0; i < SEEN_MAX; i++) {
    if (g_seen[i].eui == eui) { g_seen[i].lastMs = now; return; }   // refresh
    if (g_seen[i].eui.isEmpty()) { g_seen[i].eui = eui; g_seen[i].lastMs = now; return; }
    if (g_seen[i].lastMs < g_seen[oldest].lastMs) oldest = i;       // track LRU
  }
  g_seen[oldest].eui = eui;                                          // table full -> evict LRU
  g_seen[oldest].lastMs = now;
}

// --- Cache a sensor's latest probe CSV (for the PROBES? dropdown) ---
// [data] is the payload after "EUI=<hex>;" i.e. "t=<rom>:<temp>,..." (or legacy
// "t=<v>,..."); we store the part after "t=".
static void noteProbes(const String &eui, const String &data) {
  if (eui.isEmpty()) return;
  String csv = data;
  int eq = csv.indexOf('=');
  if (eq >= 0) csv = csv.substring(eq + 1);   // drop the "t=" tag
  csv.trim();
  uint32_t now = millis();
  int oldest = 0;
  for (int i = 0; i < PROBE_MAX; i++) {
    if (g_probes[i].eui == eui) { g_probes[i].csv = csv; g_probes[i].lastMs = now; return; }
    if (g_probes[i].eui.isEmpty()) { g_probes[i].eui = eui; g_probes[i].csv = csv; g_probes[i].lastMs = now; return; }
    if (g_probes[i].lastMs < g_probes[oldest].lastMs) oldest = i;
  }
  g_probes[oldest].eui = eui; g_probes[oldest].csv = csv; g_probes[oldest].lastMs = now;
}

// Normalize a cached probe CSV to "<rom>:<temp>,..." for the PROBES? reply. A
// ROM-tagged CSV passes through; a legacy bare-temp CSV gets position roms.
static String probesToRomTemp(const String &csv) {
  if (csv.length() == 0) return "";
  if (csv.indexOf(':') >= 0) return csv;        // already <rom>:<temp>
  String out = "";
  int start = 0, idx = 0;
  while (start <= (int)csv.length()) {
    int comma = csv.indexOf(',', start);
    String tok = (comma < 0) ? csv.substring(start) : csv.substring(start, comma);
    tok.trim();
    if (tok.length() > 0) {
      if (out.length() > 0) out += ",";
      out += "idx" + String(idx++) + ":" + tok;
    }
    if (comma < 0) break;
    start = comma + 1;
  }
  return out;
}

// --- Record a mesh node (C6 gateway/router) the C6 leader reported (ROUTERS?) ---
static void noteMeshNode(const String &eui, char role) {
  if (eui.isEmpty()) return;
  uint32_t now = millis();
  int oldest = 0;
  for (int i = 0; i < SEEN_MAX; i++) {
    if (g_mesh[i].eui == eui) { g_mesh[i].role = role; g_mesh[i].lastMs = now; return; }   // refresh
    if (g_mesh[i].eui.isEmpty()) { g_mesh[i].eui = eui; g_mesh[i].role = role; g_mesh[i].lastMs = now; return; }
    if (g_mesh[i].lastMs < g_mesh[oldest].lastMs) oldest = i;             // track LRU
  }
  g_mesh[oldest].eui = eui;
  g_mesh[oldest].role = role;
  g_mesh[oldest].lastMs = now;
}

// --- Forward the live router roster to the cloud (/v1/mesh), if configured ---
// Mirrors forwardReadingCloud: routers have no readings, so the gateway POSTs
// their presence here so the cloud (and the web dashboard) can show them.
static void forwardMeshCloud() {
  if (g_cloudUrl.isEmpty() || g_cloudKey.isEmpty()) return;
  if (!isActiveGateway) return;   // only the active gateway has the mesh view

  uint32_t now = millis();
  String arr; int n = 0;
  for (int i = 0; i < SEEN_MAX; i++) {
    if (!g_mesh[i].eui.isEmpty() && (now - g_mesh[i].lastMs) < MESH_WINDOW_MS) {
      if (n++) arr += ",";
      arr += String("{\"eui\":\"") + g_mesh[i].eui + "\",\"role\":\"" + String(g_mesh[i].role) + "\"}";
    }
  }
  if (n == 0) return;   // nothing to report; cloud ages nodes out by freshness

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(3000);
  WiFiClientSecure secure;
  WiFiClient       plain;
  bool began;
  if (g_cloudUrl.startsWith("https://")) {
    if (strlen(CLOUD_ROOT_CA) > 0) secure.setCACert(CLOUD_ROOT_CA);
    else                           secure.setInsecure();
    began = http.begin(secure, g_cloudUrl + "/v1/mesh");
  } else {
    began = http.begin(plain, g_cloudUrl + "/v1/mesh");
  }
  if (!began) return;
  http.addHeader("Content-Type", "application/json");
  http.addHeader("X-API-Key", g_cloudKey);
  // Piggyback the gateway's self-report (firmware versions, heap, role) so the
  // support console can show fleet health + the OTA "is there a newer build".
  String body = String("{\"nodes\":[") + arr + "],"
              + "\"fw_c3\":" + String(BRIDGE_FW_VERSION) + ","
              + "\"fw_c6\":" + String(g_c6Version >= 0 ? g_c6Version : 0) + ","
              + "\"heap_free\":" + String((uint32_t)ESP.getFreeHeap()) + ","
              + "\"role\":\"" + (isActiveGateway ? "LEADER" : "STANDBY") + "\"}";
  int code = http.POST(body);
  // The response to THIS post is our only inbound channel: the cloud cannot
  // reach us, so an admin's restart request parks server-side until we collect
  // it here. Every 30 s, already authenticated, and the body was being thrown
  // away — no new endpoint or poll needed.
  String resp = (code == HTTP_CODE_OK) ? http.getString() : String();
  http.end();
  LOGD("[CLOUD] mesh roster (%d nodes) -> %s/v1/mesh (%d)\n", n, g_cloudUrl.c_str(), code);

  if (resp.length()) {
    DynamicJsonDocument rdoc(256);
    if (!deserializeJson(rdoc, resp)) {
      String what = String((const char *)(rdoc["reboot"] | ""));
      if (what.length()) {
        // Queue it; rebooting inside an HTTP call would strand the socket and
        // the WDT bracket in loop() expects to own restarts (see g_pendingReset).
        if      (what == "c3")   g_pendingReboot = 1;
        else if (what == "c6")   g_pendingReboot = 2;
        else if (what == "both") g_pendingReboot = 3;
        LOGI("[CLOUD] restart requested from dashboard: %s\n", what.c_str());
      }
    }
  }
}

// --- Gateway: poll the cloud for a tiered firmware OTA job ------------------
// Mandatory builds auto-roll; optional builds apply only once the customer
// approved them (cloud returns approved_c3/c6). Rollout is staged:
//   stage=canary -> the GATEWAY self-updates first (verify-first), no broadcast;
//   stage=full   -> the gateway tells its C6 to sign + mesh-broadcast a fleet OTA
//                   sourced from the cloud's /firmware/ (every unit self-updates).
// After a canary self-update the gateway reboots onto the new build (version-gating
// then stops the self path); on Promote (stage->full) g_bcast* is 0 post-reboot so
// the gateway broadcasts to the still-behind fleet (which version-gate-skips it).
static void pollOtaJob() {
  if (g_cloudUrl.isEmpty() || g_cloudKey.isEmpty()) return;
  if (!isActiveGateway || WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(4000);
  WiFiClientSecure secure;
  WiFiClient       plain;
  bool began;
  if (g_cloudUrl.startsWith("https://")) {
    if (strlen(CLOUD_ROOT_CA) > 0) secure.setCACert(CLOUD_ROOT_CA);
    else                           secure.setInsecure();
    began = http.begin(secure, g_cloudUrl + "/v1/ota/check");
  } else {
    began = http.begin(plain, g_cloudUrl + "/v1/ota/check");
  }
  if (!began) return;
  http.addHeader("X-API-Key", g_cloudKey);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return; }
  DynamicJsonDocument doc(512);
  DeserializationError err = deserializeJson(doc, http.getString());
  http.end();
  if (err) return;

  if (g_fleetOtaPending) return;               // an OTA is already scheduled/running

  int c3v = doc["c3_version"] | 0;
  int c6v = doc["c6_version"] | 0;
  String c3sev = String((const char *)(doc["c3_severity"] | "optional"));
  String c6sev = String((const char *)(doc["c6_severity"] | "optional"));
  String c3stage = String((const char *)(doc["c3_stage"] | "full"));
  String c6stage = String((const char *)(doc["c6_stage"] | "full"));
  int appC3 = doc["approved_c3"] | 0;
  int appC6 = doc["approved_c6"] | 0;

  // A build we're allowed to roll (mandatory, or an approved optional).
  bool okC3 = (c3v > 0) && (c3sev == "mandatory" || appC3 >= c3v);
  bool okC6 = (c6v > 0) && (c6sev == "mandatory" || appC6 >= c6v);
  // Canary: the gateway updates ITSELF first, only while it's still behind.
  bool selfC3 = okC3 && c3stage == "canary" && c3v > BRIDGE_FW_VERSION;
  bool selfC6 = okC6 && c6stage == "canary" && g_c6Version >= 0 && c6v > g_c6Version;
  // Full: broadcast to the fleet, once per version (independent of OUR version,
  // since the routers may be behind even after the gateway self-updated a canary).
  bool fleetC3 = okC3 && c3stage == "full" && c3v > g_bcastC3;
  bool fleetC6 = okC6 && c6stage == "full" && c6v > g_bcastC6;

  if (selfC3 || selfC6) {
    LOGI("[OTAPOLL] canary -> gateway self-update (c3 v%d, c6 v%d) from cloud\n", c3v, c6v);
    g_fleetOtaBaseUrl = g_cloudUrl;            // self-update only: no ota_broadcast
    g_fleetOtaAt = millis() + 3000;
    g_fleetOtaPending = true;
  } else if (fleetC3 || fleetC6) {
    if (fleetC3) g_bcastC3 = c3v;
    if (fleetC6) g_bcastC6 = c6v;
    LOGI("[OTAPOLL] full -> fleet OTA broadcast (c3 v%d, c6 v%d) from cloud\n", c3v, c6v);
    Serial1.println("ota_broadcast " + g_cloudUrl);   // C6 signs + multicasts OTA_NOW <cloud>
  }
}

// --- Register an EUI -> box/slot mapping on the display node (commissioning) ---
// `label` is the human-readable location (e.g. "Rack A / Unit 1 / Intake 1");
// the box/slot fields stay so the legacy 3D box-grid dashboard keeps working.
static bool registerSensorMap(const String &eui, int box, const String &slot, const String &label) {
  if (g_nodeUrl.isEmpty()) discoverNode();
  if (g_nodeUrl.isEmpty()) return false;
  HTTPClient http;
  http.setConnectTimeout(2000);
  http.setTimeout(2000);
  if (!http.begin(g_nodeUrl + "/map")) return false;
  http.addHeader("Content-Type", "application/json");
  String body = String("{\"eui\":\"") + eui + "\",\"box\":" + String(box) +
                ",\"slot\":\"" + slot + "\",\"label\":\"" + label + "\"}";
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
  // Refuse an image whose size we cannot check. Nothing in this path verifies
  // the payload -- there is no sha256 in the manifest -- so Content-Length is
  // the only integrity signal available. Without it, a connection that dies
  // half-way still ends with Update.end(true) succeeding (the header the ESP32
  // validates lives in the first few KB), and the unit reboots into a truncated
  // image with no working uplink left to recover over.
  if (len <= 0) {
    LOGE("[OTA] server sent no Content-Length; refusing unverifiable image\n");
    bleNotifyLine("ERR OTA NO_LENGTH");
    http.end();
    return;
  }
  if (!Update.begin((size_t)len)) {
    bleNotifyLine("ERR OTA NOSPACE");
    http.end();
    return;
  }

  LOGI("[OTA] Downloading C3 image (%d bytes)...\n", len);
  bleNotifyLine("OTA DOWNLOADING");

  WiFiClient *stream = http.getStreamPtr();
  size_t written = Update.writeStream(*stream);
  http.end();

  // A short read means a truncated download. Bail out BEFORE Update.end(true),
  // which is the call that marks the new partition bootable.
  if (written != (size_t)len) {
    LOGE("[OTA] truncated: %u of %d bytes; aborting\n", (unsigned)written, len);
    Update.abort();
    bleNotifyLine("ERR OTA TRUNCATED " + String((unsigned)written) + "/" + String(len));
    return;
  }

  if (!Update.end(true)) {
    LOGE("[OTA] failed: %s\n", Update.errorString());
    bleNotifyLine("ERR OTA " + String(Update.getError()));
    return;
  }

  LOGI("[OTA] C3 update OK (%u bytes). Rebooting...\n", (unsigned)written);
  bleNotifyLine("OTA SUCCESS REBOOTING");
  delay(500);
  ESP.restart();
}

// Read the image filename for one chip out of a firmware manifest.
//
// Two spellings exist in the field: the old display node published "<kind>_file"
// while the cloud server published "<kind>file". Reading only one of them is how
// OTA came to fail silently for every build -- an absent key yields "", which the
// callers below treat as "nothing to install", so the gateway announced itself
// up-to-date while never downloading a thing. Accept either, and let the LOG say
// which was found so the next mismatch is visible instead of mute.
static String manifestFile(JsonDocument &doc, const char *kind) {
  const char *v = doc[String(kind) + "_file"] | "";
  if (v && *v) return String(v);
  v = doc[String(kind) + "file"] | "";
  return String(v ? v : "");
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
  String c3file = manifestFile(doc, "c3");
  LOGI("[OTA] manifest c3_version=%d file='%s' (running %d)\n",
                c3ver, c3file.c_str(), BRIDGE_FW_VERSION);

  // A manifest that advertises a version but no filename is a SERVER fault, not
  // an up-to-date fleet. Reporting it as up-to-date is what hid the key mismatch.
  if (c3file.isEmpty()) { bleNotifyLine("ERR OTA NO_FILE v" + String(c3ver)); return; }
  if (c3ver <= BRIDGE_FW_VERSION) { bleNotifyLine("OTA UP_TO_DATE"); return; }

  bleNotifyLine("OTA UPDATING v" + String(c3ver));
  otaC3FromUrl(g_nodeUrl + "/firmware/" + c3file);
}

// ===== Phase 2: stream a C6 firmware image to the C6 over UART =====

static void flushSerial1() { while (Serial1.available()) Serial1.read(); }

// In quiet mode, only echo C6 UART lines that carry a real signal — warnings,
// errors, joiner/network/config events — not the routine sensor-data dumps and
// role/version/mesh heartbeats. (The OpenThread "[W] ... Security" warning is
// kept via "[W]" + "Fail".)
static bool isNotableC6Line(const String &s) {
  return s.indexOf("ERR")  >= 0 || s.indexOf("Error") >= 0 ||
         s.indexOf("Fail") >= 0 || s.indexOf("Reject") >= 0 ||
         s.indexOf("[W]")  >= 0 || s.indexOf("[E]")  >= 0 ||
         s.indexOf("JOIN") >= 0 || s.indexOf("NETWORK_FORMED") >= 0 ||
         s.indexOf("CFG_PUBLISHED") >= 0 || s.indexOf("PANIC") >= 0 ||
         s.indexOf("assert") >= 0;
}

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
          LOGI("[OTAC6] C6 reported: %s\n", line.c_str());
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

  LOGI("[OTAC6] streaming %d bytes to C6...\n", total);
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

  LOGI("[OTAC6] C6 update complete; C6 is rebooting.\n");
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
    if (!deserializeJson(doc, http.getString())) c6file = manifestFile(doc, "c6");
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
    if (s.isEmpty()) { LOGE("[FLEETOTA] no Wi-Fi creds; abort\n"); return; }
    applyWifi(s, p);
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
    if (WiFi.status() != WL_CONNECTED) { LOGE("[FLEETOTA] Wi-Fi failed; abort\n"); return; }
  }

  HTTPClient http;
  http.setConnectTimeout(3000);
  http.setTimeout(5000);
  if (!http.begin(baseurl + "/firmware/manifest.json")) { LOGE("[FLEETOTA] manifest begin fail\n"); return; }
  if (http.GET() != HTTP_CODE_OK) { LOGE("[FLEETOTA] manifest HTTP fail\n"); http.end(); return; }
  DynamicJsonDocument doc(512);
  DeserializationError e = deserializeJson(doc, http.getString());
  http.end();
  if (e) { LOGE("[FLEETOTA] manifest json fail\n"); return; }

  int c3ver = doc["c3_version"] | 0; String c3file = manifestFile(doc, "c3");
  int c6ver = doc["c6_version"] | 0; String c6file = manifestFile(doc, "c6");
  LOGI("[FLEETOTA] manifest c3=%d '%s' (run %d), c6=%d '%s' (run %d)\n",
                c3ver, c3file.c_str(), BRIDGE_FW_VERSION,
                c6ver, c6file.c_str(), g_c6Version);
  // Say so out loud: a newer build we can't name is a broken manifest, and the
  // "complete (or already up-to-date)" line below would otherwise bury it.
  if (c3ver > BRIDGE_FW_VERSION && c3file.isEmpty())
    LOGW("[FLEETOTA] c3 update advertised but manifest names no file -- skipping\n");
  if (c6ver > g_c6Version && c6file.isEmpty())
    LOGW("[FLEETOTA] c6 update advertised but manifest names no file -- skipping\n");

  // C6 first (it reboots independently; the C3 stays up to stream it).
  if (!c6file.isEmpty() && c6ver > g_c6Version) {
    otaC6FromUrl(baseurl + "/firmware/" + c6file);
  }
  // C3 last (this reboots us).
  if (!c3file.isEmpty() && c3ver > BRIDGE_FW_VERSION) {
    otaC3FromUrl(baseurl + "/firmware/" + c3file);
  }
  LOGI("[FLEETOTA] complete (or already up-to-date)\n");
}

// Wipe this unit (C3 NVS + tell the C6 to wipe) and reboot.
static void doFactoryReset() {
  LOGW("\n[SYSTEM] === FACTORY RESET INITIATED ===\n");
  nvs_flash_erase();
  nvs_flash_init();
  Serial1.println("factory_reset");      // wipe the C6 too (matches its UART command)
  Serial1.flush();
  LOGW("[SYSTEM] NVS cleared + Commissioner reset sent. Rebooting...\n");
  deinitBLE();
  WiFi.disconnect(true);
  delay(2000);
  ESP.restart();
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

  LOGD("[STATE] Pending add set for EUI64=%s\n", g_pendingEui64.c_str());
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
  // Mesh roster line from the C6 leader: "MESH_NODE <eui> <G|R>" (G = active
  // gateway/leader, R = router). Track every C6 node so we can answer the app's
  // ROUTERS? and forward the roster to the cloud. (Sensors are not C6 nodes —
  // they're tracked via their readings / NODES?.)
  if (line.startsWith("MESH_NODE ")) {
    int sp1 = line.indexOf(' ');
    int sp2 = line.indexOf(' ', sp1 + 1);
    if (sp1 > 0 && sp2 > sp1) {
      String eui  = line.substring(sp1 + 1, sp2);
      String type = line.substring(sp2 + 1);
      type.trim();
      noteMeshNode(eui, (type == "G") ? 'G' : 'R');
    }
    return;
  }
  // Fleet factory-reset relayed from the mesh by our C6 -> wipe + reboot.
  if (line.startsWith("RESET_NOW")) {
    doFactoryReset();   // does not return
    return;
  }
  // Track commissioner state (for SYS?) from the C6's status lines.
  if (line.indexOf("COMMISSIONER STATE UPDATE") >= 0) {
    if (line.indexOf("ACTIVE") >= 0)        g_commState = 1;
    else if (line.indexOf("DISABLED") >= 0) g_commState = 2;
  }
  // Fleet OTA trigger relayed from the mesh by our C6. Schedule it staggered
  // (gateway goes last) so the whole fleet doesn't reboot simultaneously.
  if (line.startsWith("OTA_NOW ")) {
    g_fleetOtaBaseUrl = line.substring(8);
    g_fleetOtaBaseUrl.trim();
    uint32_t delayMs = isActiveGateway ? 90000UL : (5000UL + (uint32_t)random(0, 40000));
    g_fleetOtaAt = millis() + delayMs;
    g_fleetOtaPending = true;
    LOGI("[FLEETOTA] scheduled in %lus (gateway=%d) from %s\n",
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
    if (arrow >= 0) {
      String payload = line.substring(arrow + 3);
      payload.trim();                                   // "EUI=<hex>;t=..."
      if (payload.startsWith("EUI=")) {
        int semi = payload.indexOf(';');
        if (semi > 4) {
          String eui  = payload.substring(4, semi);
          String data = payload.substring(semi + 1);    // "t=23.1,24.0,..."
          // Track the sensor on ANY gateway that hears it (the app may connect
          // to the commissioner, which isn't always the active leader) so NODES?
          // is never empty. Only the ACTIVE gateway forwards (avoids duplicates).
          noteSeenEui(eui);
          noteProbes(eui, data);                        // cache probe ROMs for PROBES?
          LOGV("[SEEN+] %s\n", eui.c_str());
          if (isActiveGateway) forwardReading(eui, data);
        }
      } else if (payload.startsWith("ENV=")) {
        // Router/gateway BME relayed by the C6: "ENV=<eui>;e=<t>,<h>,<p>,<voc>"
        int semi = payload.indexOf(';');
        if (semi > 4) {
          String eui = payload.substring(4, semi);
          String e = payload.substring(semi + 1);       // "e=<t>,<h>,<p>,<voc>"
          if (e.startsWith("e=")) e = e.substring(2);
          if (isActiveGateway) forwardEnvCloud(eui, e);
        }
      } else if (payload.startsWith("CRASH=")) {
        // Firmware crash relayed by the C6: "CRASH=<eui>;c=<reset>|<pc>|<bt>"
        int semi = payload.indexOf(';');
        if (semi > 6) {
          String eui = payload.substring(6, semi);
          String c = payload.substring(semi + 1);        // "c=<reset>|<pc>|<bt>"
          if (c.startsWith("c=")) c = c.substring(2);
          if (isActiveGateway) forwardCrashCloud(eui, c);
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
    LOGI("[PROTO] %s\n", ack.c_str());
    bleNotifyLine(ack);

    g_pendingAdd = false;
    g_pendingEui64 = "";
    return;
  }

  // --- NEW: Check for Joiner Removal (Timeout) ---
  // Matches the new "JOINER_EVENT REMOVED" log we added in commissioner.c
  if (line.indexOf("JOINER_EVENT REMOVED") >= 0) {
    String err = "ERR ADD " + g_pendingEui64 + " timeout";
    LOGW("[PROTO] %s\n", err.c_str());
    bleNotifyLine(err);

    g_pendingAdd = false;
    g_pendingEui64 = "";
    return;
  }

  // 3. Check for Generic Errors
  if (line.indexOf("ERROR") >= 0 || line.indexOf("FAILED") >= 0) {
    String err = "ERR ADD " + g_pendingEui64 + " commissioner_error";
    LOGE("[PROTO] %s\n", err.c_str());
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

    String addr = connInfo.getAddress().toString().c_str();
    uint32_t now = millis();
    bool flapping = (addr == g_bleLastCentral) && (now - g_bleLastConnMs < BLE_FLAP_GAP_MS);
    if (flapping) {
      g_bleFlapCount++;
      if (now - g_bleSummaryMs >= BLE_FLAP_SUMMARY_MS) {   // collapse the storm
        g_bleSummaryMs = now;
        LOGW("[BLE] %s flapping — %lu connect/drop cycles (last reason=%d); check pairing/bond\n",
                      addr.c_str(), (unsigned long)g_bleFlapCount, g_bleLastReason);
      }
    } else {
      g_bleFlapCount = 0;
      g_bleSummaryMs = now;
      LOGI("[BLE] Connected: %s\n", addr.c_str());
    }
    g_bleLastCentral = addr;
    g_bleLastConnMs = now;
  }

  void onDisconnect(NimBLEServer *server, NimBLEConnInfo &connInfo, int reason) override {
    bleClientConnected = false;
    bleClientSecured = false;
    isSessionAuthenticated = false;  // Clear session state
    g_bleLastReason = reason;

    // Quiet during a flap storm (the periodic summary above covers it); otherwise
    // log the disconnect WITH its reason code (e.g. 13=remote term, 8=supervision
    // timeout, 61/0x3d=encryption/MIC failure => stale bond).
    if (g_bleFlapCount == 0) {
      LOGI("[BLE] Disconnected (reason=%d)\n", reason);
    }

    if (bleShouldAdvertise()) {
      NimBLEDevice::startAdvertising();
      if (g_bleFlapCount == 0) LOGD("[BLE] Restarted Advertising.\n");
    }
  }

  void onAuthenticationComplete(NimBLEConnInfo &connInfo) override {
    if (!connInfo.isEncrypted()) {
      LOGW("[BLE] Auth failed/unencrypted. Disconnecting.\n");
      NimBLEDevice::getServer()->disconnect(connInfo.getConnHandle());
      bleClientSecured = false;
      return;
    }
    bleClientSecured = true;
    LOGI("[BLE] Secured Link Established (OS-Level).\n");
    bleNotifyLine("BRIDGE READY");
  }
};

class BridgeCharacteristicCallbacks : public NimBLECharacteristicCallbacks {
  void onWrite(NimBLECharacteristic *pChar, NimBLEConnInfo &connInfo) override {
    // 1. OS-Level Security Check
    if (!bleClientSecured) {
      LOGW("[BLE] Rejected write (Link Not Secured)\n");
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
        LOGI("[AUTH] Session Unlocked\n");
      } else {
        bleNotifyLine("ERR AUTH FAILED");
        LOGW("[AUTH] Failed login attempt\n");
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
          LOGI("[AUTH] PIN updated and session unlocked\n");
        } else {
          bleNotifyLine("ERR SETPIN FAILED");
          LOGW("[AUTH] SETPIN failed: Old PIN mismatch\n");
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
            LOGI("[AUTH] Published new fleet PIN via mesh.\n");
          } else {
            LOGI("[AUTH] PIN set; will replicate once Wi-Fi is provisioned.\n");
          }
        }
      } else {
        bleNotifyLine("ERR SETPIN FORMAT");
      }
      return;
    }

    // Strip any trailing "|<hmac>" so we can recognise the command for gating.
    // (Commands handled locally on the C3 — MAP, OTA — may arrive signed.)
    String sCmd = stripTrailingSig(cmdLine);

    // Read-only STATUS queries are allowed WITHOUT authentication: they expose
    // only device lists / status (sensor + mesh EUIs, roles, versions, link
    // state) — no secrets, no control — so the app can populate its Devices view
    // and the assign dropdown the moment it connects, even before unlocking.
    bool isReadOnlyQuery = (sCmd == "NODES?" || sCmd == "ROUTERS?" || sCmd == "SYS?" ||
                          sCmd == "SCAN?" || sCmd.startsWith("PROBES?"));

    // SCAN? — list nearby Wi-Fi networks for the Router Setup picker. Scanning is
    // blocking (~2-4s), so defer to loop() and notify the result there.
    if (sCmd == "SCAN?") {
      g_pendingScan = true;
      bleNotifyLine("STATUS SCANNING");
      return;
    }

    // ==========================================
    // 3. THE GATEKEEPER
    // ==========================================
    // Everything that changes state requires an authenticated session.
    if (!isSessionAuthenticated && !isReadOnlyQuery) {
      LOGW("[BLE] Rejected write (App-Level Unauthenticated)\n");
      bleNotifyLine("ERR UNAUTHENTICATED");
      return;
    }

    // ==========================================
    // 4. SECURED COMMANDS
    // ==========================================

    // A. PROVISION
    if (cmdLine.startsWith("PROVISION|")) {
      LOGI("[BLE] Received Provisioning Payload\n");
      String jsonPart = cmdLine.substring(10);
      handleProvisioning(jsonPart);
      return;
    }

    // A1. NODES? — reply with the live sensor EUIs we've recently seen so the
    //     app can offer a device dropdown. Chunked to survive the BLE MTU.
    if (sCmd == "NODES?") {
      // ONE notification (one line) so nothing is dropped by back-to-back BLE
      // notifies: "NODES|<eui>,<eui>,..." (empty list -> "NODES|"). Fits the
      // 256-byte MTU for ~14 sensors.
      String resp = "NODES|";
      uint32_t now = millis();
      int count = 0;
      for (int i = 0; i < SEEN_MAX; i++) {
        if (!g_seen[i].eui.isEmpty() && (now - g_seen[i].lastMs) < SEEN_WINDOW_MS) {
          if (count++) resp += ",";
          resp += g_seen[i].eui;
        }
      }
      bleNotifyLine(resp);
      LOGD("[NODES?] replied %d live\n", count);   // diag
      return;
    }

    // A1b. ROUTERS? — reply with the C6 mesh nodes the leader currently sees
    //      (gateway + routers), each with its role, so the app can show them +
    //      online status. Chunked like NODES?. Line: "ROUTER|<eui>|<G|R>".
    if (sCmd == "ROUTERS?") {
      // ONE notification: "ROUTERS|<eui>:<role>,..." (role G/R). Empty -> "ROUTERS|".
      String resp = "ROUTERS|";
      uint32_t now = millis();
      for (int i = 0; i < SEEN_MAX; i++) {
        if (!g_mesh[i].eui.isEmpty() && (now - g_mesh[i].lastMs) < MESH_WINDOW_MS) {
          if (resp.length() > 8) resp += ",";
          resp += g_mesh[i].eui + ":" + String(g_mesh[i].role);
        }
      }
      bleNotifyLine(resp);
      return;
    }

    // A1c. PROBES?<eui> — reply with one sensor's last-heard probe ROMs + temps
    //      so the app can offer a per-probe assign dropdown. One notification:
    //      "PROBES|<eui>|<rom>:<temp>,..." (empty list -> trailing "|").
    if (sCmd.startsWith("PROBES?")) {
      String eui = sCmd.substring(7);
      eui.trim();
      eui.toLowerCase();
      String resp = "PROBES|" + eui + "|";
      uint32_t now = millis();
      for (int i = 0; i < PROBE_MAX; i++) {
        if (g_probes[i].eui == eui && (now - g_probes[i].lastMs) < SEEN_WINDOW_MS) {
          resp += probesToRomTemp(g_probes[i].csv);
          break;
        }
      }
      bleNotifyLine(resp);
      LOGD("[PROBES?] %s\n", eui.c_str());
      return;
    }

    // A2. MAP — assign a sensor's EUI to a physical location at commissioning.
    //     Format: "MAP|<EUI16hex>|<box>|<slot>[|<label>]"
    //       e.g. "MAP|58e6c5fffe164ec0|3|A|Rack A / Unit 1 / Intake 1"
    //     box/slot drive the legacy dashboard grid; label is the rich location.
    //     Forwarded to the display node (the central EUI->location table).
    if (sCmd.startsWith("MAP|")) {
      int p1 = sCmd.indexOf('|');
      int p2 = sCmd.indexOf('|', p1 + 1);
      int p3 = sCmd.indexOf('|', p2 + 1);
      if (p1 > 0 && p2 > p1 && p3 > p2) {
        int p4 = sCmd.indexOf('|', p3 + 1);             // optional 4th field
        String eui   = sCmd.substring(p1 + 1, p2);
        int    box   = sCmd.substring(p2 + 1, p3).toInt();
        String slot  = (p4 > p3) ? sCmd.substring(p3 + 1, p4) : sCmd.substring(p3 + 1);
        String label = (p4 > p3) ? sCmd.substring(p4 + 1) : "";
        eui.toLowerCase();
        slot.toUpperCase();
        bool ok = registerSensorMap(eui, box, slot, label);
        bleNotifyLine(ok ? "ACK MAP " + eui : "ERR MAP NODE_UNREACHABLE");
        LOGD("[MAP] %s -> box%d-%s (%s) %s\n",
                      eui.c_str(), box, slot.c_str(), ok ? "ok" : "failed", label.c_str());
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
      LOGI("[FLEETOTA] broadcast requested -> %s\n", g_nodeUrl.c_str());
      return;
    }

    // A6. OTA_SELF — update THIS unit (C6 then C3), reusing the fleet scheduler.
    if (sCmd == "OTA_SELF") {
      if (g_nodeUrl.isEmpty()) { bleNotifyLine("ERR OTA NO_NODE"); return; }
      g_fleetOtaBaseUrl = g_nodeUrl;
      g_fleetOtaAt      = millis();
      g_fleetOtaPending = true;
      bleNotifyLine("OTA_SELF QUEUED");
      return;
    }

    // A7. SYS? — report this unit's status to the app.
    if (sCmd == "SYS?") {
      String s = "SYS|role=";
      s += (isActiveGateway ? "LEADER" : "STANDBY");
      s += "|c3="  + String(BRIDGE_FW_VERSION);
      s += "|c6="  + String(g_c6Version);
      s += "|comm=" + String(g_commState == 1 ? "ACTIVE" : (g_commState == 2 ? "DISABLED" : "?"));
      s += "|wifi=" + String(WiFi.status() == WL_CONNECTED ? 1 : 0);
      s += "|node=" + String(g_nodeUrl.isEmpty() ? 0 : 1);
      bleNotifyLine(s);
      return;
    }

    // A8. FACTORY_RESET — wipe this unit. RESET_FLEET — wipe the whole fleet.
    if (sCmd == "FACTORY_RESET") {
      bleNotifyLine("FACTORY_RESET OK");
      g_pendingReset = true;            // run from loop() (not the BLE callback)
      return;
    }
    if (sCmd == "RESET_FLEET") {
      Serial1.println("reset_broadcast");   // C6 signs + multicasts; each unit wipes
      Serial1.flush();
      bleNotifyLine("RESET_FLEET BROADCASTING");
      return;
    }

    // B. ADD (Busy Check)
    if (g_pendingAdd && cmdLine.startsWith("add ")) {
      LOGW("[BLE] Rejecting add: Busy\n");
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
    LOGD("[UART] Forwarded full command (%d bytes)\n", cmdLine.length());
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
  LOGI("[BLE] Stack Initialized & Advertising as %s.\n", devName);
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
  LOGI("[BLE] Stack De-initialized (Secure Mode).\n");
}

// ===========================================================================
// HEALTH / MEMORY WATCHDOG
// The C3 runs BLE + Wi-Fi + HTTP in ~400 KB RAM. Over many hours the heap can
// fragment/exhaust until Wi-Fi's allocator faults (Guru Meditation in
// esf_buf_alloc_dynamic) and BLE advertising silently dies. Instead of letting
// it hard-crash unpredictably, we watch free heap + the largest free block and
// GRACEFULLY reboot just before the danger zone (and once every 12h as a
// backstop). A reboot is a clean ~3s recovery: BLE re-advertises and the C6/mesh
// + cloud reconnect on their own. We also re-assert advertising if it ever stops
// while it should be up, so the app is never permanently locked out.
// ===========================================================================
static const uint32_t HEALTH_CHECK_MS       = 5000;     // evaluate every 5s
static const uint32_t HEALTH_LOG_MS         = 60000;    // print a [HEAP] line each 60s
static const uint32_t HEALTH_FREE_FLOOR     = 16000;    // free heap < 16 KB  => danger
static const uint32_t HEALTH_BLOCK_FLOOR    = 12000;    // largest block < 12 KB => danger
static const uint32_t HEALTH_PERSIST_MS     = 8000;     // stay critical this long (ignore blips)
static const uint32_t HEALTH_FORCE_AFTER_MS = 120000;   // reboot even with a client connected after this
static const uint32_t HEALTH_MAX_UPTIME_MS  = 12UL * 60 * 60 * 1000;  // 12h idle backstop

static uint32_t g_healthLastCheck = 0;
static uint32_t g_healthLastLog   = 0;
static uint32_t g_criticalSince   = 0;   // millis when memory first went critical (0 = healthy)

static void safeReboot(const char *why) {
  LOGW("[HEALTH] REBOOT: %s (free=%u largest=%u up=%lus)\n",
                why, ESP.getFreeHeap(), ESP.getMaxAllocHeap(),
                (unsigned long)(millis() / 1000));
  Serial.flush();
  delay(150);
  ESP.restart();   // does not return
}

// Re-assert advertising if this unit SHOULD be advertising but isn't.
static void bleSelfHeal() {
  if (!bleShouldAdvertise()) return;
  if (!bleStackUp) { updateBleAdvertising(); return; }
  NimBLEAdvertising *adv = NimBLEDevice::getAdvertising();
  if (adv && !adv->isAdvertising() && !bleClientConnected) {
    NimBLEDevice::startAdvertising();
    LOGW("[BLE] self-heal: advertising was down -> restarted.\n");
  }
}

static void healthMonitor() {
  uint32_t now = millis();
  if (now - g_healthLastCheck < HEALTH_CHECK_MS) return;
  g_healthLastCheck = now;

  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t largest  = ESP.getMaxAllocHeap();

  if (now - g_healthLastLog >= HEALTH_LOG_MS) {
    g_healthLastLog = now;
    LOGD("[HEAP] free=%u min=%u largest=%u up=%lus\n",
                  freeHeap, ESP.getMinFreeHeap(), largest, (unsigned long)(now / 1000));
  }

  bleSelfHeal();

  // Never reboot mid firmware-update.
  if (g_pendingOta != 0 || g_fleetOtaPending) { g_criticalSince = 0; return; }

  // 12h proactive backstop, only when no app is connected (zero disruption).
  if (now >= HEALTH_MAX_UPTIME_MS && !bleClientConnected) {
    safeReboot("12h uptime backstop");
  }

  // Memory danger zone — debounced so a momentary RX burst doesn't trip it.
  bool critical = (freeHeap < HEALTH_FREE_FLOOR) || (largest < HEALTH_BLOCK_FLOOR);
  if (!critical) { g_criticalSince = 0; return; }
  if (g_criticalSince == 0) {
    g_criticalSince = now;
    LOGE("[HEALTH] memory critical (free=%u largest=%u) — reboot pending\n", freeHeap, largest);
    return;
  }
  if (now - g_criticalSince < HEALTH_PERSIST_MS) return;          // wait out a blip
  if (!bleClientConnected) safeReboot("memory critical");
  else if (now - g_criticalSince >= HEALTH_FORCE_AFTER_MS)
    safeReboot("memory critical (client connected)");
}

// --- Feature 3: capture a saved panic core dump at boot ---------------------
// The Arduino min_spiffs scheme already has a `coredump` partition, so a panic
// is saved there automatically. On the next boot we read its summary (PC +
// backtrace), stash it, and relay it to the cloud (via the C6) once on-network.
static void captureCrashAtBoot() {
  esp_reset_reason_t rr = esp_reset_reason();
  const char *reason;
  switch (rr) {
    case ESP_RST_PANIC:    reason = "panic";    break;
    case ESP_RST_INT_WDT:  reason = "int_wdt";  break;
    case ESP_RST_TASK_WDT: reason = "task_wdt"; break;
    case ESP_RST_WDT:      reason = "wdt";      break;
    case ESP_RST_BROWNOUT: reason = "brownout"; break;
    case ESP_RST_SW:       reason = "sw";       break;
    case ESP_RST_POWERON:  reason = "poweron";  break;
    default:               reason = "other";    break;
  }
  LOGI("[BOOT] reset reason: %s\n", reason);

  String pc = "", bt = "", detail = "";
  if (esp_core_dump_image_check() == ESP_OK) {
    esp_core_dump_summary_t *sum =
        (esp_core_dump_summary_t *) malloc(sizeof(esp_core_dump_summary_t));
    if (sum && esp_core_dump_get_summary(sum) == ESP_OK) {
      char tmp[24];
      snprintf(tmp, sizeof(tmp), "0x%08x", (unsigned)sum->exc_pc);
      pc = tmp;
      // RISC-V (C3) doesn't fill a backtrace array (that's Xtensa-only), so the
      // PC alone gave one frame and no reason. The summary DOES carry the
      // exception registers, and we were discarding them:
      //   mcause -> WHY it trapped (illegal instruction / load / store fault)
      //   mtval  -> the faulting address
      //   ra     -> the return address, i.e. the CALLER — a second frame
      // That turns "it died somewhere in the wifi task" into an actual lead.
      bt = String("task=") + String(sum->exc_task);
#if defined(__riscv)
      char regs[96];
      snprintf(regs, sizeof(regs), "mcause=0x%x mtval=0x%08x ra=0x%08x sp=0x%08x",
               (unsigned)sum->ex_info.mcause, (unsigned)sum->ex_info.mtval,
               (unsigned)sum->ex_info.ra, (unsigned)sum->ex_info.sp);
      detail = regs;
#endif
      // First bytes of the app ELF hash: proves which build an address belongs
      // to. Decoding a PC against the wrong ELF gives a confidently wrong answer.
      char sha[16];   // " elf=" + 8 hex + NUL = 14; the old [12] silently clipped a byte
      snprintf(sha, sizeof(sha), " elf=%02x%02x%02x%02x",
               sum->app_elf_sha256[0], sum->app_elf_sha256[1],
               sum->app_elf_sha256[2], sum->app_elf_sha256[3]);
      detail += sha;
    }
    if (sum) free(sum);
    // The abort REASON — the one thing the summary omits. On a deliberate abort
    // (assert / ESP_ERROR_CHECK / stack canary / FreeRTOS overflow) pc lands in
    // panic_abort and mcause=2 is merely the `unimp` the panic handler executes,
    // so pc/ra name the messenger, not the sender. The v21 tiT crash decoded to
    // exactly that: panic_abort <- esp_system_abort <- ???. IDF stores the
    // reason string in the dump header; read it before the erase below. It goes
    // BEFORE the trail so it survives the 300-byte relay truncation — the trail
    // has already told its story, the reason is what we are still missing.
    {
      char why[128];
      if (esp_core_dump_get_panic_reason(why, sizeof(why)) == ESP_OK && why[0]) {
        for (char *c = why; *c; ++c) if (*c == '\n' || *c == '\r') *c = ' ';
        detail += " reason=";
        detail += why;
      }
    }
    esp_core_dump_image_erase();   // report it once
  }

  bool isCrash = (rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT ||
                  rr == ESP_RST_TASK_WDT || rr == ESP_RST_WDT || rr == ESP_RST_BROWNOUT);

  // Breadcrumbs from before the fault. Only meaningful across a reset that
  // preserves RTC memory — a power-on cycle leaves this uninitialised, so the
  // magic check is what stops us reporting garbage as a trail.
  if (isCrash && g_blogMagic == BLOG_MAGIC && g_blogLen > 0) {
    g_blog[g_blogLen < BLOG_SZ ? g_blogLen : BLOG_SZ - 1] = '\0';
    detail += " trail=";
    detail += g_blog;
  }
  blogReset();                      // fresh trail for this boot

  // The C6 relays this in a 400-byte line ("CRASH=<16 hex>;c=" ~ 24 of them).
  // Truncate here rather than letting snprintf silently cut it over there.
  if (detail.length() > 300) detail = detail.substring(0, 300);
  // '|' separates the payload fields, so it must not appear inside one.
  detail.replace('|', '/');

  if (isCrash || pc.length() > 0) {
    g_crashPayload = String(reason) + "|" + pc + "|" + bt + "|" + detail;
    LOGE("[CRASH] captured: %s pc=%s %s\n", reason, pc.c_str(), detail.c_str());
  }
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
  pinMode(UPLINK_LED_PIN, OUTPUT);
  digitalWrite(UPLINK_LED_PIN, LOW);   // off until we become the active gateway

  LOGI("\n[BOOT] Bridge Starting...\n");
  LOGI("[BOOT] C3 fw v%d — single-notify NODES?/ROUTERS?/PROBES?; 30s live window\n", BRIDGE_FW_VERSION);
  captureCrashAtBoot();   // Feature 3: read a saved panic core dump, forward later

  // Initialize Authentication Defaults if first boot
  preferences.begin(AUTH_NAMESPACE, false);
  if (!preferences.isKey("is_setup")) {
    LOGI("[BOOT] First boot detected. Initializing Auth NVS.\n");
    preferences.putBool("is_setup", false);
    preferences.putString("pin", DEFAULT_PIN);
  }
  preferences.end();

  // Load Wi-Fi Gateway Config
  preferences.begin("gateway_config", true);
  String savedSSID = preferences.getString("ssid", "");
  String savedPass = preferences.getString("pass", "");
  String savedDisc = preferences.getString("disc", "");
  String savedCloud = preferences.getString("cloud", "");
  String savedCloudKey = preferences.getString("cloudKey", "");
  preferences.end();
  if (savedDisc.length() > 0) g_discoveryUrl = savedDisc;
  if (savedCloud.length() > 0) g_cloudUrl = savedCloud;
  if (savedCloudKey.length() > 0) g_cloudKey = savedCloudKey;
  // Back-compat: a device provisioned before discovery was merged onto the cloud
  // server has "cloud" saved but no "disc" — derive it now instead of falling
  // back to the old hardcoded dev IP (which no longer exists).
  if (savedDisc.isEmpty() && savedCloud.length() > 0) {
    g_discoveryUrl = deriveDiscoveryUrl(g_cloudUrl);
  }
  LOGI("[BOOT] Discovery server: %s\n", g_discoveryUrl.c_str());
  LOGI("[BOOT] Cloud alerting: %s (key %s)\n",
                g_cloudUrl.isEmpty() ? "(none)" : g_cloudUrl.c_str(),
                g_cloudKey.isEmpty() ? "unset" : "set");

  if (savedSSID.length() > 0) {
    // Do NOT auto-connect here. Wi-Fi is brought up only when the C6 signals
    // GW_ROLE LEADER (this unit is the active gateway). This prevents multiple
    // units from all claiming the uplink. The C6 re-signals role every ~10s.
    LOGI("[BOOT] Wi-Fi creds present (SSID: %s). Waiting for GW_ROLE from Commissioner...\n",
                  SSID_LOG(savedSSID.c_str()));
  }

  LOGI("[BOOT] free heap: %u bytes\n", ESP.getFreeHeap());
#if BRIDGE_ENABLE_TWDT
  // Widen the Arduino default 5s TWDT and subscribe the loop task, so a true
  // hang (not just low memory) also auto-recovers. OTA brackets this in loop().
  esp_task_wdt_config_t twdt = {
    .timeout_ms = BRIDGE_TWDT_TIMEOUT_S * 1000,
    .idle_core_mask = (1 << 0),
    .trigger_panic = true,
  };
  esp_task_wdt_reconfigure(&twdt);
  esp_task_wdt_add(NULL);
  LOGI("[BOOT] task watchdog armed (%ds).\n", BRIDGE_TWDT_TIMEOUT_S);
#endif
}

void loop() {
#if BRIDGE_ENABLE_TWDT
  esp_task_wdt_reset();   // we're alive
#endif
  healthMonitor();        // heap watch + graceful reboot + BLE self-heal
  updateUplinkLed();      // gateway/Wi-Fi/cloud status LED (GPIO5)

  // ==========================================
  // 0. FACTORY RESET LOGIC (1-Second Hold)
  // ==========================================
  if (digitalRead(RESET_BTN_PIN) == LOW) {  // Button is pressed (pulled to ground)
    if (!resetBtnPressed) {
      resetBtnPressed = true;
      resetBtnPressTime = millis();
      LOGW("[SYSTEM] Reset button pressed. Hold for 1s to factory reset...\n");
    } else if (millis() - resetBtnPressTime >= 10000) {
      doFactoryReset();   // wipe C3 + C6 and reboot (does not return)
    }
  } else {
    if (resetBtnPressed) {
      resetBtnPressed = false;  // Reset the timer if released early
      LOGI("[SYSTEM] Reset button released. Reset aborted.\n");
    }
  }

  static char lineBuf[UART_MAX_LINE_LEN];
  static size_t lineLen = 0;

#if BRIDGE_ENABLE_TWDT
  bool _otaThisLoop = (g_pendingOta != 0) ||
                      (g_fleetOtaPending && (int32_t)(millis() - g_fleetOtaAt) >= 0);
  if (_otaThisLoop) esp_task_wdt_delete(NULL);   // OTA blocks for minutes; don't trip the WDT
#endif

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
    LOGI("[FLEETOTA] starting self-update...\n");
    performFleetOta(g_fleetOtaBaseUrl);
  }
#if BRIDGE_ENABLE_TWDT
  if (_otaThisLoop) { esp_task_wdt_add(NULL); esp_task_wdt_reset(); }
#endif

  // 0d. Factory reset requested over BLE — run from loop() then reboot.
  if (g_pendingReset) {
    g_pendingReset = false;
    doFactoryReset();   // does not return
  }

  // 0d2. Restart requested from the dashboard (collected on the /v1/mesh post).
  //      C6 first: it reboots independently and we want the command on the wire
  //      before our own restart tears the UART down. On "both" we never reach
  //      the log line — safeReboot does not return.
  if (g_pendingReboot) {
    int what = g_pendingReboot;
    g_pendingReboot = 0;
    if (what == 2 || what == 3) {
      LOGI("[SYSTEM] Restarting C6 (dashboard request)...\n");
      Serial1.println("reboot");
      Serial1.flush();
      delay(200);              // let the C6 read the line before we go
    }
    if (what == 1 || what == 3) safeReboot("dashboard restart request");
    LOGI("[SYSTEM] C6 restart sent; C3 staying up.\n");
  }

  // 0e. Wi-Fi scan requested over BLE (blocking) — run here, notify the result.
  if (g_pendingScan) {
    // A scan sweeps every channel and monopolises the radio. Running one while
    // an association is still being negotiated aborts that handshake inside the
    // driver. Hold the request until the attempt settles -- but never longer
    // than the association timeout, or a phone asking for a network list while
    // the AP is unreachable would wait forever.
    const uint32_t nowMs = millis();
    if (g_pendingScanSince == 0) g_pendingScanSince = nowMs;
    const bool connecting = g_wifiConnectStartedMs != 0 &&
                            WiFi.status() != WL_CONNECTED &&
                            nowMs - g_wifiConnectStartedMs < 12000;
    if (!connecting || nowMs - g_pendingScanSince >= 15000) {
      if (connecting) blog("wifi.scan forced (assoc still pending)");
      g_pendingScan = false;
      g_pendingScanSince = 0;
      doWifiScan();
    }
  }

  // 0f. Relay a captured boot crash report to the C6 (which tags our EUI and
  //     routes it to the gateway/cloud). The gateway forwards it itself, so wait
  //     for Wi-Fi (it isn't up the instant we become gateway) and retry a few
  //     times; a router relays via the mesh and doesn't need its own Wi-Fi.
  if (!g_crashSent && g_crashPayload.length() > 0 && c6OnNetwork &&
      (!isActiveGateway || WiFi.status() == WL_CONNECTED) &&
      (g_crashTries == 0 || millis() - g_lastCrashTry >= 20000)) {
    g_lastCrashTry = millis();
    Serial1.printf("CRASH %s\n", g_crashPayload.c_str());
    if (++g_crashTries >= 6) g_crashSent = true;   // best-effort: give up after ~100s
  }

  // 1. Switch Logic

  bool switchState = digitalRead(SWITCH_PIN);

  if (switchState == HIGH && !isCommissionerMode) {
    isCommissionerMode = true;

    LOGI("[MODE] Switch ON -> Enter SETUP/COMMISSIONER Mode\n");
    updateBleAdvertising();   // advertises if fresh/unprovisioned (or already gateway)
    Serial1.println("commissioner_start");
  } else if (switchState == LOW && isCommissionerMode) {
    isCommissionerMode = false;

    LOGI("[MODE] Switch OFF -> Enter SECURE Mode\n");
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

#if BRIDGE_LOG_LEVEL >= BRIDGE_LOG_VERBOSE
      LOGV("[UART Rx] %s\n", lineBuf);              // firehose: every C6 line
#else
      if (isNotableC6Line(line))
        LOGI("[C6] %s\n", lineBuf);                 // signal only: warn/error/joiner/net
#endif
      handleCommissionerLine(line);

      lineLen = 0;

    } else {
      if (lineLen < UART_MAX_LINE_LEN - 1) {
        lineBuf[lineLen++] = (char)ch;

      } else {
        LOGE("[UART] Overflow dropped\n");
        lineLen = 0;
      }
    }
  }

  // 2b. Gateway stand-down hysteresis: only drop Wi-Fi if STANDBY persisted
  //     past the grace period (a real demotion, not a formation/partition blip).
  if (standbyPending && (millis() - standbyPendingSince >= STANDBY_GRACE_MS)) {
    standbyPending = false;
    isActiveGateway = false;
    LOGI("[GW] STANDBY confirmed — dropping Wi-Fi + management BLE (no longer the gateway).\n");
    blog("gw.standby drop st=%d", (int)WiFi.status());
    WiFi.disconnect(true);       // deliberate here: a standby router powers the radio down
    // We just dropped the link on purpose. Clearing the cooldown means a
    // STANDBY -> LEADER flap (Thread leadership moving back within the 30s
    // window) reconnects immediately, instead of sitting uplink-less waiting out
    // a timer armed by a retry we no longer care about.
    g_lastWifiRetry = 0;
    updateBleAdvertising();   // stop advertising; this unit is now a plain router
  }

  // 2c. Active gateway: keep the display-node endpoint fresh and heartbeat the
  //     discovery server (presence == site has internet).
  if (isActiveGateway) {
    uint32_t now = millis();
    if (WiFi.status() == WL_CONNECTED) {
      if (now - g_lastDiscover >= g_discoverIntervalMs) { g_lastDiscover = now; discoverNode(); }
      // Skip the presence heartbeat while discovery is clearly down (same server)
      // so we don't churn a failing connection every 10s.
      if (g_discFails < 2 && now - g_lastBeat >= BEAT_INTERVAL_MS) { g_lastBeat = now; heartbeatPresence(); }
      if (now - g_lastMesh     >= MESH_PUSH_INTERVAL_MS){ g_lastMesh = now;     forwardMeshCloud(); }
      if (now - g_lastOtaPoll  >= OTA_POLL_INTERVAL_MS) { g_lastOtaPoll = now;  pollOtaJob(); }
    } else {
      // Auto-reconnect is off (see applyWifi), so the uplink comes back only if
      // we ask for it. GW_ROLE used to be the sole trigger; if the C6 goes quiet
      // that never arrives and the gateway stays dark. Retry from here too --
      // same cooldown, so the two paths cannot stack.
      wifiRetryIfDown("loop.gw");
      if (now - g_lastWifiWarn >= WIFI_WARN_INTERVAL_MS) {
        g_lastWifiWarn = now;   // at most once/min, not every discover cycle
        LOGW("[GW] active gateway but Wi-Fi NOT connected (status=%d) — not forwarding\n",
                      WiFi.status());
      }
    }
  }

  // 3. Pending Timeout Check (Bridge failsafe)
  // Only fires if we never got a "REMOVED" or "ADDED" message from Comm.

  if (g_pendingAdd && (int32_t)(millis() - g_pendingDeadlineMs) >= 0) {
    bleNotifyLine("ERR ADD TIMEOUT");

    LOGW("[PROTO] Timed out waiting for JOINER_ADDED\n");
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
        LOGD("[SD] logged %s %s\n", dateStr.c_str(), timeStr.c_str());
    } else {
        LOGE("[SD] log write FAILED\n");
    }

    // Feature 1: forward this BME sample toward the gateway/cloud over the mesh.
    // The C6 tags it with our EUI and routes it (router -> gateway -> cloud; the
    // gateway loops its own back). Interval-gated so it doesn't flood the UART.
    if (c6OnNetwork && millis() - g_lastEnvSend >= ENV_SEND_INTERVAL_MS) {
      g_lastEnvSend = millis();
      Serial1.printf("ENV %s\n", dataLine.c_str());      // "ENV <t>,<h>,<p>,<voc>"
      LOGD("[ENV->C6] %s (gw=%d)\n", dataLine.c_str(), isActiveGateway);  // debug
    }
}

  delay(5);
}