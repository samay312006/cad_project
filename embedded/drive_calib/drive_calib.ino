// drive_calib - measure the constants the incline-compensation loop needs.
//
// Separate from drive_test on purpose: drive_test is the known-good bench tool
// and stays untouched. This sketch only measures; it never runs a control loop.
//
// Produces two things:
//   L  locked-rotor sweep  -> V0 and R_tot  ("how much voltage gets eaten")
//   k  flat-ground table   -> the speed baseline ("what normal looks like")
//
// Run L first, enter its results with `o` and `r`, then run k.
//
// WIRING - identical to drive_test, one L298N, two channels:
//
//   ESP32          L298N        goes to
//   GPIO25   --->  ENA          (right side enable, PWM)
//   GPIO26   --->  IN1          right OUT1  -> right motors NEGATIVE
//   GPIO27   --->  IN2          right OUT2  -> right motors POSITIVE
//   GPIO32   --->  ENB          (left side enable, PWM)
//   GPIO33   --->  IN3          left  OUT3  -> left  motors NEGATIVE
//   GPIO14   --->  IN4          left  OUT4  -> left  motors POSITIVE
//   GND      --->  GND          MANDATORY common ground
//
//   INA219 x2, shared I2C: VCC->3V3, GND->GND, SDA->GPIO21, SCL->GPIO22
//     LEFT  0x40 : shunt in the left  motor return  (OUT3 -> VIN+, VIN- -> mot-)
//     RIGHT 0x41 : shunt in the right motor return  (OUT1 -> VIN+, VIN- -> mot-)
//
//   Bus voltage divider (optional until fitted):
//     Battery+ -> 100k -> GPIO34 -> 22k -> GND, with 100nF from GPIO34 to GND.
//     Until it exists, enter the pack voltage by hand with `v`.

#include <Wire.h>
#include <Adafruit_INA219.h>
#include "fit.h"

#define ENA 25
#define IN1 26
#define IN2 27
#define ENB 32
#define IN3 33
#define IN4 14

#define I2C_SDA 21
#define I2C_SCL 22
#define VBUS_ADC_PIN 34

const int PWM_FREQ_HZ = 1000;
const int PWM_BITS    = 8;

// ------------------------------------------------------- motor protection
// The FAM1029 motors are rated 3.0-6.0 V and the L298N is fed 12 V, so full
// duty puts ~10 V across them. This sketch drives real motors, so the same
// clamp drive_test uses lives here too - inside pwmWrite(), the one choke
// point no measurement routine can route around.
const float V_MOTOR_MAX = 5.0f;

float vBusManual = 12.6f;   // used when no divider is fitted; charged 3S
float vbusScale  = 5.545f;  // (100k + 22k) / 22k, trimmed against a multimeter
bool  useDivider = false;   // flipped on by `d`

// Filled in by the locked-rotor sweep, then entered with `o` and `r` so the
// flat-ground table can be run without a re-upload in between.
float v0Drop = 1.9f;
float rTot   = 2.5f;

// ---------------------------------------------------------------- sensors
Adafruit_INA219 ina_left (0x40);
Adafruit_INA219 ina_right(0x41);

struct CurrentSensor {
  const char      *name;
  Adafruit_INA219 *dev;
  uint8_t          addr;
  bool             present;
  bool             invert;
  float            amps;      // raw, as the chip reports it
  float            ampsNorm;  // raw / duty
};

CurrentSensor isense[2] = {
  { "LEFT",  &ina_left,  0x40, true, false, 0.0f, 0.0f },
  { "RIGHT", &ina_right, 0x41, true, false, 0.0f, 0.0f },
};

struct Side { const char *name; int en, inFwd, inRev; };
Side sides[2] = {
  { "LEFT",   ENB, IN4, IN3 },   // OUT4 = +, OUT3 = -
  { "RIGHT",  ENA, IN2, IN1 },   // OUT2 = +, OUT1 = -
};
const int LEFT = 0, RIGHT = 1;

int lastDuty[2] = { 0, 0 };

// ---------------------------------------------------------------- PWM shim
void pwmSetup(int pin, int ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcAttach(pin, PWM_FREQ_HZ, PWM_BITS);
#else
  ledcSetup(ch, PWM_FREQ_HZ, PWM_BITS);
  ledcAttachPin(pin, ch);
#endif
}

float readBusVoltage() {
  if (!useDivider) return vBusManual;
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(VBUS_ADC_PIN);
  return (acc / 16.0f / 1000.0f) * vbusScale;
}

