# Commissioner (ESP32-C6) — Thread network brain & MeshCoP commissioner

The **Commissioner** is the ESP32-C6 half of each mesh unit — the 802.15.4 /
**OpenThread** radio brain. It forms (or joins) the Thread mesh, commissions new
joiners, runs leader/gateway election, replicates credentials fleet-wide, and
relays sensor traffic to its paired [Bridge](../Bridge/) (ESP32-C3) over UART.

- **MCU:** Seeed XIAO ESP32-C6 (or H2) — `SOC_IEEE802154_SUPPORTED`.
- **Framework:** ESP-IDF (v5.5.x) + OpenThread.
- **Link to C3:** UART console @ 115200 (commands in, logs/events out).
- **Firmware version:** `COMMISSIONER_FW_VERSION` in `main/config.h`.

## What it does

### 1. Form or auto-join the mesh (`thread_init.c`, `joiner_role.c`)
On boot it decides its role from NVS:
- **Dataset present** -> attach to the existing mesh and start `config_sync`.
- **No dataset** -> run as an FTD **joiner** (`router_joiner_start`, PSKd
  `ROUTER_JOIN_PSKD = "J01NME"`) so a new unit joins an existing network by
  commissioning instead of needing a copied dataset.
- The first/only unit forms a fresh network via `FORM_NET` (idempotent — skips
  re-create if a dataset already exists, avoiding a detach blip).

### 2. Self-healing commissioner (`commissioner.c`, `joiner_manager.c`)
`commissioner_start` / `commissioner_stop` toggle the MeshCoP commissioner.
`commissioner_add_joiner()` is resilient: if the commissioner session has
dropped (the classic `ADD_FAILED 13` after ~120 s) it **re-petitions** and
**queues** the joiner, applying it when the session becomes `ACTIVE` again.

### 3. Leadership-tied gateway role
On Thread role changes it signals the C3 with `GW_ROLE LEADER|STANDBY` so only
the Leader unit runs Wi-Fi + the BLE management endpoint. On leader death the
mesh re-elects and the new Leader's C3 takes over the uplink automatically.

### 4. Fleet-wide signed replication (`config_sync.c`)
A UDP multicast channel (`ff03::1`, port **1235**) carries **HMAC-SHA256-signed**
blobs with a monotonic version (anti-rollback), persisted to NVS:
- **`CFG|...`** — Wi-Fi creds + admin PIN -> every unit (`CFG_SET` to its C3).
- **`OTA|...`** — fleet OTA image URL (`config_sync_broadcast_ota`).
- **`RESET|<nonce>`** — fleet factory reset (`config_sync_broadcast_reset`),
  dedup'd by nonce -> emits `RESET_NOW` to the local C3.

### 5. OTA over UART (`ota_uart.c`)
Receives a new **C6 image pushed from the C3** in base64 chunks with stop-and-wait
ACKs (`OTA_BEGIN/DATA/END/ABORT` -> `OTA_READY/ACK/DONE/ERR`), written via
`esp_ota_*` to the two-OTA partition layout (`partitions.csv`).

### 6. Sensor / environmental / crash relay
The UDP listener receives sensor readings on the mesh (`ff03::2`, port **1234**)
and prints `[UDP_RX] ... EUI=<hex>;t=<csv>` so the C3 can forward them. Two more
payload kinds ride the **same relay** so routers (which have no Wi-Fi) can reach
the cloud via the gateway:
- **`ENV <t>,<h>,<p>,<voc>`** from the C3 → `config_sync_send_env()` tags it with
  the unit's own EUI64 and, on the Leader, loops it back as
  `[UDP_RX] … ENV=<eui>;e=<csv>` (else UDP-relays it to the gateway).
- **`CRASH <reason>|<pc>|<task>`** from the C3 → `config_sync_send_crash()`, same
  pattern, surfacing as `[UDP_RX] … CRASH=<eui>;c=<payload>`.

The gateway C3 turns these into `POST /v1/env` and `POST /v1/crashes`.
Periodically emits `C6_VERSION <n>` for the app's status view.

## UART command reference (from the C3 / console)
| Command | Effect |
|---|---|
| `commissioner_start` / `commissioner_stop` | toggle MeshCoP commissioner |
| `FORM_NET` | form a new Thread network (idempotent) |
| `add <eui> <pskd>` *(signed)* | admit a joiner |
| `cfg_publish <ssid>|<pass>|<zone>|<net>|<pin>` | replicate creds + PIN |
| `ENV <t>,<h>,<p>,<voc>` | relay this unit's BME sample to the gateway/cloud |
| `CRASH <reason>|<pc>|<task>` | relay a firmware crash report to the gateway/cloud |
| `ota_broadcast <url>` | sign + mesh-broadcast a fleet OTA |
| `reset_broadcast` | sign + mesh-broadcast a fleet factory reset |
| `OTA_BEGIN/OTA_DATA/OTA_END/OTA_ABORT` | receive a C6 image over UART |
| `factory_reset` *(signed)* | wipe this C6 |

## Source layout (`main/`)
- `main.c` — app entry; starts commissioner + UDP listener on becoming Leader.
- `thread_init.c` — boot role decision, network form/attach.
- `joiner_role.c` — FTD router auto-join (joiner) path.
- `commissioner.c` / `joiner_manager.c` — self-healing commissioning.
- `config_sync.c/.h` — signed mesh replication (creds/PIN/OTA/reset) + EUI-tagged
  `config_sync_send_env()` / `config_sync_send_crash()` relays.
- `ota_uart.c/.h` — UART OTA receiver.
- `uart_rx.c` — command dispatch from the C3.
- `config.h` — `COMMISSIONER_FW_VERSION`, `SECURE_HMAC_KEY`, `ROUTER_JOIN_PSKD`.

## Building & flashing
```sh
idf.py set-target esp32c6
idf.py build flash monitor
```
Uses a **custom partition table** — `partitions.csv` (two OTA slots, no factory)
with `CONFIG_PARTITION_TABLE_CUSTOM=y` in `sdkconfig`. Both are tracked in the
repo so the build config is reproducible.

> **Security note:** `SECURE_HMAC_KEY` is a placeholder default — change it for
> production, and review the deferred hardening items (enforce `security.c`
> HMAC, gate credential logging, NVS-at-rest encryption, per-device PSKd).

## Related modules
- [Bridge](../Bridge/) — the C3 uplink/BLE partner.
- [thread_commissioner](https://github.com/maaz-shahid99/param-hvac-mobile) — the Flutter control app.
- [SED_SENSOR_BARE](../SED_SENSOR_BARE/) — the sleepy-end-device sensors it commissions.
- [Discovery Server](https://github.com/maaz-shahid99/param-hvac-server) — where readings end up.
