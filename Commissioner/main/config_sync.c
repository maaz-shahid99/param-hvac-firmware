#include "config_sync.h"
#include "config.h"
#include "esp_log.h"
#include "nvs.h"
#include "mbedtls/md.h"
#include "esp_openthread.h"
#include "esp_openthread_lock.h"
#include "openthread/instance.h"
#include "openthread/udp.h"
#include "openthread/ip6.h"
#include "openthread/thread.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_random.h"
#include "esp_mac.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

static const char *TAG = "CFG_SYNC";

// ---- Tunables ----------------------------------------------------------
#define CFG_PORT              1235          // UDP port for config sync (sensor data is on 1234)
#define CFG_MCAST_ADDR        "ff03::1"     // mesh-local all-nodes
#define CFG_NVS_NS            "cfgsync"
#define CFG_TICK_MS           2000          // task loop period
#define CFG_ANNOUNCE_EVERY    15            // leader re-announces every N*TICK (~30s)
#define CFG_ROLE_EVERY        5             // re-signal GW_ROLE every N*TICK (~10s)

#define CFG_SSID_MAX 33
#define CFG_PASS_MAX 65
#define CFG_ZONE_MAX 33
#define CFG_NET_MAX  33
#define CFG_PIN_MAX  17
#define CFG_MSG_MAX  512

// Sentinel meaning "no PIN in this blob / leave the receiver's PIN unchanged".
#define CFG_PIN_NONE "-"

// ---- In-RAM current config --------------------------------------------
typedef struct {
    uint32_t version;
    char ssid[CFG_SSID_MAX];
    char pass[CFG_PASS_MAX];
    char zone[CFG_ZONE_MAX];
    char net [CFG_NET_MAX];
    char pin [CFG_PIN_MAX];   // admin PIN, or CFG_PIN_NONE
} cfg_blob_t;

static cfg_blob_t s_cfg;                 // current accepted config (version 0 = none)

// Pending blob accepted in the RX callback, persisted+relayed by the task.
static volatile bool s_pending_apply = false;
static cfg_blob_t    s_pending;

static otUdpSocket s_sock;
static bool        s_sock_open = false;

// Last fleet-OTA / fleet-reset nonce we acted on (dedup: relay each once).
static uint32_t s_last_ota_nonce = 0;
static uint32_t s_last_reset_nonce = 0;

// ---- Mesh node roster --------------------------------------------------
// Every C6 multicasts "IDENT|<eui>|<G|R>"; the leader aggregates them here and
// reports the roster to its C3. EUI is the factory IEEE EUI-64 (same source the
// sensors use -> matches commissioning). This survives gateway failover: the
// role 'G' simply moves to whichever node is currently the leader.
#define ROSTER_MAX        16
#define ROSTER_WINDOW_MS  60000u      // a node is "live" if heard within this
typedef struct { char eui[17]; char role; uint32_t last_ms; } roster_entry_t;
static roster_entry_t s_roster[ROSTER_MAX];

// Milliseconds since boot from the FreeRTOS tick (avoids an esp_timer dep).
static uint32_t ms_now(void) { return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS); }

// Our own factory EUI-64 as 16 lowercase hex chars (matches the sensors' EUI).
static void own_eui64(char out[17])
{
    uint8_t mac[8] = {0};
    if (esp_read_mac(mac, ESP_MAC_IEEE802154) == ESP_OK) {
        for (int i = 0; i < 8; i++) sprintf(out + i * 2, "%02x", mac[i]);
        out[16] = '\0';
    } else {
        strcpy(out, "0000000000000000");
    }
}

static void roster_note(const char *eui, char role)
{
    uint32_t now = ms_now();
    int oldest = 0;
    for (int i = 0; i < ROSTER_MAX; i++) {
        if (strcmp(s_roster[i].eui, eui) == 0) { s_roster[i].role = role; s_roster[i].last_ms = now; return; }
        if (s_roster[i].eui[0] == '\0') {
            strncpy(s_roster[i].eui, eui, 16); s_roster[i].eui[16] = '\0';
            s_roster[i].role = role; s_roster[i].last_ms = now; return;
        }
        if (s_roster[i].last_ms < s_roster[oldest].last_ms) oldest = i;
    }
    strncpy(s_roster[oldest].eui, eui, 16); s_roster[oldest].eui[16] = '\0';
    s_roster[oldest].role = role; s_roster[oldest].last_ms = now;
}