int dutyMaxCounts() {
  float head = readBusVoltage() - v0Drop;
  if (head <= 0.1f) return 0;
  float d = V_MOTOR_MAX / head;
  if (d > 1.0f) d = 1.0f;
  if (d < 0.0f) d = 0.0f;
  return (int)(d * 255.0f);
}

void pwmWrite(int pin, int ch, int duty) {
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
}

// ---------------------------------------------------------------- drive
void setSide(int s, int duty) {
  Side &sd = sides[s];
  bool fwd = duty >= 0;
  int  mag = abs(duty);
  if (mag == 0) {
    digitalWrite(sd.inFwd, LOW);
    digitalWrite(sd.inRev, LOW);
  } else {
    digitalWrite(sd.inFwd, fwd ? HIGH : LOW);
    digitalWrite(sd.inRev, fwd ? LOW  : HIGH);
  }
  pwmWrite(sd.en, s, mag);
  lastDuty[s] = mag;
}

void drive(int l, int r) { setSide(LEFT, l); setSide(RIGHT, r); }
void stopAll()           { drive(0, 0); }

// ---------------------------------------------------------------- sensing
void sensorsBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.dev->begin()) {
      cs.present = false;
      Serial.printf("INA219 %-5s : NOT FOUND at 0x%02X - press i to scan\n",
                    cs.name, cs.addr);
      continue;
    }
    cs.dev->setCalibration_32V_2A();
    Serial.printf("INA219 %-5s : ok at 0x%02X\n", cs.name, cs.addr);
  }
}

void readCurrents() {
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) { cs.amps = NAN; cs.ampsNorm = NAN; continue; }
    float a = cs.dev->getCurrent_mA() / 1000.0f;
    if (cs.invert) a = -a;
    cs.amps = a;
    float d = lastDuty[i] / 255.0f;
    cs.ampsNorm = (d > 0.01f) ? (a / d) : NAN;
  }
}

// Average both current readings over a window, with the motor already settled.
void averageCurrents(int samples, int gapMs, float *rawL, float *rawR,
                     float *normL, float *normR) {
  float sr[2] = {0, 0}, sn[2] = {0, 0};
  for (int n = 0; n < samples; n++) {
    readCurrents();
    sr[0] += isense[0].amps;     sr[1] += isense[1].amps;
    sn[0] += isense[0].ampsNorm; sn[1] += isense[1].ampsNorm;
    delay(gapMs);
  }
  *rawL  = sr[0] / samples;  *rawR  = sr[1] / samples;
  *normL = sn[0] / samples;  *normR = sn[1] / samples;
}

void i2cScan() {
  Serial.println(F("\n-- I2C scan --"));
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) { Serial.printf("  device at 0x%02X\n", a); found++; }
  }
  if (!found) Serial.println(F("  nothing found - check VCC / GND / SDA / SCL"));
  Serial.println();
}

