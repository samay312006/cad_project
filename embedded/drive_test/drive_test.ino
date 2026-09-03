/*
 * drive_test.ino — keyboard drive bring-up test (single L298N, 2 channels)
 *
 * SCOPE: tethered bench test only. Verifies the one surviving L298N, all four
 * motors, and the ESP32 pin map before any autonomy is layered on. Current
 * sensors (INA219) are NOT used here — they aren't wired yet.
 *
 * NOT the real firmware. No SAFE_HOLD, no link-loss watchdog, no mode state
 * machine (see spec.md §5 / overview_controls.md §5). Commands are LATCHED: the
 * bot keeps doing the last thing you told it until you press another key. Run it
 * with the wheels OFF THE GROUND the first time.
 *
 * >> CURRENT WARNING <<
 * Both motors on a side are wired in PARALLEL onto one L298N channel, so that
 * channel carries DOUBLE the current of a single motor. The L298N is ~2A
 * continuous per channel and has no thermal margin to spare. Do not stall the
 * wheels, do not hold a spin against a load, and put a heatsink on the chip.
 * This is the most likely way the first board died.
 *
 * Usage: upload, open Serial Monitor at 115200 baud, set line ending to
 * "No Line Ending", click into the input box, then press keys.
 *
 *   w / s     forward / reverse
 *   a / d     spin left / spin right (skid-steer, sides counter-rotate)
 *   q / e     arc left / arc right (inner side at half speed)
 *   space / x stop
 *   + / -     speed up / down (steps of 15)
 *   t         self-test: each side alone, forward then reverse
 *   1 / 2     probe LEFT / RIGHT with static levels (for a multimeter)
 *   c         read drive current from the INA219s
 *   i         I2C bus scan (use this to confirm sensor addresses)
 *   ?         reprint this help
 */

#include <Wire.h>
#include <Adafruit_INA219.h>

// ---------------------------------------------------------------- pin map
// ONE L298N. Channel A drives OUT1/OUT2, channel B drives OUT3/OUT4.
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
// INA219 current sensors (x2, one per drive channel). Both share one I2C bus;
// only the address differs. Logic side, identical for both boards:
//   ESP32 3V3  ---> VCC
//   ESP32 GND  ---> GND
//   GPIO21     ---> SDA
//   GPIO22     ---> SCL
//
// Shunt side: the L298N feeds BOTH bridges from one VS pin, so there is no
// per-channel supply feed to sense. Each shunt therefore goes in that channel's
// motor RETURN lead:
//   LEFT  0x40 : cut OUT3 -> left  motors(-).  OUT3 to VIN+, VIN- to motors(-)
//   RIGHT 0x41 : cut OUT1 -> right motors(-).  OUT1 to VIN+, VIN- to motors(-)
// If a side reads negative when driving forward, set its invert flag below.
//
// These six pins deliberately avoid GPIO2 and GPIO15 (boot strapping pins the
// old right-hand board used). Nothing here can stop the ESP32 booting.
#define ENA 25
#define IN1 26
#define IN2 27
#define ENB 32
#define IN3 33
#define IN4 14

// ---------------------------------------------------------------- tuning
const int PWM_FREQ_HZ = 1000;   // L298N is slow; 1 kHz is a safe default
const int PWM_BITS    = 8;      // duty range 0..255
const int SPEED_MIN   = 60;     // below this the L298N drop stalls the motors
const int SPEED_MAX   = 255;
const int SPEED_STEP  = 15;

int speed = 180;                // current commanded duty

// A "side" is one L298N channel driving both wheels on that side in parallel.
// The chassis is skid-steer, so a side is one logical drive channel anyway —
// this also matches the per-side current sensing in spec.md §6.1.
//
// inFwd is the IN pin feeding that side's POSITIVE motor lead, so driving it
// HIGH always means forward. That's what encodes your OUT2-positive /
// OUT4-positive wiring. Flip `invert` if a side still runs backwards.
struct Side {
  const char *name;
  int  en, inFwd, inRev;
  bool invert;
};

Side sides[2] = {
  //  name     EN   inFwd  inRev  invert
  { "LEFT",   ENB,  IN4,   IN3,   false },   // OUT4 = +, OUT3 = -
  { "RIGHT",  ENA,  IN2,   IN1,   false },   // OUT2 = +, OUT1 = -
};
const int LEFT = 0, RIGHT = 1;