// ---- HMAC-SHA256 over a string -> 64-char lowercase hex ----------------
static void compute_hmac_hex(const char *msg, char out_hex[65])
{
    uint8_t mac[32];
    const char *key = SECURE_HMAC_KEY;

    mbedtls_md_context_t ctx;
    mbedtls_md_init(&ctx);
    mbedtls_md_setup(&ctx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);
    mbedtls_md_hmac_starts(&ctx, (const unsigned char *)key, strlen(key));
    mbedtls_md_hmac_update(&ctx, (const unsigned char *)msg, strlen(msg));
    mbedtls_md_hmac_finish(&ctx, mac);
    mbedtls_md_free(&ctx);

    for (int i = 0; i < 32; i++) sprintf(out_hex + (i * 2), "%02x", mac[i]);
    out_hex[64] = '\0';
}

// Constant-time-ish compare of two equal-length hex strings.
static bool hmac_equal(const char *a, const char *b)
{
    if (strlen(a) != 64 || strlen(b) != 64) return false;
    uint8_t diff = 0;
    for (int i = 0; i < 64; i++) diff |= (uint8_t)(a[i] ^ b[i]);
    return diff == 0;
}

// ---- NVS persistence ---------------------------------------------------
static void nvs_load(void)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        s_cfg.version = 0;
        return;
    }
    uint32_t ver = 0;
    nvs_get_u32(h, "ver", &ver);
    s_cfg.version = ver;

    size_t n;
    n = sizeof(s_cfg.ssid); if (nvs_get_str(h, "ssid", s_cfg.ssid, &n) != ESP_OK) s_cfg.ssid[0] = '\0';
    n = sizeof(s_cfg.pass); if (nvs_get_str(h, "pass", s_cfg.pass, &n) != ESP_OK) s_cfg.pass[0] = '\0';
    n = sizeof(s_cfg.zone); if (nvs_get_str(h, "zone", s_cfg.zone, &n) != ESP_OK) s_cfg.zone[0] = '\0';
    n = sizeof(s_cfg.net);  if (nvs_get_str(h, "net",  s_cfg.net,  &n) != ESP_OK) s_cfg.net[0]  = '\0';
    n = sizeof(s_cfg.pin);  if (nvs_get_str(h, "pin",  s_cfg.pin,  &n) != ESP_OK) strcpy(s_cfg.pin, CFG_PIN_NONE);
    nvs_close(h);

#if LOG_SENSITIVE
    ESP_LOGI(TAG, "Loaded config v%lu (ssid='%s', net='%s')", (unsigned long)s_cfg.version, s_cfg.ssid, s_cfg.net);
#else
    ESP_LOGI(TAG, "Loaded config v%lu", (unsigned long)s_cfg.version);
#endif
}