// ------------------------------------------------------- locked rotor
// With the rotor held, the motor generates no back-EMF, so every volt goes
// into the bridge drop and the winding resistance:
//
//     V_bus * duty  =  V0  +  I * R_tot
//
// Sweeping several duty points and fitting a line gives both unknowns at once,
// and the residual says whether the model actually holds.
//
// Duties start at 0.18 because below roughly V0/V_bus the bridge does not
// conduct at all, and stop at 0.40 to keep locked-rotor current modest.
//
// Both raw and duty-normalized current are fitted. Which one is correct depends
// on whether motor current keeps circulating through the shunt during the PWM
// off-time, and that depends on the freewheel path - so it is measured here
// rather than assumed. Use whichever fit has the smaller residual.
void lockedRotorSweep(int s) {
  const int counts[5] = { 46, 60, 74, 88, 102 };   // 0.18 .. 0.40
  float vApp[5], iRaw[5], iNorm[5];

  Serial.printf("\n-- locked rotor: %s --\n", sides[s].name);
  Serial.println(F("   HOLD THE WHEELS SO THEY CANNOT TURN."));
  Serial.println(F("   If a wheel creeps, the numbers are wrong. Starting in 4 s."));
  delay(4000);

  float vBus = readBusVoltage();
  Serial.printf("   V_bus %.2f V\n", vBus);
  Serial.println(F("    duty   V_app    I_raw   I_norm"));

  for (int i = 0; i < 5; i++) {
    setSide(s, counts[i]);
    delay(250);                       // settle; keep each burst short
    float rl, rr, nl, nr;
    averageCurrents(6, 10, &rl, &rr, &nl, &nr);
    setSide(s, 0);

    float d = counts[i] / 255.0f;
    vApp[i]  = vBus * d;
    iRaw[i]  = (s == LEFT) ? rl : rr;
    iNorm[i] = (s == LEFT) ? nl : nr;
    Serial.printf("   %.3f  %6.3f  %7.4f  %7.4f\n", d, vApp[i], iRaw[i], iNorm[i]);
    delay(700);                       // let it cool between points
  }
  stopAll();

  Fit fRaw  = fitLine(iRaw,  vApp, 5);
  Fit fNorm = fitLine(iNorm, vApp, 5);

  Serial.println(F("\n   fit against RAW current:"));
  if (fRaw.ok)
    Serial.printf("     R_tot %.4f ohm   V0 %.4f V   residual %.4f V\n",
                  fRaw.slope, fRaw.intercept, fRaw.rms);
  else Serial.println(F("     failed - currents too close together"));

  Serial.println(F("   fit against NORMALIZED current:"));
  if (fNorm.ok)
    Serial.printf("     R_tot %.4f ohm   V0 %.4f V   residual %.4f V\n",
                  fNorm.slope, fNorm.intercept, fNorm.rms);
  else Serial.println(F("     failed - currents too close together"));

  if (fRaw.ok && fNorm.ok) {
    bool rawWins = fRaw.rms < fNorm.rms;
    Serial.printf("\n   -> use the %s fit (smaller residual)\n",
                  rawWins ? "RAW" : "NORMALIZED");
    const Fit &best = rawWins ? fRaw : fNorm;
    Serial.printf("   -> o%.4f    r%.4f\n", best.intercept, best.slope);
    if (best.intercept < 0.5f || best.intercept > 3.5f)
      Serial.println(F("   WARNING: V0 outside 0.5-3.5 V. Did a wheel turn?"));
    if (best.slope <= 0.0f || best.slope > 12.0f)
      Serial.println(F("   WARNING: R_tot looks wrong. Redo the measurement."));
    Serial.printf("   implied stall current at 6 V: %.2f A\n",
                  (6.0f - best.intercept) / best.slope);
  }
  Serial.println();
}

// ------------------------------------------------------- flat ground
// Drives straight at eight duty points and records what normal looks like.
// Needs V0 and R_tot from the locked-rotor sweep first - enter them with
// `o` and `r`, or the back-EMF column is meaningless.
void flatGroundTable() {
  Serial.println(F("\n-- flat-ground baseline --"));
  Serial.printf ("   using V0 %.4f V, R_tot %.4f ohm\n", v0Drop, rTot);
  Serial.println(F("   Flat level floor, clear run ahead, normal payload on."));
  Serial.println(F("   Starting in 4 s."));
  delay(4000);

  int cap = dutyMaxCounts();
  int lo  = 51;                       // 0.20
  if (cap <= lo + 7) { Serial.println(F("   duty cap too low - check V_bus")); return; }

  Serial.println(F("    duty    I_L     I_R    bemfL   bemfR"));
  float rows[8][3];

  for (int i = 0; i < 8; i++) {
    int counts = lo + (cap - lo) * i / 7;
    float d = counts / 255.0f;
    drive(counts, counts);
    delay(600);                       // settle before sampling
    float rl, rr, nl, nr;
    averageCurrents(10, 40, &rl, &rr, &nl, &nr);

    float vBus = readBusVoltage();
    float bL = vBus * d - v0Drop - nl * rTot;
    float bR = vBus * d - v0Drop - nr * rTot;
    Serial.printf("   %.3f  %6.3f  %6.3f  %6.3f  %6.3f\n", d, nl, nr, bL, bR);

    rows[i][0] = d;
    rows[i][1] = 0.5f * (nl + nr);
    rows[i][2] = 0.5f * (bL + bR);
  }
  stopAll();

  Serial.println(F("\n-- paste into embedded/control/calibration.h --\n"));
  Serial.printf("inline constexpr MotorConstants kMotorLeft  = { %.4ff, %.4ff };\n",
                v0Drop, rTot);
  Serial.printf("inline constexpr MotorConstants kMotorRight = { %.4ff, %.4ff };\n\n",
                v0Drop, rTot);
  Serial.println(F("inline constexpr BaselinePoint kBaselinePoints[] = {"));
  Serial.println(F("  // duty   iFlat   vBemfFlat"));
  for (int i = 0; i < 8; i++)
    Serial.printf("  { %.3ff, %.3ff, %.3ff },\n", rows[i][0], rows[i][1], rows[i][2]);
  Serial.println(F("};"));
  Serial.println(F("inline constexpr int kBaselineCount = 8;\n"));

  bool rising = true;
  for (int i = 1; i < 8; i++) if (rows[i][2] <= rows[i-1][2]) rising = false;
  if (!rising)
    Serial.println(F("WARNING: back-EMF is not rising with duty. Floor not flat,"
                     " wheels slipping, or V0/R_tot are wrong."));
}

