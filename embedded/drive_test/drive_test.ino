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
 *   ?         reprint this help
 */

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

// ---------------------------------------------------------------- self-test
// Spins each side alone so you can confirm which physical side responds and
// whether it turns the right way. Fix a wrong-way side via its `invert` flag.
void selfTest() {
  Serial.println(F("\n-- self-test: each side alone --"));
  for (int s = 0; s < 2; s++) {
    Serial.printf("  %s forward...\n", sides[s].name);
    setSide(s, speed);
    delay(1200);
    setSide(s, 0);
    delay(400);

    Serial.printf("  %s reverse...\n", sides[s].name);
    setSide(s, -speed);
    delay(1200);
    setSide(s, 0);
    delay(600);
  }
  Serial.println(F("-- done. Did each side turn the way it was named?"));
  Serial.println(F("   If a side ran backwards, set invert=true for it in the"));
  Serial.println(F("   sides[] table and re-upload.\n"));
}

void help() {
  Serial.println(F("\n=== drive test - single L298N, 2 channels ==="));
  Serial.println(F("  w/s  forward / reverse"));
  Serial.println(F("  a/d  spin left / spin right"));
  Serial.println(F("  q/e  arc left / arc right"));
  Serial.println(F("  spc  stop      (x also stops)"));
  Serial.println(F("  +/-  speed     t  self-test     ?  help"));
  Serial.println(F("  1/2  probe LEFT / RIGHT: static levels for a multimeter"));
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
  help();
}

void loop() {
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

    case 't': stopAll(); selfTest(); break;
    case '?': help();               break;

    default:
      Serial.printf("? '%c' - press ? for help\n", c);
      break;
  }
}