static void nvs_store(const cfg_blob_t *c)
{
    nvs_handle_t h;
    if (nvs_open(CFG_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed; cannot persist config");
        return;
    }
    nvs_set_u32(h, "ver", c->version);
    nvs_set_str(h, "ssid", c->ssid);
    nvs_set_str(h, "pass", c->pass);
    nvs_set_str(h, "zone", c->zone);
    nvs_set_str(h, "net",  c->net);
    nvs_set_str(h, "pin",  c->pin);
    nvs_commit(h);
    nvs_close(h);
}

// ---- Relay accepted config to the local C3 over UART -------------------
// NOTE: printed to UART0, which is the link to the C3 bridge. The C3 stores
// these as standby creds. (The password leaves only over the local board-to-
// board UART, never over BLE.)
static void relay_to_c3(const cfg_blob_t *c)
{
    printf("CFG_SET %s|%s|%s|%s|%s\n", c->ssid, c->pass, c->zone, c->net, c->pin);
    fflush(stdout);
}

// ---- Build the signed wire payload: "CFG|ver|ssid|pass|zone|net|hmac" ---
static int build_payload(const cfg_blob_t *c, char *out, size_t out_sz)
{
    char signed_part[CFG_MSG_MAX];
    int n = snprintf(signed_part, sizeof(signed_part), "CFG|%lu|%s|%s|%s|%s|%s",
                     (unsigned long)c->version, c->ssid, c->pass, c->zone, c->net, c->pin);
    if (n < 0 || n >= (int)sizeof(signed_part)) return -1;

    char sig[65];
    compute_hmac_hex(signed_part, sig);

    int m = snprintf(out, out_sz, "%s|%s", signed_part, sig);
    if (m < 0 || m >= (int)out_sz) return -1;
    return m;
}

// ---- Send helpers (caller MUST hold the OpenThread lock) ---------------
static void send_payload_locked(const char *payload, const otIp6Address *dst)
{
    otInstance *inst = esp_openthread_get_instance();
    otMessage *msg = otUdpNewMessage(inst, NULL);
    if (!msg) return;

    if (otMessageAppend(msg, payload, strlen(payload)) != OT_ERROR_NONE) {
        otMessageFree(msg);
        return;
    }

    otMessageInfo info;
    memset(&info, 0, sizeof(info));
    info.mPeerAddr = *dst;
    info.mPeerPort = CFG_PORT;

    if (otUdpSend(inst, &s_sock, msg, &info) != OT_ERROR_NONE) {
        otMessageFree(msg);
    }
}

static void send_multicast_locked(const char *payload)
{
    otIp6Address addr;
    otIp6AddressFromString(CFG_MCAST_ADDR, &addr);
    send_payload_locked(payload, &addr);
}

// ---- Router/gateway environmental (BME) relay --------------------------
void config_sync_send_env(const char *payload)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) return;
    otInstance *inst = esp_openthread_get_instance();
    char eui[17];
    own_eui64(eui);
    otDeviceRole role = otThreadGetDeviceRole(inst);

    char line[300];
    snprintf(line, sizeof(line), "ENV=%s;e=%s", eui, payload ? payload : "");

    if (role == OT_DEVICE_ROLE_LEADER) {
        // We ARE the gateway — hand it straight to our own C3 to forward.
        esp_openthread_lock_release();
        printf("[UDP_RX] From [self]:0 -> %s\n", line);
        fflush(stdout);
        return;
    }

    // Plain router — relay to the gateway over the mesh (sensor/env port 1234).
    otMessage *msg = otUdpNewMessage(inst, NULL);
    if (msg) {
        if (otMessageAppend(msg, line, strlen(line)) == OT_ERROR_NONE) {
            otMessageInfo info;
            memset(&info, 0, sizeof(info));
            otIp6AddressFromString("ff03::2", &info.mPeerAddr);
            info.mPeerPort = 1234;
            if (otUdpSend(inst, &s_sock, msg, &info) != OT_ERROR_NONE)
                otMessageFree(msg);
        } else {
            otMessageFree(msg);
        }
    }
    esp_openthread_lock_release();
}

// ---- Firmware crash report relay (mirrors config_sync_send_env) ---------
void config_sync_send_crash(const char *payload)
{
    if (!esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) return;
    otInstance *inst = esp_openthread_get_instance();
    char eui[17];
    own_eui64(eui);
    otDeviceRole role = otThreadGetDeviceRole(inst);

    char line[400];
    snprintf(line, sizeof(line), "CRASH=%s;c=%s", eui, payload ? payload : "");

    if (role == OT_DEVICE_ROLE_LEADER) {
        esp_openthread_lock_release();
        printf("[UDP_RX] From [self]:0 -> %s\n", line);
        fflush(stdout);
        return;
    }

    otMessage *msg = otUdpNewMessage(inst, NULL);
    if (msg) {
        if (otMessageAppend(msg, line, strlen(line)) == OT_ERROR_NONE) {
            otMessageInfo info;
            memset(&info, 0, sizeof(info));
            otIp6AddressFromString("ff03::2", &info.mPeerAddr);
            info.mPeerPort = 1234;
            if (otUdpSend(inst, &s_sock, msg, &info) != OT_ERROR_NONE)
                otMessageFree(msg);
        } else {
            otMessageFree(msg);
        }
    }
    esp_openthread_lock_release();
}

