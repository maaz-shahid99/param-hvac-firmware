# QR codes — commissioning labels for routers & sensors

Per-device commissioning artifacts used by the **Thread Commissioner** app's
"Scan QR" flow ([`https://github.com/YOUR-ORG/hvac-mobile`](https://github.com/YOUR-ORG/hvac-mobile)). Scanning a
code feeds the device's identity straight into the `add`/commission command, so
an installer never has to type a 16-hex EUI-64 by hand.

## Contents
For each device there are two files:
- **`<name>.png`** — the scannable QR code. The app decodes it into the device's
  **EUI-64** (and join **PSKd**) and commissions that joiner.
- **`<name>.txt`** — the same device's EUI-64 in plain hex, for reference /
  manual entry. e.g. `Router 1.txt → 58e6c5fffe1116e0`, `SED 1.txt → 58e6c5fffe164ec0`.

| Group    | Files                          | Role |
|----------|--------------------------------|------|
| Routers  | `Router1..4.png` / `Router 1..4.txt` | FTD/router nodes (the C6 + C3 bridge units) |
| Sensors  | `SED1..3.png` / `SED 1..3.txt` | Sleepy-end-device temperature probes |

## How they're used
1. A device boots and starts the MeshCoP **joiner** with the shared passphrase
   (`J01NME`, the routers' `ROUTER_JOIN_PSKD` / the sensor's `pskd`).
2. The operator opens the app, taps **Scan QR**, and scans that device's `.png`.
3. The app sends the signed `add <eui64> <pskd>` to the gateway's Commissioner,
   which admits the joiner into the mesh.
4. For sensors, the post-commission dialog then assigns the EUI to a physical
   rack/unit/port (see the app's Rack Layout).

## Regenerating / adding a device
The EUI-64 is printed on each node's serial boot log (`[HW] EUI-64: …`). Encode
that EUI (with the join PSKd) into a QR using whatever tool produced these, drop
the `.png` + a matching `.txt` here, and name it after the device.

> Keep these labels with the physical units — the EUI-64 is the permanent
> identity used everywhere downstream (commissioning, box/slot mapping, the
> display-node dashboard).