// ------------------------------------------------------- current sensing
// Exactly two sensors, one per drive channel - that is all this topology can
// observe, since both motors on a side are paralleled onto one channel.
// Both boards are now fitted. A slot whose `present` is false, or whose board
// fails to answer at boot, is skipped cleanly rather than hanging the sketch.
#define I2C_SDA 21
#define I2C_SCL 22

Adafruit_INA219 ina_left (0x40);   // no solder jumpers
Adafruit_INA219 ina_right(0x41);   // A0 bridged

struct CurrentSensor {
  const char      *name;
  Adafruit_INA219 *dev;
  uint8_t          addr;
  bool             present;   // false = not fitted, skipped
  bool             invert;    // true if forward drive reads negative
  float            amps;      // last raw reading
  float            ampsNorm;  // raw / duty - see readCurrents()
};

CurrentSensor isense[2] = {
  //  name     device        addr  present invert  amps normd
  { "LEFT",  &ina_left,  0x40, true,   false,  0.0f, 0.0f },
  { "RIGHT", &ina_right, 0x41, true,   false,  0.0f, 0.0f },
};

// Magnitude of the duty currently commanded to each side, 0..255. readCurrents()
// needs it, see the normalization note there.
int lastDuty[2] = { 0, 0 };

// Below this duty the I/D division amplifies noise faster than it corrects, so
// the normalized value is reported as unavailable rather than as garbage.
const float DUTY_NORM_MIN = 0.15f;

// Live current stream. On from boot so readings scroll without being asked for;
// `m` toggles it off when the scroll gets in the way of reading other output.
// Non-blocking, so driving still works while it runs.
bool     monitorOn   = true;
uint32_t monitorNext = 0;
const uint32_t MONITOR_MS = 250;

// ---------------------------------------------------------------- PWM shim
// ESP32 Arduino core 3.x attaches PWM per pin; 2.x uses explicit channels.
void pwmSetup(int pin, int ch) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcAttach(pin, PWM_FREQ_HZ, PWM_BITS);
#else
  ledcSetup(ch, PWM_FREQ_HZ, PWM_BITS);
  ledcAttachPin(pin, ch);
#endif
}

void pwmWrite(int pin, int ch, int duty) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  (void)ch;
  ledcWrite(pin, duty);
#else
  (void)pin;
  ledcWrite(ch, duty);
#endif
}

// Release a pin from the PWM peripheral so it can be driven as a plain GPIO.
void pwmDetach(int pin) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  ledcDetach(pin);
#else
  ledcDetachPin(pin);
#endif
}

// ---------------------------------------------------------------- drive
// duty: -255..255. Negative = reverse. Zero = coast (both IN low, EN low).
void setSide(int s, int duty) {
  Side &sd = sides[s];
  if (sd.invert) duty = -duty;

  bool fwd = duty >= 0;
  int  mag = abs(duty);
  if (mag > SPEED_MAX) mag = SPEED_MAX;

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

void drive(int left, int right) {
  setSide(LEFT,  left);
  setSide(RIGHT, right);
}

void stopAll() {
  drive(0, 0);
}

// ---------------------------------------------------------------- probe mode
// Static-level probe for tracing a side that won't turn. PWM is detached and EN
// is driven as a plain DC high, so the OUT pair reads as steady DC on a
// multimeter instead of a 1 kHz square wave the meter would average into
// something meaningless. One side at a time, latched until you stop it.
int  probeSel = -1;      // side under probe, -1 = none
bool probeOn  = false;

void probeExit() {
  if (!probeOn) return;
  Side &sd = sides[probeSel];
  digitalWrite(sd.inFwd, LOW);
  digitalWrite(sd.inRev, LOW);
  digitalWrite(sd.en,    LOW);
  pwmSetup(sd.en, probeSel);       // hand the pin back to the PWM peripheral
  probeOn  = false;
  probeSel = -1;
  stopAll();
  Serial.println(F("probe off - PWM re-attached"));
}

void probeEnter(int s) {
  probeExit();
  Side &sd = sides[s];
  probeSel = s;
  probeOn  = true;

  pwmDetach(sd.en);
  pinMode(sd.en, OUTPUT);
  digitalWrite(sd.en,    HIGH);
  digitalWrite(sd.inFwd, HIGH);
  digitalWrite(sd.inRev, LOW);

  Serial.printf("\n-- probe: %s held static forward, no PWM --\n", sd.name);
  Serial.println(F("   Black meter lead on ESP32 GND for these three:"));
  Serial.printf ("     EN    GPIO%-2d = HIGH  expect ~3.3V\n", sd.en);
  Serial.printf ("     inFwd GPIO%-2d = HIGH  expect ~3.3V\n", sd.inFwd);
  Serial.printf ("     inRev GPIO%-2d = LOW   expect ~0V\n",   sd.inRev);
  Serial.println(F("   Any of those wrong -> fault is the ESP32 pin or its"));
  Serial.println(F("   jumper. Stop there."));
  Serial.println(F("   All three right -> meter this side's OUT pair:"));
  Serial.println(F("     ~supply minus 2V -> driver works, suspect motor/leads"));
  Serial.println(F("     ~0V              -> L298N not driving; check its 5V"));
  Serial.println(F("                         logic rail and the common ground"));
  Serial.println(F("   space stops. 1/2 probes the other side.\n"));
}

void sensorsBegin() {
  Wire.begin(I2C_SDA, I2C_SCL);
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) {
      Serial.printf("INA219 %-5s : slot disabled in isense[]\n", cs.name);
      continue;
    }
    if (!cs.dev->begin()) {
      cs.present = false;
      Serial.printf("INA219 %-5s : NOT FOUND at 0x%02X - press i to scan the bus\n",
                    cs.name, cs.addr);
      continue;
    }
    // Default range: calibrated to 2A, counter overflows at 3.2A. Two motors in
    // parallel WILL exceed that near stall - see the note in help().
    cs.dev->setCalibration_32V_2A();
    Serial.printf("INA219 %-5s : ok at 0x%02X\n", cs.name, cs.addr);
  }
}