// ---- Parse + verify an incoming "CFG|..." blob into *out ---------------
static bool parse_and_verify(const char *msg, cfg_blob_t *out)
{
    // Split off the trailing "|<hmac>".
    const char *last = strrchr(msg, '|');
    if (!last) return false;

    size_t signed_len = (size_t)(last - msg);
    if (signed_len == 0 || signed_len >= CFG_MSG_MAX) return false;

    char signed_part[CFG_MSG_MAX];
    memcpy(signed_part, msg, signed_len);
    signed_part[signed_len] = '\0';
    const char *recv_sig = last + 1;

    char expect_sig[65];
    compute_hmac_hex(signed_part, expect_sig);
    if (!hmac_equal(recv_sig, expect_sig)) {
        ESP_LOGW(TAG, "Rejected config: bad signature");
        return false;
    }

    // Tokenize the verified part: CFG | ver | ssid | pass | zone | net
    char work[CFG_MSG_MAX];
    strncpy(work, signed_part, sizeof(work) - 1);
    work[sizeof(work) - 1] = '\0';

    char *save = NULL;
    char *tag  = strtok_r(work, "|", &save);
    char *vers = strtok_r(NULL, "|", &save);
    char *ssid = strtok_r(NULL, "|", &save);
    char *pass = strtok_r(NULL, "|", &save);
    char *zone = strtok_r(NULL, "|", &save);
    char *net  = strtok_r(NULL, "|", &save);
    char *pin  = strtok_r(NULL, "|", &save);

    if (!tag || strcmp(tag, "CFG") != 0 || !vers || !ssid || !pass || !zone || !net || !pin) {
        ESP_LOGW(TAG, "Rejected config: malformed");
        return false;
    }

    memset(out, 0, sizeof(*out));
    out->version = (uint32_t)strtoul(vers, NULL, 10);
    strncpy(out->ssid, ssid, sizeof(out->ssid) - 1);
    strncpy(out->pass, pass, sizeof(out->pass) - 1);
    strncpy(out->zone, zone, sizeof(out->zone) - 1);
    strncpy(out->net,  net,  sizeof(out->net)  - 1);
    strncpy(out->pin,  pin,  sizeof(out->pin)  - 1);
    return true;
}

