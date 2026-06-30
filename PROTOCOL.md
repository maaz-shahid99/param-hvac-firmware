# HVAC System — Integration Contract

This is the **shared contract** between the four repositories that make up the
system. Each repo is built by a different team, so the values below are the
seams between them. **Any change to a value here must be coordinated across the
affected repos** or the system breaks silently in the field.

Keep this file **identical** in `hvac-firmware` and `hvac-server` (it is the
canonical copy); the mobile and web repos link to it.

| Repo | Contents | Talks to |
|---|---|---|
| **hvac-firmware** | Commissioner (C6, ESP-IDF), Bridge (C3, Arduino), sensors | mesh + Cloud Server (via C3) |
| **hvac-server** | `cloud-server` (FastAPI), `discovery-server` + display node | app, web, firmware |
| **hvac-mobile** | Flutter app (BLE commissioning + cloud console) | firmware (BLE), Cloud Server |
| **hvac-web** | `web-dashboard` (customer), `field-console` (manufacturer) | Cloud Server |

## 1. Thread mesh (firmware-internal — Commissioner ↔ Bridge ↔ sensors)
| Item | Value | Source of truth |
|---|---|---|
| Thread channel | **15** | `Commissioner/main/thread_init.c` |
| PAN ID | **0x1234** | `Commissioner/main/thread_init.c` |
| Join PSKd (sensors + routers) | **`J01NME`** | `Commissioner/main/config.h` (`ROUTER_JOIN_PSKD`) + `SED_SENSOR_BARE.ino` — **must match** |
| Readings group / port | **`ff03::2`** / UDP **1234** | `Commissioner/main/udp_listener.c`, `config_sync.c` |
| Config-sync group / port | **`ff03::1`** / UDP **1235** | `Commissioner/main/config_sync.*` |

## 2. Wire formats (UART lines / UDP payloads)
```
Sensor reading : EUI=<eui64>;t=<v1>,<v2>,...      (per-probe DS18B20 temp, or "err")
Env (BME680)   : ENV=<t>,<h>,<p>,<voc>;e=<eui>
Crash report   : CRASH=<reason>|<pc>|<task>
Config-sync    : CFG|<ver>|<ssid>|<pass>|<zone>|<net>|<pin>|<hmac>
```

## 3. Cloud Server HTTP API — port **8002** (`hvac-server/cloud-server`)
| Plane | Auth | Endpoints | Caller |
|---|---|---|---|
| Ingest | `X-API-Key` | `POST /v1/readings`, `/v1/env`, `/v1/mesh`, `/v1/crashes`; `GET /v1/ota/check`; `GET /firmware/*` | Bridge (C3) |
| Customer | JWT (HS256) | `POST /v1/auth/{register,login,join,forgot}`; `GET /v1/current,/v1/thresholds,/v1/alerts,…` | app + web-dashboard |
| Support | `X-Support-Token` | `/v1/support/*` + firmware publish (404 unless `SUPPORT_TOKEN` set) | field-console |

## 4. Discovery + display node (LAN, optional on-prem path — `hvac-server/discovery-server`)
| Service | Port | Endpoints |
|---|---|---|
| Discovery Server | **8000** | `/register/sensor`, `/register/forwarder`, `/discover`, `/data`, `/poll`, `/mobile/poll` |
| Display node | **8001** | `/ingest`, `/poll` (serves the local 3D-mesh dashboard) |

## 5. Frontend → backend (`hvac-web`)
- `web-dashboard` dev server **5173**, `field-console` dev server **5174**.
- Both proxy `/v1` (field-console also `/firmware`) → `http://localhost:8002` in dev
  (`vite.config.ts`). Production: set `VITE_CLOUD_URL`, or override at runtime via
  `localStorage` (`cloud_base_url` / `fc_base`).

## 6. ⚠️ Shared secret — app ↔ firmware HMAC key
The app signs privileged BLE commands with an HMAC key the C6 verifies. The key is a
placeholder (`PROD_SECRET_KEY_CHANGE_ME`) in **two repos** and they must be changed to
the same real value **in lockstep** before production:
- `hvac-firmware` → `Commissioner/main/config.h`
- `hvac-mobile` → `lib/ble_service.dart`

A mismatch means the firmware silently rejects every privileged command from the app.

## 7. Secrets that live in NO repo
`cloud-server/.env` holds `JWT_SECRET`, `BOOTSTRAP_TOKEN`, `SUPPORT_TOKEN`, and SMTP/
Gmail credentials. It is gitignored and supplied per-environment — see
`cloud-server/.env.example`. Rotate any credential that was ever shared before
handing repos to a new team.
