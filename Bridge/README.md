# Bridge (ESP32-C3) — gateway uplink & BLE management endpoint

The **Bridge** is the ESP32-C3 half of each mesh unit. It pairs over UART with
the [Commissioner](../Commissioner/) (ESP32-C6, the Thread radio brain) and owns
everything the C6 can't: **Wi-Fi uplink**, the **BLE management endpoint** the
phone app talks to, **OTA**, and **forwarding sensor data** to the display node.

Every unit is hardware-identical, so any unit can become the gateway — the
Bridge ties its gateway duties to the C6's Thread **leadership**, so the role
self-heals on failover.

- **MCU:** Seeed XIAO ESP32-C3 (Arduino core, NimBLE).
- **Link to C6:** UART @ 115200, `TX=21 / RX=20`.
- **Firmware version:** `BRIDGE_FW_VERSION` (bump on every published build; OTA
  only applies a strictly newer `c3_version`).

## What it does

### 1. Single management endpoint (leadership-gated)
Only the **active gateway** (the unit whose C6 is the Thread Leader) brings up
Wi-Fi and advertises the BLE service, so the app always sees exactly one device.
Role changes arrive from the C6 as `GW_ROLE LEADER|STANDBY`; a **30 s stand-down
grace** (`STANDBY_GRACE_MS`) prevents transient non-Leader blips from tearing
down the uplink.

### 2. Secure BLE control
GATT service `4fafc201-...914b` / char `beb5483e-...26a8`. The app authenticates
with a PIN (`STATUS?` -> `AUTH|<pin>` / `SETPIN|<old>|<new>`); privileged
commands are HMAC-signed and verified against the session. Locally-handled
commands tolerate the signature via `stripTrailingSig()`.

### 3. Provisioning & credential replication
- `PROVISION|{ssid,pass,zone,netName,disc,cloud,cloudKey}` -> stores creds,
  switches AP cleanly. Optional `disc` overrides the discovery-server URL;
  optional `cloud`/`cloudKey` set the AWS alerting service base URL + per-site
  API key (persisted in NVS).
- Receives mesh-replicated Wi-Fi creds + admin PIN from the C6 (`CFG_SET`) so one
  provisioning propagates fleet-wide; standby units hold them in NVS.

### 4. Sensor data pipeline
- Parses the C6's `[UDP_RX] ... EUI=<hex>;t=<csv>` lines and `forwardReading()`s
  them to the display node `/ingest` **and** (if provisioned) to the AWS Cloud
  Server `/v1/readings` with the `X-API-Key` (`[CLOUD]` log line) for overheat
  alerting — the LAN path is unaffected if the cloud is unreachable.
- Tracks recently-seen sensor EUIs and answers **`NODES?`** (`NODES_BEGIN` /
  `NODE|<eui>` / `NODES_END`) so the app can offer a live-device dropdown.
- `MAP|<eui>|<box>|<slot>|<label>` -> `registerSensorMap()` POSTs the
  EUI->location mapping to the display node `/map`.
- `discoverNode()` / `heartbeatPresence()` keep the gateway<->display-node link up.

### 5. OTA (3 phases)
Run from `loop()` so UART has a single reader during transfers:
- `OTA` — self-update this C3 over Wi-Fi (`Update` lib, version-gated by manifest).
- `OTAC6` — push a new C6 image to the Commissioner over UART (base64 + ACK).
- `OTA_FLEET` — the C6 signs + mesh-broadcasts the image URL; every unit
  self-updates staggered (gateway last). `OTA_SELF` updates just this unit
  (C6 then C3).

### 6. Lifecycle / status
- `SYS?` -> `SYS|role=...|c3=...|c6=...|comm=...|wifi=...|node=...` for the app's
  System screen.
- `commissioner_start` / `commissioner_stop` relay to the C6.
- `FACTORY_RESET` (this unit) / `RESET_FLEET` (signed mesh broadcast via the C6).
  The hardware **BOOT button** triggers the same wipe-and-reboot.

## Command reference (app -> bridge)
| Command | Signed | Effect / reply |
|---|---|---|
| `STATUS?` / `AUTH|<pin>` / `SETPIN|<old>|<new>` | -- | auth handshake |
| `PROVISION|{...}` | no | Wi-Fi + discovery setup |
| `SYS?` | no | `SYS|role=...` status line |
| `NODES?` | no | `NODES_BEGIN` / `NODE|<eui>` / `NODES_END` |
| `MAP|<eui>|<box>|<slot>|<label>` | no | `ACK MAP <eui>` / `ERR MAP ...` |
| `commissioner_start` / `commissioner_stop` | no | relayed to C6 |
| `add <eui> <pskd>` | **yes** | commission a joiner (relayed to C6) |
| `OTA` / `OTAC6` / `OTA_FLEET` / `OTA_SELF` | no | firmware update |
| `FACTORY_RESET` / `RESET_FLEET` | no | wipe this unit / whole fleet |

## Building & flashing
Open `Bridge.ino` in the Arduino IDE with the ESP32 core, select **XIAO
ESP32-C3**, and upload. Sensor/peripheral helpers live in `bme_sensor.h`,
`rtc_ds1307.h`, `logger.h`. Watch serial @ 115200 for `GW_ROLE`, `[FWD]`,
`[MAP]`, and OTA progress lines.

## Related modules
- [Commissioner](../Commissioner/) — the C6 Thread/OpenThread partner.
- [thread_commissioner](../thread_commissioner/) — the Flutter control app.
- [Discovery Server](../Discovery%20Server/) — discovery + display node.
- [SED_SENSOR_BARE](../SED_SENSOR_BARE/) — the mesh sensor firmware.