// PWM on the EN pin coasts the bridge during off-time, so shunt current is ~0
// for that fraction of every cycle and the averaged reading comes back as
// roughly (duty x actual motor current). Dividing by duty recovers the real
// motor current. Skipping this step puts the duty term on BOTH sides of any
// closed loop built on it - raise duty, read more current, raise duty again -
// which is positive feedback with no load change at all. Always control on
// ampsNorm, never on amps.
void readCurrents() {
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) { cs.amps = NAN; cs.ampsNorm = NAN; continue; }

    float a = cs.dev->getCurrent_mA() / 1000.0f;
    if (cs.invert) a = -a;
    cs.amps = a;

    float d = lastDuty[i] / 255.0f;
    cs.ampsNorm = (d >= DUTY_NORM_MIN) ? (a / d) : NAN;
  }
}

void printCurrents() {
  readCurrents();
  Serial.println(F("\n-- drive current --"));
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) { Serial.printf("  %-5s : absent\n", cs.name); continue; }
    Serial.printf("  %-5s : raw %6.3f A   duty %3d", cs.name, cs.amps, lastDuty[i]);
    if (isnan(cs.ampsNorm)) Serial.println(F("   norm    --   (duty too low)"));
    else                    Serial.printf("   norm %6.3f A\n", cs.ampsNorm);
  }
  Serial.println();
}

// One compact line per sample, for the `m` stream. Same numbers printCurrents()
// shows, laid out to be readable as it scrolls.
void printCurrentsLine() {
  readCurrents();
  Serial.print(F("I: "));
  for (int i = 0; i < 2; i++) {
    CurrentSensor &cs = isense[i];
    if (!cs.present) { Serial.printf("%-5s absent        ", cs.name); continue; }
    Serial.printf("%-5s %6.3fA d%3d ", cs.name, cs.amps, lastDuty[i]);
    if (isnan(cs.ampsNorm)) Serial.print(F("n   --   "));
    else                    Serial.printf("n %6.3fA ", cs.ampsNorm);
    Serial.print(F("  "));
  }
  Serial.println();
}

void i2cScan() {
  Serial.println(F("\n-- I2C scan --"));
  int found = 0;
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) {
      Serial.printf("  device at 0x%02X\n", a);
      found++;
    }
  }
  if (!found) Serial.println(F("  nothing found - check VCC / GND / SDA / SCL"));
  Serial.println();
}

// Prints the driven side next to the idle one. During a single-side spin the
// driven sensor should climb and the idle one sit near zero; anything else means
// the shunts are in the wrong channels or one is not in the motor return lead.
void reportSideCurrent(int driven) {
  int idle = 1 - driven;
  Serial.printf("    driven %-5s ", isense[driven].name);
  if (isense[driven].present) Serial.printf("%6.3f A", isense[driven].amps);
  else                        Serial.print(F("  absent"));
  Serial.printf("   idle %-5s ", isense[idle].name);
  if (isense[idle].present)   Serial.printf("%6.3f A\n", isense[idle].amps);
  else                        Serial.println(F("  absent"));
}

