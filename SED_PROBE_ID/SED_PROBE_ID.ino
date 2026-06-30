/*
 * DS18B20 Probe Identifier — standalone (no Thread / no router needed).
 *
 * Maps each physical probe to a stable index (1..N) + its ROM address, so you
 * can label probes 1..8 by hand without the mesh.
 *
 * HOW TO USE
 *   1. Connect all probes, flash this, open Serial Monitor @ 115200.
 *   2. It lists every probe with an index and its ROM.
 *   3. Pinch/warm ONE probe with your fingers (your hand is ~10 C above room).
 *   4. The table flags the probe that moved the most:  "<<< THIS ONE".
 *   5. Note that probe's index/ROM, label the physical wire, then RELEASE it.
 *   6. Send any character in Serial Monitor to re-zero the baseline, and repeat
 *      for the next probe.
 *
 * NOTES
 *   - The index is the 1-Wire ROM-search order, so it is STABLE across reboots
 *     as long as the same probes stay on the bus (unplug one and indices shift).
 *   - The ROM (64-bit serial) is the unique id the app/cloud use when you assign
 *     a probe to a rack location, so that's the one worth writing down.
 *
 * WIRING (XIAO ESP32-C6): data -> D4, VDD -> 3V3, GND -> GND, and one 4.7k
 *   (3.3k for long runs) pull-up between D4 and 3V3. Use external power, not
 *   parasitic, for a multi-probe string.
 */

#include <Arduino.h>
#include <OneWire.h>
#include <DallasTemperature.h>

#define ONE_WIRE_BUS   D4      // same data pin the SED firmware uses
#define MAX_PROBES     16
#define RESOLUTION     11      // 9..12 bits (lower = faster refresh, coarser)
#define TOUCH_DELTA_C  1.0f    // a probe must move >= this (vs baseline) to be flagged

OneWire oneWire(ONE_WIRE_BUS);
DallasTemperature sensors(&oneWire);

DeviceAddress addr[MAX_PROBES];
float baseline[MAX_PROBES];
int   nProbes = 0;

static void printRom(const DeviceAddress a) {
  char b[3];
  for (int i = 0; i < 8; i++) { sprintf(b, "%02x", a[i]); Serial.print(b); }
}

static void takeBaseline() {
  sensors.requestTemperatures();
  for (int i = 0; i < nProbes; i++) baseline[i] = sensors.getTempC(addr[i]);
  Serial.println(">> baseline reset\n");
}

void setup() {
  Serial.begin(115200);
  delay(1500);
  Serial.println("\n=== DS18B20 Probe Identifier ===");

  sensors.begin();
  nProbes = sensors.getDeviceCount();
  Serial.printf("Found %d probe(s).\n", nProbes);
  if (nProbes == 0)
    Serial.println("None found -> check D4 wiring, the 4.7k pull-up to 3V3, and power.");
  if (nProbes > MAX_PROBES) nProbes = MAX_PROBES;

  for (int i = 0; i < nProbes; i++) {
    if (sensors.getAddress(addr[i], i)) {
      sensors.setResolution(addr[i], RESOLUTION);
      Serial.printf("  probe %2d   ROM=", i + 1);
      printRom(addr[i]);
      Serial.println();
    } else {
      Serial.printf("  probe %2d   <ROM read failed>\n", i + 1);
    }
  }

  Serial.println("\nWarm ONE probe with your fingers and watch which one moves.");
  Serial.println("Send any char to re-zero the baseline between probes.\n");
  takeBaseline();
}

void loop() {
  // Re-baseline whenever the user sends anything.
  if (Serial.available()) { while (Serial.available()) Serial.read(); takeBaseline(); }

  sensors.requestTemperatures();

  // Read all probes; find the biggest mover vs baseline (the one you're holding).
  float temp[MAX_PROBES];
  int   mover = -1;
  float best  = TOUCH_DELTA_C;
  for (int i = 0; i < nProbes; i++) {
    temp[i] = sensors.getTempC(addr[i]);
    if (temp[i] == DEVICE_DISCONNECTED_C) continue;
    float d = fabsf(temp[i] - baseline[i]);
    if (d >= best) { best = d; mover = i; }
  }

  Serial.println("---------------------------------------------------------------");
  for (int i = 0; i < nProbes; i++) {
    Serial.printf("probe %2d  ROM=", i + 1);
    printRom(addr[i]);
    if (temp[i] == DEVICE_DISCONNECTED_C) Serial.print("   err        ");
    else Serial.printf("  %6.2f C  (%+5.2f)", temp[i], temp[i] - baseline[i]);
    if (i == mover) Serial.print("   <<< THIS ONE");
    Serial.println();
  }

  delay(700);
}