// ---------------------------------------------------------------- ui
void help() {
  Serial.println(F("\n=== drive_calib - measure, do not drive ==="));
  Serial.println(F("  order: v (or d) -> L -> o/r -> k"));
  Serial.println(F("  v    set pack voltage by hand, e.g. v12.4"));
  Serial.println(F("  d    toggle the GPIO34 divider as the voltage source"));
  Serial.println(F("  b    read bus voltage now"));
  Serial.println(F("  L    locked-rotor sweep, both sides (HOLD THE WHEELS)"));
  Serial.println(F("  o    set V0,    e.g. o1.9200"));
  Serial.println(F("  r    set R_tot, e.g. r2.4800"));
  Serial.println(F("  k    flat-ground baseline table"));
  Serial.println(F("  c    read current    i  I2C scan    x  stop    ?  help"));
  Serial.printf ("  V_bus %.2f V (%s) -> duty cap %d/255, motor sees %.1f V max\n",
                 readBusVoltage(), useDivider ? "divider" : "manual",
                 dutyMaxCounts(),
                 (readBusVoltage() - v0Drop) * dutyMaxCounts() / 255.0f);
  Serial.printf ("  V0 %.4f V   R_tot %.4f ohm\n\n", v0Drop, rTot);
}

void setup() {
  int enPins[2] = { ENA, ENB };
  for (int i = 0; i < 2; i++) { pinMode(enPins[i], OUTPUT); digitalWrite(enPins[i], LOW); }
  for (int s = 0; s < 2; s++) {
    pinMode(sides[s].inFwd, OUTPUT);
    pinMode(sides[s].inRev, OUTPUT);
    digitalWrite(sides[s].inFwd, LOW);
    digitalWrite(sides[s].inRev, LOW);
    pwmSetup(sides[s].en, s);
  }
  stopAll();

  Serial.begin(115200);
  delay(300);
  sensorsBegin();
  help();
}

void loop() {
  if (!Serial.available()) return;
  char c = Serial.read();
  if (c == '\n' || c == '\r') return;

  switch (c) {
    case 'v': {
      float val = Serial.parseFloat();
      if (val > 3.0f && val < 30.0f) {
        vBusManual = val; useDivider = false;
        Serial.printf("V_bus = %.2f V (manual) -> duty cap %d/255\n",
                      vBusManual, dutyMaxCounts());
      } else Serial.println(F("v: expected 3.0-30.0, e.g. v12.4"));
      break;
    }
    case 'd':
      useDivider = !useDivider;
      Serial.printf("voltage source: %s -> %.2f V\n",
                    useDivider ? "GPIO34 divider" : "manual", readBusVoltage());
      break;

    case 'b':
      Serial.printf("V_bus %.3f V (%s), duty cap %d/255\n",
                    readBusVoltage(), useDivider ? "divider" : "manual",
                    dutyMaxCounts());
      break;

    case 'o': {
      float val = Serial.parseFloat();
      if (val > 0.0f && val < 6.0f) { v0Drop = val; Serial.printf("V0 = %.4f V\n", v0Drop); }
      else Serial.println(F("o: expected 0-6, e.g. o1.9200"));
      break;
    }
    case 'r': {
      float val = Serial.parseFloat();
      if (val > 0.0f && val < 50.0f) { rTot = val; Serial.printf("R_tot = %.4f ohm\n", rTot); }
      else Serial.println(F("r: expected 0-50, e.g. r2.4800"));
      break;
    }

    case 'L': stopAll(); lockedRotorSweep(LEFT); lockedRotorSweep(RIGHT); break;
    case 'k': stopAll(); flatGroundTable();                               break;

    case 'c': {
      readCurrents();
      Serial.printf("LEFT %.4f A (norm %.4f)   RIGHT %.4f A (norm %.4f)\n",
                    isense[0].amps, isense[0].ampsNorm,
                    isense[1].amps, isense[1].ampsNorm);
      break;
    }
    case 'i': i2cScan();  break;
    case 'x':
    case ' ': stopAll(); Serial.println(F("STOP")); break;
    case '?': help();     break;
    default:  Serial.printf("? '%c' - press ? for help\n", c); break;
  }
}