// ---------------------------------------------------------------- self-test
// Spins each side alone so you can confirm which physical side responds and
// whether it turns the right way. Fix a wrong-way side via its `invert` flag.
void selfTest() {
  Serial.println(F("\n-- self-test: each side alone --"));
  for (int s = 0; s < 2; s++) {
    Serial.printf("  %s forward...\n", sides[s].name);
    setSide(s, speed);
    delay(600);          // let the motor reach steady state before sampling
    readCurrents();
    reportSideCurrent(s);
    delay(600);
    setSide(s, 0);
    delay(400);

    Serial.printf("  %s reverse...\n", sides[s].name);
    setSide(s, -speed);
    delay(600);
    readCurrents();
    reportSideCurrent(s);
    delay(600);
    setSide(s, 0);
    delay(600);
  }
  Serial.println(F("-- done. Did each side turn the way it was named?"));
  Serial.println(F("   If a side ran backwards, set invert=true for it in the"));
  Serial.println(F("   sides[] table and re-upload."));
  Serial.println(F("   Each side should draw current ONLY while it is the one"));
  Serial.println(F("   named as spinning. If the other sensor moves instead,"));
  Serial.println(F("   the two shunts are swapped between the channels."));
  Serial.println(F("   A sign that stays negative means invert=true in"));
  Serial.println(F("   isense[] for that sensor.\n"));
}

void help() {
  Serial.println(F("\n=== drive test - single L298N, 2 channels ==="));
  Serial.println(F("  w/s  forward / reverse"));
  Serial.println(F("  a/d  spin left / spin right"));
  Serial.println(F("  q/e  arc left / arc right"));
  Serial.println(F("  spc  stop      (x also stops)"));
  Serial.println(F("  +/-  speed     t  self-test     ?  help"));
  Serial.println(F("  1/2  probe LEFT / RIGHT: static levels for a multimeter"));
  Serial.println(F("  c    read drive current    i  I2C bus scan"));
  Serial.println(F("  m    current stream on/off (ON at boot, keeps driving)"));
  Serial.printf ("  speed = %d / %d\n", speed, SPEED_MAX);
  Serial.println(F("  NOTE: 2 motors per channel - do not stall the wheels.\n"));
}

// ---------------------------------------------------------------- lifecycle
void setup() {
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

  Serial.begin(115200);
  delay(300);
  sensorsBegin();
  help();
}

void loop() {
  if (monitorOn && (int32_t)(millis() - monitorNext) >= 0) {
    monitorNext = millis() + MONITOR_MS;
    printCurrentsLine();
  }

  if (!Serial.available()) return;

  char c = Serial.read();
  if (c == '\n' || c == '\r') return;

  // Any motion command leaves probe mode first, so EN gets its PWM back.
  if (probeOn && strchr("wsadqe x+-=_t", c)) probeExit();

  switch (c) {
    case 'w': drive( speed,  speed);         Serial.println(F("forward"));    break;
    case 's': drive(-speed, -speed);         Serial.println(F("reverse"));    break;
    case 'a': drive(-speed,  speed);         Serial.println(F("spin left"));  break;
    case 'd': drive( speed, -speed);         Serial.println(F("spin right")); break;
    case 'q': drive( speed / 2,  speed);     Serial.println(F("arc left"));   break;
    case 'e': drive( speed,  speed / 2);     Serial.println(F("arc right"));  break;

    case ' ':
    case 'x': stopAll();                     Serial.println(F("STOP"));       break;

    case '+':
    case '=':
      speed = min(speed + SPEED_STEP, SPEED_MAX);
      Serial.printf("speed = %d\n", speed);
      break;

    case '-':
    case '_':
      speed = max(speed - SPEED_STEP, SPEED_MIN);
      Serial.printf("speed = %d\n", speed);
      break;

    case '1': probeEnter(LEFT);  break;
    case '2': probeEnter(RIGHT); break;

    case 'c': printCurrents(); break;
    case 'i': i2cScan();       break;

    case 'm':
      monitorOn   = !monitorOn;
      monitorNext = millis();
      Serial.printf("current stream %s\n", monitorOn ? "ON" : "off");
      break;

    case 't': stopAll(); selfTest(); break;
    case '?': help();               break;

    default:
      Serial.printf("? '%c' - press ? for help\n", c);
      break;
  }
}