// ---- UDP receive callback (runs in OpenThread task ctx, lock held) -----
static void cfg_recv_cb(void *ctx, otMessage *msg, const otMessageInfo *info)
{
    char buf[CFG_MSG_MAX];
    uint16_t len = otMessageGetLength(msg) - otMessageGetOffset(msg);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1;
    otMessageRead(msg, otMessageGetOffset(msg), buf, len);
    buf[len] = '\0';

    // Mesh identity announce from a C6 node: "IDENT|<eui16hex>|<G|R>". Every C6
    // multicasts this; the leader aggregates them into the roster it reports to
    // its C3 (so we have REAL factory EUIs + live roles, even across failover).
    if (strncmp(buf, "IDENT|", 6) == 0) {
        char *p = buf + 6;
        char *bar = strchr(p, '|');
        if (bar && (bar - p) == 16) {
            char eui[17];
            memcpy(eui, p, 16);
            eui[16] = '\0';
            roster_note(eui, bar[1] == 'G' ? 'G' : 'R');
        }
        return;
    }

    // A peer is asking for the latest config: answer if we hold one.
    if (strncmp(buf, "CFGREQ", 6) == 0) {
        if (s_cfg.version > 0) {
            char payload[CFG_MSG_MAX];
            if (build_payload(&s_cfg, payload, sizeof(payload)) > 0) {
                send_payload_locked(payload, &info->mPeerAddr);  // unicast reply
            }
        }
        return;
    }

    // Fleet-OTA command: "OTA|<nonce>|<baseurl>|<hmac>"
    if (strncmp(buf, "OTA|", 4) == 0) {
        const char *last = strrchr(buf, '|');
        if (!last) return;
        size_t signed_len = (size_t)(last - buf);
        if (signed_len == 0 || signed_len >= CFG_MSG_MAX) return;

        char signed_part[CFG_MSG_MAX];
        memcpy(signed_part, buf, signed_len);
        signed_part[signed_len] = '\0';

        char expect[65];
        compute_hmac_hex(signed_part, expect);
        if (!hmac_equal(last + 1, expect)) { ESP_LOGW(TAG, "Rejected OTA: bad signature"); return; }

        // Parse nonce + baseurl from the verified part.
        char work[CFG_MSG_MAX];
        strncpy(work, signed_part, sizeof(work) - 1); work[sizeof(work) - 1] = '\0';
        char *save = NULL;
        strtok_r(work, "|", &save);                 // "OTA"
        char *nonce_s = strtok_r(NULL, "|", &save);
        char *baseurl = strtok_r(NULL, "|", &save);
        if (!nonce_s || !baseurl) return;

        uint32_t nonce = (uint32_t)strtoul(nonce_s, NULL, 10);
        if (nonce == s_last_ota_nonce) return;       // already relayed this one
        s_last_ota_nonce = nonce;

        ESP_LOGW(TAG, "Fleet OTA command accepted -> %s", baseurl);
        printf("OTA_NOW %s\n", baseurl);             // tell our C3 to self-update
        fflush(stdout);
        return;
    }

    // Fleet factory-reset command: "RESET|<nonce>|<hmac>"
    if (strncmp(buf, "RESET|", 6) == 0) {
        const char *last = strrchr(buf, '|');
        if (!last) return;
        size_t signed_len = (size_t)(last - buf);
        if (signed_len == 0 || signed_len >= CFG_MSG_MAX) return;

        char signed_part[CFG_MSG_MAX];
        memcpy(signed_part, buf, signed_len);
        signed_part[signed_len] = '\0';

        char expect[65];
        compute_hmac_hex(signed_part, expect);
        if (!hmac_equal(last + 1, expect)) { ESP_LOGW(TAG, "Rejected RESET: bad signature"); return; }

        uint32_t nonce = (uint32_t)strtoul(signed_part + 6, NULL, 10);   // after "RESET|"
        if (nonce == s_last_reset_nonce) return;
        s_last_reset_nonce = nonce;

        ESP_LOGW(TAG, "Fleet factory-reset command accepted");
        printf("RESET_NOW\n");                        // tell our C3 to wipe + reboot
        fflush(stdout);
        return;
    }

    if (strncmp(buf, "CFG|", 4) != 0) return;

    cfg_blob_t incoming;
    if (!parse_and_verify(buf, &incoming)) return;   // strict: bad sig/format dropped

    // Anti-rollback: only accept strictly newer versions.
    uint32_t best = s_pending_apply ? s_pending.version : s_cfg.version;
    if (incoming.version <= best) return;

    s_pending = incoming;
    s_pending_apply = true;   // task persists to NVS + relays to C3
    ESP_LOGI(TAG, "Accepted config v%lu from mesh (pending apply)", (unsigned long)incoming.version);
}

// ---- Tell the local C3 whether we are the active gateway ---------------
static void signal_gateway_role_locked(void)
{
    otInstance *inst = esp_openthread_get_instance();
    bool is_leader = (otThreadGetDeviceRole(inst) == OT_DEVICE_ROLE_LEADER);
    printf("GW_ROLE %s\n", is_leader ? "LEADER" : "STANDBY");
    printf("C6_VERSION %d\n", COMMISSIONER_FW_VERSION);   // so the C3 can version-gate fleet OTA
    fflush(stdout);
}

// ---- Announce our own identity to the mesh (every C6 node) -------------
// Multicasts "IDENT|<eui>|<G|R>" so the current leader can build a roster of
// every C6 (gateway + routers) keyed by real factory EUI. G = we are the active
// leader/gateway, R = we are a (standby) router.
static void announce_identity_locked(void)
{
    otInstance *inst = esp_openthread_get_instance();
    char eui[17];
    own_eui64(eui);
    char role = (otThreadGetDeviceRole(inst) == OT_DEVICE_ROLE_LEADER) ? 'G' : 'R';
    char payload[40];
    snprintf(payload, sizeof(payload), "IDENT|%s|%c", eui, role);
    send_multicast_locked(payload);
}

