# Security

What's enforced in the code today, and the provisioning/ops steps that finish
the hardening. Pairs with the go-live list in `DEPLOYMENT.md`.

---

## 1. Command authentication (HMAC) — ENFORCED
`add <EUI> <PSKd>` admits a device to the Thread mesh. The app signs it
`add …|<HMAC-SHA256>` with `SECURE_HMAC_KEY`; the Commissioner (C6) **verifies
and rejects** anything unsigned, malformed, or mismatched
(`Commissioner/main/security.c`, constant-time compare). So even a compromised
Bridge (C3) can't forge a join. `factory_reset` is a trusted-link command over
the local UART (gated by the C3's authenticated BLE session / an HMAC-verified
mesh RESET), so it is intentionally not on the signed path.

**Rotate the key for production** (the repo ships a placeholder):
1. `Commissioner/main/config.h` → `SECURE_HMAC_KEY` = a long random string; rebuild/flash the C6 fleet.
2. App → set the matching key in the app's secure storage (`BLEService.updateSecretKey(...)`, default in `ble_service.dart` is `PROD_SECRET_KEY_CHANGE_ME`).
3. The two MUST match or commissioning is rejected. Roll both together.

## 2. Per-device join PSKd — provisionable (no longer a hard-coded constant)
A shared PSKd is a weak secret. The sensor now loads its PSKd from **NVS**
(namespace `factory`, key `pskd`) and only falls back to the build default if
none is set (`SED_SENSOR_BARE.ino`). The router/joiner default lives in
`config.h` (`ROUTER_JOIN_PSKD`). Mesh admission is still gated per-EUI by the
signed `add`, so a leaked PSKd alone can't join — but unique PSKds remove the
shared-secret risk entirely.

**Factory provisioning (per unit):**
1. Pick a unique PSKd per device (6–32 chars, uppercase A–Z + 0–9, excluding
   I O Q Z per the Thread spec), e.g. derive `HMAC(factory_secret, EUI)` → map
   to that charset, or use a random generator.
2. Write it to NVS. With ESP-IDF's `nvs_partition_gen.py` from a CSV:
   ```
   key,type,encoding,value
   factory,namespace,,
   pskd,data,string,ABC234DEF5
   ```
   `python nvs_partition_gen.py generate pskd.csv pskd.bin 0x6000` then flash it
   at the `nvs` partition offset with esptool.
3. Print the EUI + PSKd on the unit's QR label (see `QR codes/`). The app sends
   `add <EUI> <PSKd>`; because the EUI and PSKd both come from that label, every
   device has distinct join credentials.

## 3. Sensitive logging — gated
Secrets are no longer printed by default. The join PSKd / raw `add` line (C6
`uart_rx.c`) and Wi-Fi SSID (C6 `config_sync.c`, C3 `Bridge.ino`) are behind
`#define LOG_SENSITIVE 0`. Wi-Fi passwords and admin PINs are never logged.
Set `LOG_SENSITIVE 1` only for bench debugging.

## 4. NVS-at-rest encryption (flash encryption) — enable at manufacturing
Credentials (Wi-Fi pass, admin PIN, PSKd) sit in NVS. To encrypt flash at rest
on the **C6 (ESP-IDF)**:
1. `idf.py menuconfig` → *Security features* → **Enable flash encryption on boot**
   (Release mode), and *NVS* → enable **NVS encryption** (adds an `nvs_keys`
   partition — already room in `partitions.csv`'s layout, add the partition).
2. First boot burns eFuses and encrypts flash. **This is irreversible and
   per-board** — validate on a sacrificial unit; a wrong config can brick it.
3. Use **Release** (not Development) flash-encryption mode for production so the
   key can't be read back over UART.

The **C3 (Arduino)** doesn't expose flash encryption as cleanly; for the C3,
treat the device as needing physical security, or move its credential storage to
the C6. Document the chosen posture per product.

## 5. Cloud hardening — enforced in code
- **Fail-fast:** with `ENV=production`, the server refuses to start on insecure
  config — default/weak `JWT_SECRET`, default `BOOTSTRAP_TOKEN`, `CORS_ORIGINS=*`,
  or SQLite (`config.validate_production()`).
- **CORS:** `CORS_ORIGINS` is an explicit allowlist in prod (not `*`).
- **Rate limiting:** per-IP sliding window on `login`/`register`/`forgot`/`reset`
  (`AUTH_RATE_MAX`/`AUTH_RATE_WINDOW_S`) to blunt brute-force + OTP spam. It's
  in-memory per process — front a shared store (Redis) for a hard global cap at
  scale.
- Passwords are bcrypt-hashed; OTP reset codes and gateway API keys are stored
  only as hashes; every table is `tenant_id`-scoped. JWTs sign sessions.
- Set real secrets in prod: `JWT_SECRET` (≥32 random chars), `BOOTSTRAP_TOKEN`
  (or empty to disable self-registration). See `Cloud Server/deploy/AWS_SETUP.md`.

## 6. App
- `android:allowBackup="false"` + backup/extraction rules exclude all app data,
  so the saved JWT/prefs can't be cloud-backed-up or transferred to another
  device (`AndroidManifest.xml`, `res/xml/*`).
- **TODO before shipping:** set `usesCleartextTraffic="false"` once the cloud is
  HTTPS (kept `true` only for local `http://` bring-up), and replace the
  hard-coded `defaultSecretKey` with a provisioned per-deployment key (§1).

## Transport security
All service traffic is TLS in production: the cloud is fronted by Nginx/ALB
(`Cloud Server/deploy/`), and the C3 gateway speaks TLS to `https://` cloud URLs
(`WiFiClientSecure`, with optional `CLOUD_ROOT_CA` pinning). See `DEPLOYMENT.md`.

---

## Reporting
For a real product, add a contact here (e.g. `security@yourdomain.com`) and a
coordinated-disclosure policy.
