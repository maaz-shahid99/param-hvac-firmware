# hvac-firmware

Embedded firmware for the HVAC Thread-mesh temperature-monitoring system. This repo
holds every ESP32/Arduino target; the rest of the system lives in sibling repos.

> **Integration contract:** see [PROTOCOL.md](PROTOCOL.md) for the mesh parameters,
> wire formats, ports, and the **app↔firmware HMAC key** that must stay in lockstep
> with `hvac-mobile`.

## Components
| Path | MCU | Toolchain | Role |
|---|---|---|---|
| [Commissioner/](Commissioner/) | XIAO ESP32-**C6** | ESP-IDF (CMake) | Thread leader / commissioner; UDP sink; UART to the C3 |
| [Bridge/](Bridge/) | XIAO ESP32-**C3** | Arduino | BLE + Wi-Fi gateway; posts to the cloud; SD/RTC |
| [SED_SENSOR_BARE/](SED_SENSOR_BARE/) | ESP32-C6/H2 | Arduino | Sleepy-end-device sensor (DS18B20 probes) — the shipping sensor |
| [SED_SENSOR/](SED_SENSOR/) | ESP32 | Arduino | MLX90640 thermal-camera experiments |
| [SED_PROBE_ID/](SED_PROBE_ID/) | ESP32-C6 | Arduino | Bench utility: map DS18B20 probes to ROMs (no mesh needed) |
| [Sensor_Probe/](Sensor_Probe/) | ESP32 | Arduino | Minimal sensor sketch |
| [qr-codes/](qr-codes/) | — | — | Per-device EUI-64 + PSKd provisioning QR codes |

## Build
- **Commissioner (C6):** ESP-IDF with OpenThread — `idf.py set-target esp32c6 && idf.py build flash monitor`.
- **Arduino targets:** open the `.ino` in Arduino IDE (ESP32 core), pick the C6/C3 board, upload, watch serial @ 115200.

## Sibling repos
- App: https://github.com/maaz-shahid99/param-hvac-mobile
- Backend: https://github.com/maaz-shahid99/param-hvac-server
- Web: https://github.com/maaz-shahid99/param-hvac-web

Architecture diagrams: [docs/architecture/](docs/architecture/). Security notes: [SECURITY.md](SECURITY.md).