// ---- Report the mesh-node roster to the local C3 (every node) ----------
// Emits "MESH_NODE <eui> <G|R>" for every live C6 node (ourself + peers heard
// via IDENT). Uses the REAL factory EUI-64 (matches commissioning + sensor
// readings). Sensors are not C6 nodes — they're tracked separately via NODES?.
// EVERY node reports (not just the leader) so the app sees the roster no matter
// which gateway it's connected to (the commissioner isn't always the leader).
// We report ourself with our ACTUAL role; the leader's 'G' reaches everyone via
// its IDENT, so failover just works.
static void report_roster_locked(void)
{
    otInstance *inst = esp_openthread_get_instance();
    bool leader = (otThreadGetDeviceRole(inst) == OT_DEVICE_ROLE_LEADER);

    char self_eui[17];
    own_eui64(self_eui);
    printf("MESH_NODE %s %c\n", self_eui, leader ? 'G' : 'R');   // self, actual role

    uint32_t now = ms_now();
    for (int i = 0; i < ROSTER_MAX; i++) {
        if (s_roster[i].eui[0] == '\0') continue;
        if (strcmp(s_roster[i].eui, self_eui) == 0) continue;        // don't double-report self
        if ((now - s_roster[i].last_ms) > ROSTER_WINDOW_MS) continue; // aged out
        printf("MESH_NODE %s %c\n", s_roster[i].eui, s_roster[i].role);
    }
    fflush(stdout);
}

// ---- Background task: convergence + role heartbeat ---------------------
static void config_sync_task(void *arg)
{
    (void)arg;

    // Boot pull: ask the mesh for the latest config.
    if (esp_openthread_lock_acquire(pdMS_TO_TICKS(200))) {
        send_multicast_locked("CFGREQ");
        esp_openthread_lock_release();
    }

    uint32_t tick = 0;
    while (1) {
        // 1) Persist + relay any config accepted in the RX callback.
        if (s_pending_apply) {
            cfg_blob_t c = s_pending;     // snapshot
            s_pending_apply = false;
            s_cfg = c;
            nvs_store(&c);
            relay_to_c3(&c);
#if LOG_SENSITIVE
            ESP_LOGI(TAG, "Applied config v%lu (ssid='%s')", (unsigned long)c.version, c.ssid);
#else
            ESP_LOGI(TAG, "Applied config v%lu", (unsigned long)c.version);
#endif
        }

        // 2) Periodically re-signal our gateway role, announce our identity to
        //    the mesh, and (if leader) report the aggregated node roster.
        if ((tick % CFG_ROLE_EVERY) == 0) {
            if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
                signal_gateway_role_locked();
                announce_identity_locked();   // every node announces itself
                report_roster_locked();       // leader reports the whole roster
                esp_openthread_lock_release();
            }
        }

        // 3) If we are Leader and hold a config, re-announce for convergence.
        if ((tick % CFG_ANNOUNCE_EVERY) == 0 && s_cfg.version > 0) {
            if (esp_openthread_lock_acquire(pdMS_TO_TICKS(100))) {
                if (otThreadGetDeviceRole(esp_openthread_get_instance()) == OT_DEVICE_ROLE_LEADER) {
                    char payload[CFG_MSG_MAX];
                    if (build_payload(&s_cfg, payload, sizeof(payload)) > 0) {
                        send_multicast_locked(payload);
                    }
                }
                esp_openthread_lock_release();
            }
        }

        tick++;
        vTaskDelay(pdMS_TO_TICKS(CFG_TICK_MS));
    }
}

// ---- Public API --------------------------------------------------------
void config_sync_init(void)
{
    static bool s_inited = false;
    if (s_inited) return;            // safe to call from both the attach path and FORM_NET
    s_inited = true;

    nvs_load();

    // Open + bind the UDP socket. Called from the OpenThread task context
    // (thread_init, before the mainloop) — no explicit lock needed here.
    otInstance *inst = esp_openthread_get_instance();
    memset(&s_sock, 0, sizeof(s_sock));

    if (otUdpOpen(inst, &s_sock, cfg_recv_cb, NULL) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otUdpOpen failed");
        return;
    }

    otSockAddr bind;
    memset(&bind, 0, sizeof(bind));
    bind.mPort = CFG_PORT;
    if (otUdpBind(inst, &s_sock, &bind, OT_NETIF_UNSPECIFIED) != OT_ERROR_NONE) {
        ESP_LOGE(TAG, "otUdpBind failed");
        otUdpClose(inst, &s_sock);
        return;
    }
    s_sock_open = true;

    xTaskCreate(config_sync_task, "cfg_sync", 4096, NULL, 4, NULL);
    ESP_LOGI(TAG, "Config-sync active on UDP %d (current v%lu)", CFG_PORT, (unsigned long)s_cfg.version);
}

