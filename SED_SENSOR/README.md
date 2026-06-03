# SED_SENSOR — MLX90640 thermal-camera bring-up sketches

Stand-alone experiments for the **MLX90640** 32×24 thermal-camera sensor,
evaluated as an alternative/additional sensing front-end for the rack probes.
These are bench bring-up sketches — they are **not** Thread-mesh firmware and do
not join the network or send UDP. For the shipping mesh sensor see
[`../SED_SENSOR_BARE/`](../SED_SENSOR_BARE/).

## Contents
- `SED_SENSOR/SED_SENSOR.ino` — MLX90640 bare-bones test.
- `MLX90640/MLX90640.ino` — MLX90640 variant (plus the Adafruit library's
  `ci.yml`).

## What it does
A minimal read loop using the Adafruit MLX90640 driver:
1. I2C on `Wire.begin(D4, D5)` clocked at **400 kHz** (needed for the large
   EEPROM/calibration dump).
2. `mlx.begin(MLX90640_I2CADDR_DEFAULT)` at address `0x33`; halts on failure.
3. Configured for `MLX90640_CHESS` mode, `ADC_18BIT`, `2 Hz` refresh.
4. `loop()` grabs a 768-pixel (`32*24`) frame into `frame[]` and prints it to
   serial for inspection.

## Hardware
- **MCU:** XIAO ESP32-C6 (I2C on D4/D5).
- **Sensor:** MLX90640 thermal camera (I2C `0x33`).
- **Library:** `Adafruit_MLX90640`.

## Running
Open either `.ino` in the Arduino IDE, select the ESP32-C6 board, install the
Adafruit MLX90640 library, upload, and open the serial monitor at 115200 baud
(wait ~3 s for it to initialize before frames appear).

## Status
Experimental / reference. To turn this into a mesh node, the frame data would
need to be reduced (e.g. min/max/avg per zone) and tagged + transmitted the same
way `SED_SENSOR_BARE` sends its DS18B20 CSV payload (`EUI=…;t=…` to `ff03::2`).
