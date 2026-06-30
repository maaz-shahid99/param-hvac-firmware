# SED_SENSOR_BARE — Thread Sleepy-End-Device sensor firmware

Production firmware for the **probe nodes** in the HVAC mesh. Each node is an
ESP32-C6/H2 that joins the Thread network as a child, reads a string of DS18B20
temperature probes, and pushes a tagged reading into the mesh every 10 s. This
is the "bare" (no display / no extra peripherals) build that actually ships on
the rack sensors.

## Hardware
- **MCU:** Seeed XIAO ESP32-C6 (or H2) — any chip with an 802.15.4 radio
  (`SOC_IEEE802154_SUPPORTED`).
- **Probes:** up to `NUM_SENSORS` (default **8**) DS18B20s on a single OneWire
  bus at pin `D4` (`ONE_WIRE_BUS`). A disconnected probe reports `err` instead
  of a value, so the slot count stays stable.

## Status LEDs
Three discrete (single-colour) LEDs give at-a-glance status without a serial
monitor. Each is wired **active-HIGH**: `pin → resistor → LED → GND` (set
`LED_ACTIVE_LOW 1` in the sketch for common-anode wiring). Default pins are XIAO
ESP32-C6 pads `D1/D2/D3` — `D4` is reserved for the OneWire bus.

| LED | Pin | Meaning |
|---|---|---|
| **Power** (green) | `D1` | Solid once the firmware is running. *(Can instead be wired straight to 3V3 — then drop `LED_PWR_PIN`.)* |
| **Network** (blue) | `D2` | **Blinks** (~1.25 Hz) while joining/attaching → **solid** once attached to the mesh as a `CHILD` (i.e. reporting). |
| **Fault** (red) | `D3` | **Solid** on a permanent join failure (e.g. PSKd mismatch, err 28); **brief blink** on a transient error (a DS18B20 `err` or a UDP send failure). |

Reading them together: *green only, blue blinking* = looking for the mesh;
*green + blue solid* = healthy and reporting; *red solid* = won't join, check the
PSKd/commissioning; *red blip* = a probe dropped out or a send failed.

## What it does
1. **Boot / identity** — formats NVS if needed and reads the factory **EUI-64**
   (`ESP_MAC_IEEE802154`) into `g_eui`. The EUI tags every payload so the
   gateway / display node can map this physical sensor to its rack box/slot.
2. **Attach to the mesh:**
   - If an active Thread dataset is already stored in NVS → it brings the stack
     up and attaches as a sleepy child (no re-join needed).
   - Otherwise → it runs the **MeshCoP joiner** (`otJoinerStart`) with the join
     passphrase `pskd = "J01NME"`, scanning on channel 15. On success the network
     keys are saved to NVS and the device reboots to apply them. Error 23 (missed
     beacon) auto-retries; error 28 means a PSKd mismatch.
3. **Report:** once attached as `OT_DEVICE_ROLE_CHILD`, every 10 s it reads all
   probes and UDP-sends a payload to the mesh:
   ```
   EUI=58e6c5fffe164ec0;t=23.1,24.2,err,25.0,...
   ```
   Sent to `ff03::2` (all-routers, mesh-local) on port **1234**, where the
   gateway's Commissioner (C6) UDP listener receives it and relays it to the
   Bridge (C3) → display node.

> The join passphrase must match the Commissioner's `ROUTER_JOIN_PSKD` /
> commissioner-added joiner credential. The `ff03::2` group + port 1234 must
> match the gateway's UDP listener.

## Flashing
Open `SED_SENSOR_BARE.ino` in the Arduino IDE (ESP32 core with OpenThread),
select an **ESP32-C6/H2** board, and upload. Watch the serial monitor at
115200 baud:
- `[HW] EUI-64: …` — note this; it's the device's identity (and what its
  commissioning QR encodes, see [`../QR codes/`](../qr-codes/)).
- `[JOINER RADAR] …` — scanning for a commissioner.
- `[UDP] Packet sent: EUI=…;t=…` — attached and reporting.

`Untitled.png` is a reference wiring/screenshot for the probe build.

## Related modules
- [`../Commissioner/`](../Commissioner/) — the C6 that commissions joiners and
  receives these UDP readings.
- [`../Bridge/`](../Bridge/) — the C3 that forwards readings to the display node.
- [`../Discovery Server/`](https://github.com/YOUR-ORG/hvac-server) — ingests and visualizes them.
- [`../SED_SENSOR/`](../SED_SENSOR/) — MLX90640 thermal-camera experiments.
