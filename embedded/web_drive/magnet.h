#pragma once

// magnet.h - electromagnet on/off through a 1-channel 5 V relay module, same
// wiring and polarity as drive_test.ino (GPIO19, normally-open contact on the
// magnet supply, flyback diode across the magnet). Plain on/off, no PWM.
//
// The magnet state is LATCHED: nothing in the drive path - not the command
// watchdog, not stopAll(), not an MQTT disconnect - touches it, so a link
// loss or a stop never drops a carried payload (spec.md section 5). Only an
// explicit robot/{id}/magnet command, or a reboot (relay boots OFF), changes it.

#include <Arduino.h>

#define MAGNET_RELAY 19
static const bool RELAY_ACTIVE_HIGH = true;   // false if your module clicks on when IN is pulled LOW

static bool magnetOn = false;

static void magnetSet(bool on) {
  magnetOn = on;
  digitalWrite(MAGNET_RELAY, (on == RELAY_ACTIVE_HIGH) ? HIGH : LOW);
}

// Call FIRST in setup(): latch the OFF level before the pin becomes an
// output so the relay never glitches on during boot.
static void magnetSetup() {
  digitalWrite(MAGNET_RELAY, RELAY_ACTIVE_HIGH ? LOW : HIGH);
  pinMode(MAGNET_RELAY, OUTPUT);
  magnetSet(false);
}