void config_sync_publish_local(const char *ssid, const char *pass,
                               const char *zone, const char *net_name,
                               const char *pin)
{
    cfg_blob_t c;
    memset(&c, 0, sizeof(c));
    c.version = s_cfg.version + 1;            // monotonic bump
    strncpy(c.ssid, ssid     ? ssid     : "", sizeof(c.ssid) - 1);
    strncpy(c.pass, pass     ? pass     : "", sizeof(c.pass) - 1);
    strncpy(c.zone, zone     ? zone     : "", sizeof(c.zone) - 1);
    strncpy(c.net,  net_name ? net_name : "", sizeof(c.net)  - 1);

    // PIN: a real value updates the fleet PIN; CFG_PIN_NONE ("-") or NULL means
    // "leave unchanged" -> keep whatever we already had so we don't wipe it.
    if (pin && strcmp(pin, CFG_PIN_NONE) != 0) {
        strncpy(c.pin, pin, sizeof(c.pin) - 1);
    } else {
        strncpy(c.pin, s_cfg.pin[0] ? s_cfg.pin : CFG_PIN_NONE, sizeof(c.pin) - 1);
    }

    s_cfg = c;
    nvs_store(&c);
    relay_to_c3(&c);

    char payload[CFG_MSG_MAX];
    if (build_payload(&c, payload, sizeof(payload)) <= 0) {
        ESP_LOGE(TAG, "publish: payload build failed");
        return;
    }

    if (s_sock_open && esp_openthread_lock_acquire(pdMS_TO_TICKS(500))) {
        send_multicast_locked(payload);
        esp_openthread_lock_release();
        ESP_LOGI(TAG, "Published config v%lu to mesh", (unsigned long)c.version);
    } else {
        ESP_LOGW(TAG, "publish: socket not ready / lock busy (stored locally, will announce when Leader)");
    }
}

void config_sync_broadcast_ota(const char *baseurl)
{
    if (!baseurl || !baseurl[0]) return;

    uint32_t nonce = esp_random();
    char signed_part[CFG_MSG_MAX];
    int n = snprintf(signed_part, sizeof(signed_part), "OTA|%lu|%s",
                     (unsigned long)nonce, baseurl);
    if (n < 0 || n >= (int)sizeof(signed_part)) return;

    char sig[65];
    compute_hmac_hex(signed_part, sig);

    char payload[CFG_MSG_MAX];
    if (snprintf(payload, sizeof(payload), "%s|%s", signed_part, sig) >= (int)sizeof(payload)) return;

    // Mesh-wide: every other node verifies + relays OTA_NOW to its C3.
    if (s_sock_open && esp_openthread_lock_acquire(pdMS_TO_TICKS(500))) {
        send_multicast_locked(payload);
        esp_openthread_lock_release();
        ESP_LOGW(TAG, "Broadcast fleet OTA -> %s", baseurl);
    }

    // The sender doesn't receive its own multicast, so trigger our own C3 too.
    s_last_ota_nonce = nonce;
    printf("OTA_NOW %s\n", baseurl);
    fflush(stdout);
}

void config_sync_broadcast_reset(void)
{
    uint32_t nonce = esp_random();
    char signed_part[CFG_MSG_MAX];
    int n = snprintf(signed_part, sizeof(signed_part), "RESET|%lu", (unsigned long)nonce);
    if (n < 0 || n >= (int)sizeof(signed_part)) return;

    char sig[65];
    compute_hmac_hex(signed_part, sig);

    char payload[CFG_MSG_MAX];
    if (snprintf(payload, sizeof(payload), "%s|%s", signed_part, sig) >= (int)sizeof(payload)) return;

    if (s_sock_open && esp_openthread_lock_acquire(pdMS_TO_TICKS(500))) {
        send_multicast_locked(payload);
        esp_openthread_lock_release();
        ESP_LOGW(TAG, "Broadcast fleet factory-reset");
    }

    // The sender doesn't receive its own multicast, so trigger our own C3 too.
    s_last_reset_nonce = nonce;
    printf("RESET_NOW\n");
    fflush(stdout);
}
