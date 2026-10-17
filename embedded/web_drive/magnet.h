#pragma once

// magnet.h - electromagnet on/off through a 1-channel 5 V relay module on
// GPIO23 (relay IN -> D23, VCC -> VIN, GND -> GND). Plain on/off, no PWM.
//
// Bench-measured behaviour of THIS module (relay_test, 2026-10-17):
//   * ACTIVE-LOW input: D23 driven LOW  -> relay energized.
//   * A 3.3 V HIGH does NOT reliably release it (module input is pulled up to
//     5 V), so the relay is de-energized by RELEASING the pin (high-Z input),
//     never by driving it HIGH.
//   * The magnet is wired to the contact that is CLOSED while the relay is
//     idle (NC-style): relay energized (D23 LOW) = magnet OFF, relay idle
//     (D23 released) = magnet ON.
//
// Consequence: the magnet is ON whenever the relay is idle - during the
// ~0.3 s ESP32 boot, and whenever the ESP32 is unpowered. To get the safer
// "magnet only on when actively commanded" default, move the magnet wire to
// the other contact (the one closed when energized) and set
// MAGNET_WIRED_IDLE_CLOSED = false below; nothing else changes.
//
// The magnet state is LATCHED: nothing in the drive path - not the command
// watchdog, not stopAll(), not an MQTT disconnect - touches it. Only an
// explicit robot/{id}/magnet command, or a reboot, changes it.

#include <Arduino.h>

#define MAGNET_RELAY 23   // human-directed 2026-10-17: relay IN is wired to D23
static const bool MAGNET_WIRED_IDLE_CLOSED = true;

static bool magnetOn = false;

// Active-low module: energize = drive LOW, de-energize = release the pin.
static void relayEnergize(bool energize) {
  if (energize) {
    digitalWrite(MAGNET_RELAY, LOW);
    pinMode(MAGNET_RELAY, OUTPUT);
  } else {
    pinMode(MAGNET_RELAY, INPUT);
  }
}

static void magnetSet(bool on) {
  magnetOn = on;
  relayEnergize(MAGNET_WIRED_IDLE_CLOSED ? !on : on);
}

// Call FIRST in setup(): put the magnet in its OFF state as early as possible.
static void magnetSetup() {
  magnetSet(false);
}
