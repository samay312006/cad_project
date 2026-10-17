#pragma once

// drive_core.h - motor drive + current sensing + duty clamp.
// Pin map and drive logic copied from embedded/drive_test/drive_test.ino
// (bench-verified, untouched by this plan). Duty clamp copied from
// embedded/drive_calib/drive_calib.ino - drive_test.ino has no such clamp,
// which docs/superpowers/specs/2026-09-03-incline-compensation-design.md
// §3.1 flags as a live over-voltage risk (motors rated 3.0-6.0V on a 12V
// bus). This sketch does not reintroduce that gap.

#include <Wire.h>
#include <Adafruit_INA219.h>

// ---------------------------------------------------------------- pin map
#define ENA 25
#define IN1 26
#define IN2 27
#define ENB 32
#define IN3 33
#define IN4 14

#define I2C_SDA 21
#define I2C_SCL 22
#define VBUS_ADC_PIN 34

static const int PWM_FREQ_HZ = 1000;
static const int PWM_BITS    = 8;

// ------------------------------------------------------- voltage/duty clamp
// Motors are rated 3.0-6.0V; the L298N is fed 12V. V_MOTOR_MAX=5.5V leaves
// margin below the 6.0V datasheet max for error in v0Drop/rTot. These
// v0Drop/rTot values are drive_calib.ino's DEFAULTS, not a measured
// calibration (none exists in this repo yet) - see design doc §9.
static const float V_MOTOR_MAX = 5.5f;
static float vBusManual = 12.6f;
static float vbusScale  = 5.545f;   // (100k + 22k) / 22k
static bool  useDivider = false;    // no GPIO34 divider fitted yet
static float v0Drop = 1.9f;
static float rTot   = 2.5f;

static float readBusVoltage() {
  if (!useDivider) return vBusManual;
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(VBUS_ADC_PIN);
  return (acc / 16.0f / 1000.0f) * vbusScale;
}

static int dutyMaxCounts() {
  float head = readBusVoltage() - v0Drop;
  if (head <= 0.1f) return 0;
  float d = V_MOTOR_MAX / head;
  if (d > 1.0f) d = 1.0f;
  if (d < 0.0f) d = 0.0f;
  return (int)(d * 255.0f);
}

// ---------------------------------------------------------------- drive
struct Side {
  const char *name;
  int  en, inFwd, inRev;
};

// If a side runs backwards during bring-up, swap its entry's inFwd/inRev pin
// values here (no invert flag like drive_test.ino has).
static Side sides[2] = {
  { "LEFT",  ENB, IN4, IN3 },   // OUT4 = +, OUT3 = -
  { "RIGHT", ENA, IN2, IN1 },   // OUT2 = +, OUT1 = -
};
static const int LEFT = 0, RIGHT = 1;

static int lastDuty[2] = { 0, 0 };

static void pwmSetup(int pin, int ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcAttach(pin, PWM_FREQ_HZ, PWM_BITS);
#else
  ledcSetup(ch, PWM_FREQ_HZ, PWM_BITS);
  ledcAttachPin(pin, ch);
#endif
}

// Returns the actually-applied (clamped) duty magnitude, so callers can
// record what the hardware really did rather than what was requested.
static int pwmWrite(int pin, int ch, int duty) {
  int cap = dutyMaxCounts();
  if (duty > cap) duty = cap;
  if (duty < 0)   duty = 0;
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(ch, duty);
#endif
  return duty;
}

static void setSide(int s, int duty) {
  Side &sd = sides[s];
  bool fwd = duty >= 0;
  int  mag = abs(duty);
  if (mag > 255) mag = 255;
  if (mag == 0) {
    digitalWrite(sd.inFwd, LOW);
    digitalWrite(sd.inRev, LOW);
  } else {
    digitalWrite(sd.inFwd, fwd ? HIGH : LOW);
    digitalWrite(sd.inRev, fwd ? LOW  : HIGH);
  }
  int applied = pwmWrite(sd.en, s, mag);
  lastDuty[s] = fwd ? applied : -applied;
}

static void drive(int left, int right) {
  setSide(LEFT,  left);
  setSide(RIGHT, right);
}

static void stopAll() { drive(0, 0); }

// ------------------------------------------------------- current sensing
static Adafruit_INA219 ina_left (0x40);
static Adafruit_INA219 ina_right(0x41);

struct CurrentSensor {
  const char      *name;
  Adafruit_INA219 *dev;
  uint8_t          addr;
  bool             present;
  bool             invert;
  float            amps;
};

static CurrentSensor isense[2] = {
  { "LEFT",  &ina_left,  0x40, true, false, 0.0f },
  { "RIGHT", &ina_right, 0x41, true, false, 0.0f },
};

static void sensorsBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.dev->begin()) {
      cs.present = false;
      Serial.printf("INA219 %-5s : NOT FOUND at 0x%02X\n", cs.name, cs.addr);
      continue;
    }
    cs.dev->setCalibration_32V_2A();
    Serial.printf("INA219 %-5s : ok at 0x%02X\n", cs.name, cs.addr);
  }
}

static void readCurrents() {
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) { cs.amps = NAN; continue; }
    float a = cs.dev->getCurrent_mA() / 1000.0f;
    if (cs.invert) a = -a;
    cs.amps = a;
  }
}

// ---------------------------------------------------------------- lifecycle
static void driveSetup() {
  // Kill both H-bridges FIRST, before anything else can run.
  int enPins[2] = { ENA, ENB };
  for (int i = 0; i < 2; i++) {
    pinMode(enPins[i], OUTPUT);
    digitalWrite(enPins[i], LOW);
  }
  for (int s = 0; s < 2; s++) {
    pinMode(sides[s].inFwd, OUTPUT);
    pinMode(sides[s].inRev, OUTPUT);
    digitalWrite(sides[s].inFwd, LOW);
    digitalWrite(sides[s].inRev, LOW);
    pwmSetup(sides[s].en, s);
  }
  stopAll();
  sensorsBegin();
}
