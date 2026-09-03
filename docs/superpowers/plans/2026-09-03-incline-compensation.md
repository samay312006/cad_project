# Incline Compensation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the ESP32 control core that detects sustained drive load above a flat-ground baseline and raises PWM to hold speed, validated in native simulation before it drives real motors.

**Architecture:** A portable C++ control core (`embedded/control/`) with no Arduino headers, sitting behind a HAL (`embedded/hal/`). The core is a pure function of its inputs, so the native Linux test build runs exactly the code that ships to the ESP32. A back-EMF speed-hold PI regulates `V_bemf` against a calibrated flat-ground table; the two INA219 channels serve as load detector, stall gate and current ceiling.

**Tech Stack:** C++17, GNU Make, g++ 11.4 (native tests); Arduino ESP32 core 3.3.11, arduino-cli 1.5.2-rc.1 (firmware); Adafruit INA219 library.

**Spec:** `docs/superpowers/specs/2026-09-03-incline-compensation-design.md`

## Global Constraints

- **Motors are rated 3.0–6.0 V** (`FAM1029` datasheet). Supply to the L298N is 12 V. `V_MOTOR_MAX = 5.0 V`.
- **L298N is rated 2 A per channel.** `I_CEIL = 1.8 A` per channel.
- **Two motors are paralleled per channel**, one channel per side (`UPDATES.md` #5). A side is one logical drive channel.
- **The duty clamp is the only protection against over-voltage** (design decision D5). It lives inside the PWM write and is recomputed from measured `V_bus` every cycle.
- **`spec.md` §6 is the shared source of truth for wire formats.** Do not change a shared schema without an `UPDATES.md` row.
- **Do not write AWS, SLAM/Nav2/D*-Lite, MQTT-broker, or Android/GUI code.** This is the embedded role. Flag in `UPDATES.md` instead.
- Control rate 100 Hz; PI rate 50 Hz.
- Stall detection within 200 ms; incline response within 500 ms (`spec.md` §8).
- Every scenario in Task 11 passes in sim before Task 13 puts the loop on hardware (NFR-5).

## File Structure

| File | Responsibility |
|---|---|
| `embedded/control/motor_model.h/.cpp` | Affine back-EMF model; locked-rotor solve for `V0`/`R_tot` |
| `embedded/control/baseline.h/.cpp` | Flat-ground calibration table + interpolation |
| `embedded/control/load_detector.h/.cpp` | Duty normalization, low-pass filter, left/right symmetry |
| `embedded/control/safety.h/.cpp` | Duty clamp, current ceiling, overflow, stall, brownout |
| `embedded/control/incline_comp.h/.cpp` | The PI: saturation, anti-windup, slew limit |
| `embedded/control/drive_core.h/.cpp` | Orchestrator — the one entry point the HAL calls |
| `embedded/control/calibration.h` | Measured constants from Task 2, committed to git |
| `embedded/control/test/test_util.h` | Minimal zero-dependency assert harness |
| `embedded/control/test/*.cpp` | Unit tests per module; `test_scenarios.cpp` for the eight cases |
| `embedded/hal/hal.h` | Abstract HAL interface |
| `embedded/hal/hal_sim.cpp/.h` | Motor + gearbox + grade model, native |
| `embedded/hal/hal_esp32.cpp/.h` | INA219 + ADC divider + LEDC |
| `embedded/Makefile` | Native test build |
| `embedded/drive_test/drive_test.ino` | Bench tool — gains the clamp and calibration commands |

---

### Task 1: Duty clamp in the bench tool

This ships first because every later bench step drives real motors, and right now
nothing stops `drive_test` from putting 10.1 V across a 6 V motor. `SPEED_MAX` is
255 at `drive_test.ino:77`.

The divider hardware does not exist yet, so `V_bus` is entered manually here and the
default is the worst case (a fully charged pack). A higher assumed `V_bus` produces a
*tighter* clamp, so an unset value fails safe.

**Files:**
- Modify: `embedded/drive_test/drive_test.ino:74-80` (constants), `:145-164` (`pwmWrite`), `:392-396` (`help`), `:400-470` (`loop`)

**Interfaces:**
- Consumes: nothing
- Produces: bench-verified clamp behavior; the `v` serial command used by Task 2

- [ ] **Step 1: Add the clamp constants and state**

Replace the constants block at `drive_test.ino:74-80`:

```c
const int PWM_FREQ_HZ = 1000;   // L298N is slow; 1 kHz is a safe default
const int PWM_BITS    = 8;
const int SPEED_MIN   = 60;     // below this the L298N drop stalls the motors
const int SPEED_MAX   = 128;    // was 255 - see V_MOTOR_MAX below
const int SPEED_STEP  = 15;

int speed = 100;                // current commanded duty

// ------------------------------------------------------- motor protection
// The FAM1029 motors are rated 3.0-6.0 V and the L298N is fed 12 V, so full
// duty puts ~10.1 V across them. Firmware is the ONLY thing preventing that
// (design decision D5), so the clamp lives inside pwmWrite() - one choke point
// no code path can route around.
//
// Until the resistor divider exists, vBusAssumed is entered with `v` and
// defaults to a charged pack. Higher assumed V_bus = tighter clamp, so an
// unset value fails safe.
const float V_MOTOR_MAX = 5.0f;   // 1 V below the datasheet maximum
float vBusAssumed = 12.6f;        // charged 3S; override with `v`
float v0Drop      = 1.9f;         // L298N offset drop, refined by Task 2

int dutyMaxCounts() {
  float head = vBusAssumed - v0Drop;
  if (head <= 0.1f) return 0;
  float d = V_MOTOR_MAX / head;
  if (d > 1.0f) d = 1.0f;
  if (d < 0.0f) d = 0.0f;
  return (int)(d * 255.0f);
}
```

- [ ] **Step 2: Enforce the clamp inside the PWM write**

Replace the body of `pwmWrite` at `drive_test.ino:145-164` (keep the `#if` shim):

```c
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
```

- [ ] **Step 3: Add the `v` command and report the cap**

Add to `help()` after the current-stream line:

```c
  Serial.println(F("  v    set assumed V_bus, e.g. v12.4 (multimeter reading)"));
  Serial.printf ("  V_bus %.2f V -> duty cap %d/255 (%.1f V at the motor)\n",
                 vBusAssumed, dutyMaxCounts(),
                 (vBusAssumed - v0Drop) * dutyMaxCounts() / 255.0f);
```

Add to the `switch` in `loop()` next to `case 'c'`:

```c
    case 'v': {
      float val = Serial.parseFloat();
      if (val > 3.0f && val < 30.0f) {
        vBusAssumed = val;
        Serial.printf("V_bus = %.2f V -> duty cap %d/255\n",
                      vBusAssumed, dutyMaxCounts());
      } else {
        Serial.println(F("v: expected 3.0-30.0, e.g. v12.4"));
      }
      break;
    }
```

- [ ] **Step 4: Compile and upload**

```bash
~/bin/arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 embedded/drive_test
~/bin/arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32doit-devkit-v1 embedded/drive_test
```

Close the IDE Serial Monitor first or the upload fails with "port is busy".

- [ ] **Step 5: Verify the clamp on the bench**

Measure the battery with a multimeter, enter it with `v`, then press `+` repeatedly
to reach `SPEED_MAX` and drive forward with `w`.

Expected: with `v12.4`, the cap prints as `121/255`. With a meter across one motor's
terminals while driving, average voltage reads at or below 5.0 V. Pressing `+` past
128 does not raise it further.

This measurement is the whole point of the task — do not skip it. If the motor
voltage exceeds 5.0 V, stop and recheck `v0Drop` before any further bench work.

- [ ] **Step 6: Commit**

```bash
git add embedded/drive_test/drive_test.ino
git commit -m "feat(drive_test): clamp duty to keep 6 V motors off a 12 V rail"
```

---

### Task 2: Bench calibration

Produces the four numbers the whole design rests on. This is measurement on the bench
tool, not the control loop running on hardware — the local-first constraint is not
engaged.

**Files:**
- Modify: `embedded/drive_test/drive_test.ino` (add `L` and `k` commands)
- Create: `embedded/control/calibration.h`

**Interfaces:**
- Consumes: `v` command and `dutyMaxCounts()` from Task 1
- Produces: `calibration.h` defining `kMotorConstants` and `kBaselinePoints[]`, consumed by Tasks 4, 5 and 11

- [ ] **Step 1: Add the locked-rotor command**

Add before `help()` in `drive_test.ino`:

```c
// Locked rotor: V_bemf is zero by definition, so V_bus*d = V0 + I*R_tot.
// Two duty points give two equations in two unknowns. Bursts are kept short -
// locked rotor is worst case for both motor and bridge.
void lockedRotorTest(int s) {
  const int duties[2] = { 31, 51 };      // 0.12 and 0.20 of 255
  float vApp[2], amps[2];

  Serial.printf("\n-- locked rotor, %s --\n", sides[s].name);
  Serial.println(F("   HOLD THE WHEELS FIRMLY. Starting in 3 s."));
  delay(3000);

  for (int i = 0; i < 2; i++) {
    setSide(s, duties[i]);
    delay(400);                          // settle, stay under 2 s total
    readCurrents();
    amps[i] = isense[s].amps;            // raw: at locked rotor duty is applied
    vApp[i] = vBusAssumed * duties[i] / 255.0f;
    setSide(s, 0);
    Serial.printf("   d=%.3f  V_app=%.3f V  I=%.3f A\n",
                  duties[i] / 255.0f, vApp[i], amps[i]);
    delay(600);
  }

  float di = amps[1] - amps[0];
  if (di < 0.02f) {
    Serial.println(F("   FAIL: currents too close. Was the rotor actually held?"));
    return;
  }
  float rTot = (vApp[1] - vApp[0]) / di;
  float v0   = vApp[0] - amps[0] * rTot;
  Serial.printf("   R_tot = %.4f ohm   V0 = %.4f V\n", rTot, v0);
  Serial.printf("   implied stall current at 6 V: %.2f A\n",
                (6.0f - v0) / rTot);
}
```

- [ ] **Step 2: Add the flat-ground table command**

```c
// Eight points from 0.20 up to the clamp. Drive straight, flat ground, no
// payload. Produces both the load baseline and the speed target.
void flatGroundTable() {
  Serial.println(F("\n-- flat-ground baseline --"));
  Serial.println(F("   Robot on flat ground, clear run ahead. Starting in 3 s."));
  delay(3000);
  Serial.println(F("   duty      I_L      I_R    Vbemf_L  Vbemf_R"));

  int cap = dutyMaxCounts();
  for (int i = 0; i < 8; i++) {
    int counts = 51 + (cap - 51) * i / 7;     // 0.20 .. clamp
    float d = counts / 255.0f;
    drive(counts, counts);
    delay(500);                                // settle
    float sumL = 0, sumR = 0;
    for (int n = 0; n < 10; n++) { readCurrents();
                                   sumL += isense[0].ampsNorm;
                                   sumR += isense[1].ampsNorm;
                                   delay(50); }
    float iL = sumL / 10.0f, iR = sumR / 10.0f;
    float bL = vBusAssumed * d - v0Drop - iL * 2.5f;   // R_tot placeholder
    float bR = vBusAssumed * d - v0Drop - iR * 2.5f;
    Serial.printf("   %.3f  %7.3f  %7.3f  %7.3f  %7.3f\n", d, iL, iR, bL, bR);
  }
  stopAll();
  Serial.println(F("-- done. Paste into control/calibration.h --"));
}
```

- [ ] **Step 3: Wire both into `help()` and `loop()`**

In `help()`:

```c
  Serial.println(F("  L    locked-rotor test (hold wheels) - R_tot and V0"));
  Serial.println(F("  k    flat-ground baseline table"));
```

In the `loop()` switch:

```c
    case 'L': stopAll(); lockedRotorTest(LEFT); lockedRotorTest(RIGHT); break;
    case 'k': stopAll(); flatGroundTable();                             break;
```

- [ ] **Step 4: Compile, upload, and run both procedures**

```bash
~/bin/arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 embedded/drive_test
~/bin/arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32doit-devkit-v1 embedded/drive_test
```

Run `v<measured>` first, then `L`, then `k`.

Expected from `L`: `R_tot` in the range 1–4 Ω per channel, `V0` in the range
1.2–2.6 V. A negative `V0` or an `R_tot` above 10 Ω means the rotor moved — redo it.

Expected from `k`: current rising monotonically with duty; `V_bemf` rising and
staying positive.

- [ ] **Step 5: Check the INA219 common-mode voltage on a scope**

`[VERIFY]` from the design's open items, and the last one that needs hardware.

The shunts sit in the motor return leads, and during PWM off-time the L298N coasts
with both outputs high-Z — the shunt's common-mode voltage is then held up only by
the body diodes. It must stay inside the INA219's 0–26 V window relative to the
ESP32's ground.

Probe `VIN−` against ESP32 GND while driving at `speed = 100`. Expected: the
waveform stays between 0 V and the pack voltage, with no negative excursion below
about −0.5 V and no ringing above 26 V.

If it swings negative beyond a diode drop, stop — the readings from Task 2 are not
trustworthy and the shunt placement needs revisiting before any constant derived
from it is committed.

- [ ] **Step 6: Record the constants**

Create `embedded/control/calibration.h` with the real measured numbers substituted
for the illustrative ones below:

```c
#pragma once
#include "motor_model.h"
#include "baseline.h"

// Measured 2026-09-03 by drive_test `L` and `k`. Re-run after any change to the
// motors, the bridge, or the battery.
inline constexpr MotorConstants kMotorLeft  = { 1.92f, 2.48f };   // V0, R_tot
inline constexpr MotorConstants kMotorRight = { 1.89f, 2.51f };

inline constexpr BaselinePoint kBaselinePoints[] = {
  // duty   iFlat   vBemfFlat
  { 0.200f, 0.181f, 0.383f },
  { 0.243f, 0.198f, 0.833f },
  { 0.286f, 0.214f, 1.284f },
  { 0.329f, 0.231f, 1.734f },
  { 0.371f, 0.248f, 2.185f },
  { 0.414f, 0.265f, 2.635f },
  { 0.457f, 0.282f, 3.086f },
  { 0.500f, 0.299f, 3.536f },
};
inline constexpr int kBaselineCount = 8;
```

- [ ] **Step 7: Commit**

```bash
git add embedded/drive_test/drive_test.ino embedded/control/calibration.h
git commit -m "feat(calib): locked-rotor and flat-ground calibration, measured constants"
```

---

### Task 3: Native test harness

**Files:**
- Create: `embedded/control/test/test_util.h`, `embedded/Makefile`

**Interfaces:**
- Consumes: nothing
- Produces: `CHECK`, `CHECK_NEAR`, `RUN`, `testSummary()`; `make test` target used by every later task

- [ ] **Step 1: Write the harness**

Create `embedded/control/test/test_util.h`:

```cpp
#pragma once
#include <cstdio>
#include <cmath>

inline int g_checks   = 0;
inline int g_failures = 0;

#define CHECK(cond)                                                       \
  do {                                                                    \
    g_checks++;                                                           \
    if (!(cond)) {                                                        \
      g_failures++;                                                       \
      std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond);       \
    }                                                                     \
  } while (0)

#define CHECK_NEAR(a, b, tol)                                             \
  do {                                                                    \
    g_checks++;                                                           \
    double _a = (a), _b = (b), _t = (tol);                                \
    if (!(std::fabs(_a - _b) <= _t)) {                                    \
      g_failures++;                                                       \
      std::printf("  FAIL %s:%d  %s = %g, expected %g +/- %g\n",          \
                  __FILE__, __LINE__, #a, _a, _b, _t);                    \
    }                                                                     \
  } while (0)

#define RUN(fn)                                                           \
  do { std::printf("== %s\n", #fn); fn(); } while (0)

inline int testSummary() {
  std::printf("%d checks, %d failures\n", g_checks, g_failures);
  return g_failures ? 1 : 0;
}
```

- [ ] **Step 2: Write the Makefile**

Create `embedded/Makefile`:

```make
CXX      := g++
CXXFLAGS := -std=c++17 -Wall -Wextra -Werror -O1 -g -Icontrol -Ihal

SRC  := $(wildcard control/*.cpp) $(wildcard hal/hal_sim.cpp)
TEST := $(wildcard control/test/*.cpp)
BIN  := build/run_tests

.PHONY: test clean
test: $(BIN)
	./$(BIN)

$(BIN): $(SRC) $(TEST)
	@mkdir -p build
	$(CXX) $(CXXFLAGS) $(SRC) $(TEST) -o $@

clean:
	rm -rf build
```

- [ ] **Step 3: Add a placeholder test main so the target builds**

Create `embedded/control/test/test_main.cpp`:

```cpp
#include "test_util.h"

void testHarnessWorks() {
  CHECK(1 + 1 == 2);
  CHECK_NEAR(0.1 + 0.2, 0.3, 1e-9);
}

int main() {
  RUN(testHarnessWorks);
  return testSummary();
}
```

- [ ] **Step 4: Run it**

```bash
cd embedded && make test
```

Expected: `== testHarnessWorks` then `2 checks, 0 failures`, exit 0.

- [ ] **Step 5: Commit**

```bash
git add embedded/Makefile embedded/control/test/
git commit -m "build: native C++17 test harness for the control core"
```

---

### Task 4: Motor model

**Files:**
- Create: `embedded/control/motor_model.h`, `embedded/control/motor_model.cpp`, `embedded/control/test/test_motor_model.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `test_util.h` (Task 3)
- Produces: `struct MotorConstants { float v0; float rTot; }`, `float bemfVolts(const MotorConstants&, float vBus, float duty, float iNorm)`, `bool solveLockedRotor(float vApp1, float i1, float vApp2, float i2, MotorConstants* out)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_motor_model.cpp`:

```cpp
#include "motor_model.h"
#include "test_util.h"

void testBemfFromTerminalModel() {
  MotorConstants mc{1.9f, 2.5f};
  // 12 V * 0.5 duty = 6.0 applied; minus 1.9 offset; minus 0.4 * 2.5 = 1.0
  CHECK_NEAR(bemfVolts(mc, 12.0f, 0.5f, 0.4f), 3.1f, 1e-4);
}

void testBemfGoesNegativeUnderStall() {
  MotorConstants mc{1.9f, 2.5f};
  // High current at low duty: the model must not clamp at zero, because the
  // stall gate reads this value.
  CHECK(bemfVolts(mc, 12.0f, 0.2f, 1.5f) < 0.0f);
}

void testSolveLockedRotorRecoversConstants() {
  MotorConstants out{};
  // Constructed from v0=1.9, rTot=2.5: V_app = 1.9 + I*2.5
  CHECK(solveLockedRotor(2.4f, 0.2f, 3.4f, 0.6f, &out));
  CHECK_NEAR(out.rTot, 2.5f, 1e-3);
  CHECK_NEAR(out.v0, 1.9f, 1e-3);
}

void testSolveLockedRotorRejectsDegenerateInput() {
  MotorConstants out{};
  CHECK(!solveLockedRotor(2.4f, 0.30f, 3.4f, 0.31f, &out));
}
```

- [ ] **Step 2: Register the tests and run to verify failure**

Replace `embedded/control/test/test_main.cpp`:

```cpp
#include "test_util.h"

void testHarnessWorks();
void testBemfFromTerminalModel();
void testBemfGoesNegativeUnderStall();
void testSolveLockedRotorRecoversConstants();
void testSolveLockedRotorRejectsDegenerateInput();

int main() {
  RUN(testHarnessWorks);
  RUN(testBemfFromTerminalModel);
  RUN(testBemfGoesNegativeUnderStall);
  RUN(testSolveLockedRotorRecoversConstants);
  RUN(testSolveLockedRotorRejectsDegenerateInput);
  return testSummary();
}
```

Move `testHarnessWorks` into `embedded/control/test/test_harness.cpp` (same body,
without `main`).

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: motor_model.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `embedded/control/motor_model.h`:

```cpp
#pragma once

// Affine terminal model for one drive channel. Folds motor winding resistance
// and the L298N's current-dependent drop into a single pair of constants,
// measured by the locked-rotor procedure rather than taken from a datasheet.
struct MotorConstants {
  float v0;     // V, bridge offset drop
  float rTot;   // ohm, motor + bridge series resistance for the channel
};

// Back-EMF for the channel. duty is the APPLIED duty (post-boost), 0..1.
// iNorm is duty-normalized current in amps. Result may be negative - the stall
// gate depends on that, so it is deliberately not clamped.
float bemfVolts(const MotorConstants &mc, float vBus, float duty, float iNorm);

// Solve v0 and rTot from two locked-rotor points, where V_bemf is zero by
// definition so V_applied = v0 + I * rTot. Returns false when the two points
// are too close in current to separate the unknowns.
bool solveLockedRotor(float vApp1, float i1, float vApp2, float i2,
                      MotorConstants *out);
```

- [ ] **Step 4: Write the implementation**

Create `embedded/control/motor_model.cpp`:

```cpp
#include "motor_model.h"
#include <cmath>

float bemfVolts(const MotorConstants &mc, float vBus, float duty, float iNorm) {
  return vBus * duty - mc.v0 - iNorm * mc.rTot;
}

bool solveLockedRotor(float vApp1, float i1, float vApp2, float i2,
                      MotorConstants *out) {
  const float di = i2 - i1;
  if (std::fabs(di) < 0.02f) return false;   // below INA219 noise floor
  const float rTot = (vApp2 - vApp1) / di;
  if (rTot <= 0.0f) return false;
  out->rTot = rTot;
  out->v0   = vApp1 - i1 * rTot;
  return true;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 6: Commit**

```bash
git add embedded/control/motor_model.h embedded/control/motor_model.cpp embedded/control/test/
git commit -m "feat(control): affine back-EMF model and locked-rotor solve"
```

---

### Task 5: Baseline table

**Files:**
- Create: `embedded/control/baseline.h`, `embedded/control/baseline.cpp`, `embedded/control/test/test_baseline.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `test_util.h`
- Produces: `struct BaselinePoint { float duty; float iFlat; float vBemfFlat; }`, `struct Baseline { const BaselinePoint* pts; int n; }`, `float baselineCurrent(const Baseline&, float duty)`, `float baselineBemf(const Baseline&, float duty)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_baseline.cpp`:

```cpp
#include "baseline.h"
#include "test_util.h"

static const BaselinePoint kPts[] = {
  {0.20f, 0.10f, 0.40f},
  {0.40f, 0.20f, 2.40f},
  {0.60f, 0.30f, 4.40f},
};
static const Baseline kB{kPts, 3};

void testBaselineExactPoint() {
  CHECK_NEAR(baselineBemf(kB, 0.40f), 2.40f, 1e-5);
  CHECK_NEAR(baselineCurrent(kB, 0.40f), 0.20f, 1e-5);
}

void testBaselineInterpolatesBetweenPoints() {
  CHECK_NEAR(baselineBemf(kB, 0.30f), 1.40f, 1e-5);
  CHECK_NEAR(baselineCurrent(kB, 0.50f), 0.25f, 1e-5);
}

void testBaselineClampsBelowAndAboveRange() {
  CHECK_NEAR(baselineBemf(kB, 0.05f), 0.40f, 1e-5);
  CHECK_NEAR(baselineBemf(kB, 0.95f), 4.40f, 1e-5);
}
```

- [ ] **Step 2: Register and run to verify failure**

Add to `test_main.cpp` the three declarations and `RUN` lines, matching the pattern
already there.

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: baseline.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `embedded/control/baseline.h`:

```cpp
#pragma once

// One row of the flat-ground calibration, measured by drive_test `k`.
struct BaselinePoint {
  float duty;        // 0..1, commanded
  float iFlat;       // A, duty-normalized current on flat ground
  float vBemfFlat;   // V, back-EMF on flat ground - this is the speed target
};

// Points must be sorted by increasing duty. n >= 2.
struct Baseline {
  const BaselinePoint *pts;
  int n;
};

// Linear interpolation, clamped to the end points outside the measured range.
float baselineCurrent(const Baseline &b, float duty);
float baselineBemf(const Baseline &b, float duty);
```

- [ ] **Step 4: Write the implementation**

Create `embedded/control/baseline.cpp`:

```cpp
#include "baseline.h"

namespace {

float interp(const Baseline &b, float duty, bool wantBemf) {
  const BaselinePoint *p = b.pts;
  auto pick = [&](const BaselinePoint &q) {
    return wantBemf ? q.vBemfFlat : q.iFlat;
  };
  if (b.n <= 0) return 0.0f;
  if (duty <= p[0].duty)        return pick(p[0]);
  if (duty >= p[b.n - 1].duty)  return pick(p[b.n - 1]);

  for (int i = 1; i < b.n; i++) {
    if (duty <= p[i].duty) {
      const float span = p[i].duty - p[i - 1].duty;
      if (span <= 0.0f) return pick(p[i]);
      const float t = (duty - p[i - 1].duty) / span;
      return pick(p[i - 1]) + t * (pick(p[i]) - pick(p[i - 1]));
    }
  }
  return pick(p[b.n - 1]);
}

}  // namespace

float baselineCurrent(const Baseline &b, float duty) {
  return interp(b, duty, false);
}

float baselineBemf(const Baseline &b, float duty) {
  return interp(b, duty, true);
}
```

- [ ] **Step 5: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 6: Commit**

```bash
git add embedded/control/baseline.h embedded/control/baseline.cpp embedded/control/test/
git commit -m "feat(control): flat-ground baseline table with interpolation"
```

---

### Task 6: Load detector

**Files:**
- Create: `embedded/control/load_detector.h`, `embedded/control/load_detector.cpp`, `embedded/control/test/test_load_detector.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `test_util.h`
- Produces: `struct LoadConfig`, `struct LoadState`, `struct LoadReading`, `LoadReading loadUpdate(LoadState*, const LoadConfig&, float iRawL, float iRawR, float dutyL, float dutyR)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_load_detector.cpp`:

```cpp
#include "load_detector.h"
#include "test_util.h"

static LoadConfig cfg() {
  return LoadConfig{1.0f, 0.30f, 0.15f};   // alpha, asymAmps, dutyNormMin
}

void testNormalizesByDuty() {
  LoadState st{};
  // EN-pin PWM coasts the bridge, so the averaged reading is duty * real.
  LoadReading r = loadUpdate(&st, cfg(), 0.25f, 0.25f, 0.50f, 0.50f);
  CHECK(r.valid);
  CHECK_NEAR(r.iNormL, 0.50f, 1e-4);
  CHECK_NEAR(r.iNormR, 0.50f, 1e-4);
}

void testInvalidBelowDutyFloor() {
  LoadState st{};
  LoadReading r = loadUpdate(&st, cfg(), 0.02f, 0.02f, 0.10f, 0.10f);
  CHECK(!r.valid);
}

void testDetectsOneSidedLoad() {
  LoadState st{};
  LoadReading r = loadUpdate(&st, cfg(), 0.25f, 0.50f, 0.50f, 0.50f);
  CHECK(r.asymmetric);
}

void testTurnIsNotFlaggedAsymmetric() {
  LoadState st{};
  // Commanded duties differ - this is a turn, so the symmetry test must not fire.
  LoadReading r = loadUpdate(&st, cfg(), 0.15f, 0.40f, 0.30f, 0.60f);
  CHECK(!r.asymmetric);
}

void testFilterSmoothsAcrossSamples() {
  LoadState st{};
  LoadConfig c = cfg();
  c.lpfAlpha = 0.5f;
  loadUpdate(&st, c, 0.10f, 0.10f, 0.50f, 0.50f);          // iNorm 0.2
  LoadReading r = loadUpdate(&st, c, 0.30f, 0.30f, 0.50f, 0.50f);  // iNorm 0.6
  CHECK_NEAR(r.iNormL, 0.40f, 1e-4);                       // halfway
}
```

- [ ] **Step 2: Register and run to verify failure**

Add the five declarations and `RUN` lines to `test_main.cpp`.

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: load_detector.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `embedded/control/load_detector.h`:

```cpp
#pragma once

struct LoadConfig {
  float lpfAlpha;      // 0..1 per sample; 1.0 disables filtering
  float asymAmps;      // |iNormL - iNormR| above this is one-sided load
  float dutyNormMin;   // below this applied duty, normalization is invalid
};

struct LoadState {
  float iFiltL;
  float iFiltR;
  bool  primed;
};

struct LoadReading {
  float iNormL;      // filtered, duty-normalized amps
  float iNormR;
  bool  valid;       // false when duty is too low to normalize
  bool  asymmetric;  // one-sided load: a snag, not a grade - suppress boost
};

// dutyL/dutyR are the APPLIED duty magnitudes, 0..1. Boost is common-mode, so
// their ratio equals the commanded ratio; the symmetry test only fires when
// they are close, which is what keeps a turn from reading as a snag.
LoadReading loadUpdate(LoadState *st, const LoadConfig &cfg,
                       float iRawL, float iRawR, float dutyL, float dutyR);
```

- [ ] **Step 4: Write the implementation**

Create `embedded/control/load_detector.cpp`:

```cpp
#include "load_detector.h"
#include <cmath>

LoadReading loadUpdate(LoadState *st, const LoadConfig &cfg,
                       float iRawL, float iRawR, float dutyL, float dutyR) {
  LoadReading out{};

  if (dutyL < cfg.dutyNormMin || dutyR < cfg.dutyNormMin) {
    out.valid = false;
    st->primed = false;
    return out;
  }

  const float rawNormL = iRawL / dutyL;
  const float rawNormR = iRawR / dutyR;

  if (!st->primed) {
    st->iFiltL = rawNormL;
    st->iFiltR = rawNormR;
    st->primed = true;
  } else {
    st->iFiltL += cfg.lpfAlpha * (rawNormL - st->iFiltL);
    st->iFiltR += cfg.lpfAlpha * (rawNormR - st->iFiltR);
  }

  out.iNormL = st->iFiltL;
  out.iNormR = st->iFiltR;
  out.valid  = true;

  const bool dutiesMatched = std::fabs(dutyL - dutyR) < 0.05f;
  out.asymmetric = dutiesMatched &&
                   std::fabs(out.iNormL - out.iNormR) > cfg.asymAmps;
  return out;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 6: Commit**

```bash
git add embedded/control/load_detector.h embedded/control/load_detector.cpp embedded/control/test/
git commit -m "feat(control): duty-normalizing load detector with symmetry test"
```

---

### Task 7: Safety interlocks

**Files:**
- Create: `embedded/control/safety.h`, `embedded/control/safety.cpp`, `embedded/control/test/test_safety.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `test_util.h`
- Produces: `enum class Fault`, `struct SafetyConfig`, `struct SafetyState`, `struct SafetyInputs`, `float dutyMaxAbs(const SafetyConfig&, float vBus, float v0)`, `Fault safetyCheck(SafetyState*, const SafetyConfig&, const SafetyInputs&)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_safety.cpp`:

```cpp
#include "safety.h"
#include "test_util.h"

static SafetyConfig cfg() {
  SafetyConfig c{};
  c.vMotorMax = 5.0f;
  c.iCeil     = 1.8f;
  c.iStall    = 1.5f;
  c.vStall    = 0.5f;
  c.tStallMs  = 200;
  c.vMin      = 9.0f;
  return c;
}

static SafetyInputs healthy(unsigned long t) {
  SafetyInputs in{};
  in.vBus = 12.0f; in.iNormL = 0.3f; in.iNormR = 0.3f;
  in.vBemfL = 3.0f; in.vBemfR = 3.0f;
  in.overflowL = false; in.overflowR = false;
  in.tMs = t;
  return in;
}

void testDutyClampTightensAsBusRises() {
  // 5.0 / (12.0 - 1.9) = 0.495
  CHECK_NEAR(dutyMaxAbs(cfg(), 12.0f, 1.9f), 0.495f, 1e-3);
  // A 4S pack at 16.8 V must tighten the clamp, not loosen it.
  CHECK(dutyMaxAbs(cfg(), 16.8f, 1.9f) < dutyMaxAbs(cfg(), 12.0f, 1.9f));
}

void testDutyClampIsSafeWhenHeadroomVanishes() {
  CHECK_NEAR(dutyMaxAbs(cfg(), 1.9f, 1.9f), 0.0f, 1e-6);
}

void testHealthyStateHasNoFault() {
  SafetyState st{};
  CHECK(safetyCheck(&st, cfg(), healthy(0)) == Fault::None);
}

void testOverflowIsTreatedAsCeilingBreach() {
  SafetyState st{};
  SafetyInputs in = healthy(0);
  in.iNormL = 0.1f;        // wrapped counter reads LOW - must not look healthy
  in.overflowL = true;
  CHECK(safetyCheck(&st, cfg(), in) == Fault::SensorOverflow);
}

void testOverCurrentTrips() {
  SafetyState st{};
  SafetyInputs in = healthy(0);
  in.iNormR = 2.0f;
  CHECK(safetyCheck(&st, cfg(), in) == Fault::OverCurrent);
}

void testStallNeedsBothCurrentAndZeroSpeed() {
  SafetyState st{};
  SafetyInputs in = healthy(0);
  in.iNormL = 1.6f;        // high current but still moving - a climb, not a stall
  in.vBemfL = 2.0f;
  CHECK(safetyCheck(&st, cfg(), in) != Fault::Stall);
}

void testStallTripsOnlyAfterSustainedWindow() {
  SafetyState st{};
  SafetyInputs in = healthy(0);
  in.iNormL = 1.6f;
  in.vBemfL = 0.1f;
  CHECK(safetyCheck(&st, cfg(), in) != Fault::Stall);   // t=0, just started
  in.tMs = 199;
  CHECK(safetyCheck(&st, cfg(), in) != Fault::Stall);   // still inside window
  in.tMs = 201;
  CHECK(safetyCheck(&st, cfg(), in) == Fault::Stall);   // 200 ms elapsed
}

void testStallTimerResetsWhenConditionClears() {
  SafetyState st{};
  SafetyInputs in = healthy(0);
  in.iNormL = 1.6f; in.vBemfL = 0.1f;
  safetyCheck(&st, cfg(), in);
  in = healthy(100);                                     // cleared
  safetyCheck(&st, cfg(), in);
  in = healthy(150); in.iNormL = 1.6f; in.vBemfL = 0.1f; // starts over
  safetyCheck(&st, cfg(), in);
  in.tMs = 300;
  CHECK(safetyCheck(&st, cfg(), in) != Fault::Stall);
}

void testBrownoutOutranksOtherFaults() {
  SafetyState st{};
  SafetyInputs in = healthy(0);
  in.vBus = 8.5f;
  in.iNormL = 2.0f;
  CHECK(safetyCheck(&st, cfg(), in) == Fault::Brownout);
}
```

- [ ] **Step 2: Register and run to verify failure**

Add the nine declarations and `RUN` lines to `test_main.cpp`.

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: safety.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `embedded/control/safety.h`:

```cpp
#pragma once
#include <cstdint>

enum class Fault : int {
  None = 0,
  Brownout,        // V_bus below cutoff -> SAFE_HOLD
  Stall,           // high current AND near-zero speed -> SAFE_HOLD
  SensorOverflow,  // INA219 counter wrapped - treat as a ceiling breach
  OverCurrent,     // above the L298N's per-channel budget -> decay boost
};

struct SafetyConfig {
  float    vMotorMax;   // V, the duty clamp target
  float    iCeil;       // A per channel, set by the L298N not the motors
  float    iStall;      // A per channel
  float    vStall;      // V of back-EMF below which the motor is not turning
  uint32_t tStallMs;
  float    vMin;        // V, brownout cutoff
};

struct SafetyState {
  uint32_t stallSinceMs;
  bool     stallActive;
};

struct SafetyInputs {
  float    vBus;
  float    iNormL, iNormR;
  float    vBemfL, vBemfR;
  bool     overflowL, overflowR;
  uint32_t tMs;
};

// Highest duty that keeps the motor at or below vMotorMax. Recomputed every
// cycle from measured vBus so a fuller pack tightens the clamp automatically.
float dutyMaxAbs(const SafetyConfig &cfg, float vBus, float v0);

// Returns the most severe active fault. Independent of controller state by
// design: a corrupted PI must not be able to command past these.
Fault safetyCheck(SafetyState *st, const SafetyConfig &cfg,
                  const SafetyInputs &in);
```

- [ ] **Step 4: Write the implementation**

Create `embedded/control/safety.cpp`:

```cpp
#include "safety.h"

float dutyMaxAbs(const SafetyConfig &cfg, float vBus, float v0) {
  const float head = vBus - v0;
  if (head <= 0.1f) return 0.0f;
  float d = cfg.vMotorMax / head;
  if (d > 1.0f) d = 1.0f;
  if (d < 0.0f) d = 0.0f;
  return d;
}

Fault safetyCheck(SafetyState *st, const SafetyConfig &cfg,
                  const SafetyInputs &in) {
  if (in.vBus < cfg.vMin) return Fault::Brownout;

  const bool stalledL = in.iNormL > cfg.iStall && in.vBemfL < cfg.vStall;
  const bool stalledR = in.iNormR > cfg.iStall && in.vBemfR < cfg.vStall;
  if (stalledL || stalledR) {
    if (!st->stallActive) {
      st->stallActive  = true;
      st->stallSinceMs = in.tMs;
    } else if (in.tMs - st->stallSinceMs >= cfg.tStallMs) {
      return Fault::Stall;
    }
  } else {
    st->stallActive = false;
  }

  // A wrapped counter reads LOW, so an unchecked overflow looks like reduced
  // load and the loop would boost harder into a real overcurrent.
  if (in.overflowL || in.overflowR) return Fault::SensorOverflow;

  if (in.iNormL > cfg.iCeil || in.iNormR > cfg.iCeil) return Fault::OverCurrent;

  return Fault::None;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 6: Commit**

```bash
git add embedded/control/safety.h embedded/control/safety.cpp embedded/control/test/
git commit -m "feat(control): duty clamp, current ceiling, overflow, stall and brownout gates"
```

---

### Task 8: The PI controller

**Files:**
- Create: `embedded/control/incline_comp.h`, `embedded/control/incline_comp.cpp`, `embedded/control/test/test_incline_comp.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `test_util.h`
- Produces: `struct PiConfig`, `struct PiState`, `struct PiInputs`, `float inclineUpdate(PiState*, const PiConfig&, const PiInputs&)`, `void inclineReset(PiState*)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_incline_comp.cpp`:

```cpp
#include "incline_comp.h"
#include "test_util.h"

static PiConfig cfg() {
  PiConfig c{};
  c.kp = 0.15f; c.ki = 0.4f; c.boostMax = 0.50f;
  c.slewPerSec = 2.0f; c.dCmdMin = 0.20f;
  return c;
}

static PiInputs in(float err, unsigned long t, bool suppress = false) {
  PiInputs i{};
  i.errVolts = err; i.dCmd = 0.4f; i.suppress = suppress; i.tMs = t;
  return i;
}

void testNoBoostWithoutError() {
  PiState st{}; inclineReset(&st);
  CHECK_NEAR(inclineUpdate(&st, cfg(), in(0.0f, 20)), 0.0f, 1e-6);
}

void testBoostRisesWithSustainedError() {
  PiState st{}; inclineReset(&st);
  float last = 0.0f;
  for (unsigned long t = 20; t <= 400; t += 20) {
    const float b = inclineUpdate(&st, cfg(), in(1.0f, t));
    CHECK(b >= last - 1e-6f);
    last = b;
  }
  CHECK(last > 0.1f);
}

void testBoostNeverGoesNegative() {
  PiState st{}; inclineReset(&st);
  // Negative error means running FASTER than baseline - downhill. The loop
  // holds speed, it never cuts below what the operator commanded.
  for (unsigned long t = 20; t <= 400; t += 20) {
    CHECK(inclineUpdate(&st, cfg(), in(-2.0f, t)) >= 0.0f);
  }
}

void testBoostSaturatesAtMax() {
  PiState st{}; inclineReset(&st);
  for (unsigned long t = 20; t <= 4000; t += 20) {
    inclineUpdate(&st, cfg(), in(5.0f, t));
  }
  CHECK_NEAR(st.boost, 0.50f, 1e-4);
}

void testAntiWindupPreventsOvershootOnCrest() {
  PiState st{}; inclineReset(&st);
  unsigned long t = 20;
  for (; t <= 6000; t += 20) inclineUpdate(&st, cfg(), in(5.0f, t));
  CHECK_NEAR(st.boost, 0.50f, 1e-4);
  // Grade clears. Without anti-windup the integral would hold boost high for
  // seconds; it must instead fall away promptly.
  for (unsigned long e = t; e < t + 1000; e += 20) {
    inclineUpdate(&st, cfg(), in(-0.5f, e));
  }
  CHECK(st.boost < 0.10f);
}

void testSlewLimitBoundsRateOfChange() {
  PiState st{}; inclineReset(&st);
  // One 20 ms step at 2.0/s can move boost by at most 0.04.
  CHECK(inclineUpdate(&st, cfg(), in(10.0f, 20)) <= 0.04f + 1e-6f);
}

void testSuppressDecaysBoostToZero() {
  PiState st{}; inclineReset(&st);
  unsigned long t = 20;
  for (; t <= 2000; t += 20) inclineUpdate(&st, cfg(), in(5.0f, t));
  CHECK(st.boost > 0.2f);
  for (unsigned long e = t; e < t + 2000; e += 20) {
    inclineUpdate(&st, cfg(), in(5.0f, e, true));
  }
  CHECK_NEAR(st.boost, 0.0f, 1e-4);
}

void testLoopIdlesBelowCommandFloor() {
  PiState st{}; inclineReset(&st);
  PiInputs i = in(5.0f, 20);
  i.dCmd = 0.10f;                       // below dCmdMin
  CHECK_NEAR(inclineUpdate(&st, cfg(), i), 0.0f, 1e-6);
}

void testResetClearsIntegral() {
  PiState st{}; inclineReset(&st);
  for (unsigned long t = 20; t <= 1000; t += 20) inclineUpdate(&st, cfg(), in(5.0f, t));
  inclineReset(&st);
  CHECK_NEAR(st.boost, 0.0f, 1e-6);
  CHECK_NEAR(st.integral, 0.0f, 1e-6);
}
```

- [ ] **Step 2: Register and run to verify failure**

Add the nine declarations and `RUN` lines to `test_main.cpp`.

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: incline_comp.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `embedded/control/incline_comp.h`:

```cpp
#pragma once
#include <cstdint>

struct PiConfig {
  float kp;          // boost per volt of back-EMF error
  float ki;          // boost per volt-second
  float boostMax;    // hard upper bound on the output
  float slewPerSec;  // maximum rate of change of boost
  float dCmdMin;     // below this commanded duty the loop idles
};

struct PiState {
  float    integral;
  float    boost;
  uint32_t lastMs;
  bool     started;
};

struct PiInputs {
  float    errVolts;   // target - measured; positive means lagging
  float    dCmd;       // commanded duty magnitude, 0..1
  bool     suppress;   // asymmetry, invalid reading, or an active fault
  uint32_t tMs;
};

// Returns boost in 0..boostMax. Never negative: this loop holds speed, it does
// not cut below the operator's command.
float inclineUpdate(PiState *st, const PiConfig &cfg, const PiInputs &in);

// Clears integral and output. Called on SAFE_HOLD entry and every mode exit.
void inclineReset(PiState *st);
```

- [ ] **Step 4: Write the implementation**

Create `embedded/control/incline_comp.cpp`:

```cpp
#include "incline_comp.h"

void inclineReset(PiState *st) {
  st->integral = 0.0f;
  st->boost    = 0.0f;
  st->lastMs   = 0;
  st->started  = false;
}

namespace {

float slewToward(float current, float target, float maxStep) {
  const float delta = target - current;
  if (delta >  maxStep) return current + maxStep;
  if (delta < -maxStep) return current - maxStep;
  return target;
}

}  // namespace

float inclineUpdate(PiState *st, const PiConfig &cfg, const PiInputs &in) {
  float dt = 0.0f;
  if (st->started) dt = (in.tMs - st->lastMs) / 1000.0f;
  st->lastMs  = in.tMs;
  st->started = true;
  if (dt <= 0.0f || dt > 0.5f) dt = 0.0f;   // first call, or a stalled loop

  const float maxStep = cfg.slewPerSec * dt;

  if (in.suppress || in.dCmd < cfg.dCmdMin) {
    st->integral = 0.0f;
    st->boost    = slewToward(st->boost, 0.0f, maxStep);
    return st->boost;
  }

  const float candidate = cfg.kp * in.errVolts + cfg.ki * st->integral;

  // Conditional integration: accumulate only when doing so would not push an
  // already-saturated output further into its limit. Without this, cresting a
  // ramp leaves the integral high and the robot lunges.
  const bool atUpper = candidate >= cfg.boostMax && in.errVolts > 0.0f;
  const bool atLower = candidate <= 0.0f && in.errVolts < 0.0f;
  if (!atUpper && !atLower) st->integral += in.errVolts * dt;

  float target = cfg.kp * in.errVolts + cfg.ki * st->integral;
  if (target > cfg.boostMax) target = cfg.boostMax;
  if (target < 0.0f)         target = 0.0f;

  st->boost = slewToward(st->boost, target, maxStep);
  if (st->boost < 0.0f) st->boost = 0.0f;
  return st->boost;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 6: Commit**

```bash
git add embedded/control/incline_comp.h embedded/control/incline_comp.cpp embedded/control/test/
git commit -m "feat(control): back-EMF speed-hold PI with anti-windup and slew limit"
```

---

### Task 9: Drive core orchestrator

**Files:**
- Create: `embedded/control/drive_core.h`, `embedded/control/drive_core.cpp`, `embedded/control/test/test_drive_core.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `MotorConstants`/`bemfVolts` (Task 4), `Baseline`/`baselineBemf` (Task 5), `LoadConfig`/`loadUpdate` (Task 6), `SafetyConfig`/`dutyMaxAbs`/`safetyCheck`/`Fault` (Task 7), `PiConfig`/`inclineUpdate`/`inclineReset` (Task 8)
- Produces: `struct DriveConfig`, `struct DriveState`, `struct DriveInputs`, `struct DriveOutputs`, `DriveOutputs driveUpdate(DriveState*, const DriveConfig&, const DriveInputs&)`, `void driveReset(DriveState*)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_drive_core.cpp`:

```cpp
#include "drive_core.h"
#include "test_util.h"
#include <cmath>

// Self-consistent with motorL/motorR below at vBus 12.0:
//   vBemf = 12*duty - v0(1.9) - iFlat*rTot(2.5)
// If these drift out of agreement, testFlatGroundProducesNoBoost sees a
// standing error and boosts on flat ground - which is the exact pathology the
// design's section 9 warns about, showing up as a test-data bug.
static const BaselinePoint kPts[] = {
  {0.20f, 0.18f, 0.05f},
  {0.35f, 0.24f, 1.70f},
  {0.50f, 0.30f, 3.35f},
};

static DriveConfig cfg() {
  DriveConfig c{};
  c.motorL = {1.9f, 2.5f};
  c.motorR = {1.9f, 2.5f};
  c.baseline = {kPts, 3};
  c.load = {1.0f, 0.30f, 0.15f};
  c.safety = {5.0f, 1.8f, 1.5f, 0.5f, 200, 9.0f};
  c.pi = {0.15f, 0.4f, 0.50f, 2.0f, 0.20f};
  return c;
}

static DriveInputs flat(unsigned long t) {
  DriveInputs in{};
  in.cmdL = 0.35f; in.cmdR = 0.35f;
  in.iL = 0.084f; in.iR = 0.084f;      // 0.24 A normalized at 0.35 duty
  in.vBus = 12.0f; in.tMs = t;
  return in;
}

void testFlatGroundProducesNoBoost() {
  DriveState st{}; driveReset(&st);
  DriveOutputs out{};
  for (unsigned long t = 20; t <= 1000; t += 20) out = driveUpdate(&st, cfg(), flat(t));
  CHECK_NEAR(out.boostPct, 0.0f, 0.02f);
  CHECK(!out.compensating);
  CHECK(out.fault == Fault::None);
}

void testLoadRiseProducesBoost() {
  DriveState st{}; driveReset(&st);
  DriveOutputs out{};
  for (unsigned long t = 20; t <= 1000; t += 20) {
    DriveInputs in = flat(t);
    in.iL = 0.21f; in.iR = 0.21f;      // 0.6 A normalized: heavy load
    out = driveUpdate(&st, cfg(), in);
  }
  CHECK(out.boostPct > 0.05f);
  CHECK(out.compensating);
}

void testDutyNeverExceedsTheClamp() {
  DriveState st{}; driveReset(&st);
  const float cap = dutyMaxAbs(cfg().safety, 12.0f, 1.9f);
  for (unsigned long t = 20; t <= 4000; t += 20) {
    DriveInputs in = flat(t);
    in.cmdL = 1.0f; in.cmdR = 1.0f;    // operator at full stick
    in.iL = 0.6f; in.iR = 0.6f;        // heavy load, boost commanded
    DriveOutputs out = driveUpdate(&st, cfg(), in);
    CHECK(std::fabs(out.dutyL) <= cap + 1e-4f);
    CHECK(std::fabs(out.dutyR) <= cap + 1e-4f);
  }
}

void testAsymmetrySuppressesBoost() {
  DriveState st{}; driveReset(&st);
  DriveOutputs out{};
  for (unsigned long t = 20; t <= 1000; t += 20) {
    DriveInputs in = flat(t);
    in.iL = 0.07f; in.iR = 0.28f;      // one side dragging: a snag
    out = driveUpdate(&st, cfg(), in);
  }
  CHECK_NEAR(out.boostPct, 0.0f, 1e-3);
}

void testStallZeroesOutput() {
  DriveState st{}; driveReset(&st);
  DriveOutputs out{};
  for (unsigned long t = 20; t <= 1000; t += 20) {
    DriveInputs in = flat(t);
    in.iL = 0.6f; in.iR = 0.6f;        // 1.7 A normalized, and not moving
    out = driveUpdate(&st, cfg(), in);
  }
  CHECK(out.fault == Fault::Stall);
  CHECK_NEAR(out.dutyL, 0.0f, 1e-6);
  CHECK_NEAR(out.dutyR, 0.0f, 1e-6);
}

void testReverseCommandKeepsItsSign() {
  DriveState st{}; driveReset(&st);
  DriveInputs in = flat(20);
  in.cmdL = -0.35f; in.cmdR = -0.35f;
  DriveOutputs out = driveUpdate(&st, cfg(), in);
  CHECK(out.dutyL < 0.0f);
  CHECK(out.dutyR < 0.0f);
}
```

- [ ] **Step 2: Register and run to verify failure**

Add the six declarations and `RUN` lines to `test_main.cpp`.

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: drive_core.h: No such file or directory`

- [ ] **Step 3: Write the header**

Create `embedded/control/drive_core.h`:

```cpp
#pragma once
#include <cstdint>
#include "baseline.h"
#include "incline_comp.h"
#include "load_detector.h"
#include "motor_model.h"
#include "safety.h"

struct DriveConfig {
  MotorConstants motorL, motorR;
  Baseline       baseline;
  LoadConfig     load;
  SafetyConfig   safety;
  PiConfig       pi;
};

struct DriveState {
  LoadState  load;
  SafetyState safety;
  PiState    pi;
  float      lastDutyL, lastDutyR;   // applied magnitudes, for normalization
};

struct DriveInputs {
  float    cmdL, cmdR;        // operator command, -1..1
  float    iL, iR;            // A, raw from the INA219s
  float    vBus;              // V, from the divider
  bool     overflowL, overflowR;
  uint32_t tMs;
};

struct DriveOutputs {
  float dutyL, dutyR;         // signed, what reaches the bridge
  float boostPct;             // 0..boostMax, for telemetry
  float vBemfL, vBemfR;
  bool  compensating;         // GUI indication
  Fault fault;
};

DriveOutputs driveUpdate(DriveState *st, const DriveConfig &cfg,
                         const DriveInputs &in);
void driveReset(DriveState *st);
```

- [ ] **Step 4: Write the implementation**

Create `embedded/control/drive_core.cpp`:

```cpp
#include "drive_core.h"
#include <cmath>

void driveReset(DriveState *st) {
  st->load = LoadState{};
  st->safety = SafetyState{};
  inclineReset(&st->pi);
  st->lastDutyL = 0.0f;
  st->lastDutyR = 0.0f;
}

namespace {

float magClamp(float v) {
  const float m = std::fabs(v);
  return m > 1.0f ? 1.0f : m;
}

}  // namespace

DriveOutputs driveUpdate(DriveState *st, const DriveConfig &cfg,
                         const DriveInputs &in) {
  DriveOutputs out{};

  const float dCmdL = magClamp(in.cmdL);
  const float dCmdR = magClamp(in.cmdR);
  const float cap   = dutyMaxAbs(cfg.safety, in.vBus, cfg.motorL.v0);

  // Normalization uses the duty actually applied last cycle, not the command.
  const LoadReading load = loadUpdate(&st->load, cfg.load, in.iL, in.iR,
                                      st->lastDutyL, st->lastDutyR);

  out.vBemfL = bemfVolts(cfg.motorL, in.vBus, st->lastDutyL, load.iNormL);
  out.vBemfR = bemfVolts(cfg.motorR, in.vBus, st->lastDutyR, load.iNormR);

  SafetyInputs si{};
  si.vBus = in.vBus;
  si.iNormL = load.iNormL; si.iNormR = load.iNormR;
  si.vBemfL = out.vBemfL;  si.vBemfR = out.vBemfR;
  si.overflowL = in.overflowL; si.overflowR = in.overflowR;
  si.tMs = in.tMs;
  out.fault = safetyCheck(&st->safety, cfg.safety, si);

  const bool halt = out.fault == Fault::Stall || out.fault == Fault::Brownout;

  // Common-mode boost: one scalar for both sides, so the commanded turn ratio
  // survives untouched. The left/right difference only ever suppresses.
  const float dCmdMean = 0.5f * (dCmdL + dCmdR);
  const float target   = baselineBemf(cfg.baseline, dCmdMean);
  const float measured = 0.5f * (out.vBemfL + out.vBemfR);

  PiInputs pi{};
  pi.errVolts = target - measured;
  pi.dCmd     = dCmdMean;
  pi.suppress = halt || !load.valid || load.asymmetric ||
                out.fault != Fault::None;
  pi.tMs      = in.tMs;
  out.boostPct = inclineUpdate(&st->pi, cfg.pi, pi);

  float magL = dCmdL * (1.0f + out.boostPct);
  float magR = dCmdR * (1.0f + out.boostPct);
  if (magL > cap) magL = cap;
  if (magR > cap) magR = cap;
  if (halt) { magL = 0.0f; magR = 0.0f; inclineReset(&st->pi); out.boostPct = 0.0f; }

  st->lastDutyL = magL;
  st->lastDutyR = magR;

  out.dutyL = in.cmdL < 0.0f ? -magL : magL;
  out.dutyR = in.cmdR < 0.0f ? -magR : magR;
  out.compensating = out.boostPct > 0.02f;
  return out;
}
```

- [ ] **Step 5: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 6: Commit**

```bash
git add embedded/control/drive_core.h embedded/control/drive_core.cpp embedded/control/test/
git commit -m "feat(control): drive core orchestrating estimation, safety and boost"
```

---

### Task 10: Simulated HAL

**Files:**
- Create: `embedded/hal/hal.h`, `embedded/hal/hal_sim.h`, `embedded/hal/hal_sim.cpp`, `embedded/control/test/test_hal_sim.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: nothing from earlier tasks
- Produces: `struct HalCurrents`, `class Hal`, `class SimHal` with `setGrade(float)`, `lockWheel(int side, bool)`, `setBusVoltage(float)`, `step(float dt)`, `omega(int side)`

- [ ] **Step 1: Write the failing test**

Create `embedded/control/test/test_hal_sim.cpp`:

```cpp
#include "hal_sim.h"
#include "test_util.h"

void testSimSpinsUpUnderDuty() {
  SimHal hal;
  hal.setDuty(0.4f, 0.4f);
  for (int i = 0; i < 500; i++) hal.step(0.002f);   // 1 s
  CHECK(hal.omega(0) > 100.0f);
}

void testSimCurrentRisesWithGrade() {
  SimHal flatHal, hillHal;
  flatHal.setDuty(0.4f, 0.4f);
  hillHal.setDuty(0.4f, 0.4f);
  hillHal.setGrade(0.15f);
  for (int i = 0; i < 500; i++) { flatHal.step(0.002f); hillHal.step(0.002f); }
  CHECK(hillHal.readCurrents().iL > flatHal.readCurrents().iL);
}

void testSimSpeedDropsOnGrade() {
  SimHal flatHal, hillHal;
  flatHal.setDuty(0.4f, 0.4f);
  hillHal.setDuty(0.4f, 0.4f);
  hillHal.setGrade(0.15f);
  for (int i = 0; i < 500; i++) { flatHal.step(0.002f); hillHal.step(0.002f); }
  CHECK(hillHal.omega(0) < flatHal.omega(0));
}

void testLockedWheelStopsAndDrawsCurrent() {
  SimHal hal;
  hal.setDuty(0.4f, 0.4f);
  for (int i = 0; i < 500; i++) hal.step(0.002f);
  hal.lockWheel(0, true);
  for (int i = 0; i < 250; i++) hal.step(0.002f);
  CHECK_NEAR(hal.omega(0), 0.0f, 1.0f);
  CHECK(hal.readCurrents().iL > hal.readCurrents().iR);
}

void testOverflowFlagsAboveRange() {
  SimHal hal;
  hal.setBusVoltage(12.0f);
  hal.setDuty(1.0f, 1.0f);
  hal.lockWheel(0, true);
  hal.lockWheel(1, true);
  for (int i = 0; i < 100; i++) hal.step(0.002f);
  CHECK(hal.readCurrents().overflowL);
}
```

- [ ] **Step 2: Register and run to verify failure**

Add the five declarations and `RUN` lines to `test_main.cpp`.

Run: `cd embedded && make test`
Expected: FAIL — `fatal error: hal_sim.h: No such file or directory`

- [ ] **Step 3: Write the HAL interface**

Create `embedded/hal/hal.h`:

```cpp
#pragma once
#include <cstdint>

struct HalCurrents {
  float iL, iR;              // A, raw (duty-averaged) as the INA219 reports
  bool  overflowL, overflowR;
};

// Only the HAL differs between the native and ESP32 builds. The control core
// never includes this header - it takes plain structs.
class Hal {
 public:
  virtual ~Hal() {}
  virtual HalCurrents readCurrents() = 0;
  virtual float       readBusVoltage() = 0;
  virtual void        setDuty(float dutyL, float dutyR) = 0;   // -1..1
  virtual uint32_t    millisNow() = 0;
};
```

- [ ] **Step 4: Write the simulator header**

Create `embedded/hal/hal_sim.h`:

```cpp
#pragma once
#include "hal.h"

// Two paralleled FAM1029 motors per channel driving one side, with a 48:1
// gearbox and a scriptable grade. Constants are estimates from the datasheet
// until Task 2's measurements replace them.
class SimHal : public Hal {
 public:
  HalCurrents readCurrents() override;
  float       readBusVoltage() override;
  void        setDuty(float dutyL, float dutyR) override;
  uint32_t    millisNow() override;

  void  step(float dt);                  // advance the model by dt seconds
  void  setGrade(float g);               // 0.15 = a 15% slope
  void  lockWheel(int side, bool locked);
  void  setBusVoltage(float v);
  void  setNoiseAmps(float a);           // PWM-correlated noise amplitude
  float omega(int side) const;           // rad/s at the motor shaft

 private:
  float channelCurrent(int side) const;

  float vBus_    = 12.0f;
  float dutyL_   = 0.0f;
  float dutyR_   = 0.0f;
  float omega_[2]{0.0f, 0.0f};
  bool  locked_[2]{false, false};
  float grade_   = 0.0f;
  float noise_   = 0.0f;
  uint32_t tMs_  = 0;
  int   noiseTick_ = 0;
};
```

- [ ] **Step 5: Write the simulator**

Create `embedded/hal/hal_sim.cpp`:

```cpp
#include "hal_sim.h"
#include <cmath>

namespace {
// Per-channel constants: two motors in parallel, so resistance halves and
// current doubles relative to one motor. Replace with Task 2 measurements.
constexpr float kR      = 2.5f;      // ohm, channel
constexpr float kKe     = 0.004975f; // V*s/rad at the motor shaft
constexpr float kKt     = 0.004975f; // N*m/A
constexpr float kJ      = 4.0e-6f;   // kg*m^2, both motors motor-referred
constexpr float kB      = 2.0e-8f;   // N*m*s/rad viscous
constexpr float kV0     = 1.9f;      // V, bridge offset drop
constexpr float kFric   = 6.0e-5f;   // N*m constant friction
constexpr float kGradeK = 2.6e-3f;   // N*m per unit grade, motor-referred
constexpr float kOverflowAmps = 3.2f;
}  // namespace

void SimHal::setDuty(float dutyL, float dutyR) {
  dutyL_ = std::fabs(dutyL);
  dutyR_ = std::fabs(dutyR);
}

void SimHal::setGrade(float g)                 { grade_ = g; }
void SimHal::setBusVoltage(float v)            { vBus_ = v; }
void SimHal::setNoiseAmps(float a)             { noise_ = a; }
void SimHal::lockWheel(int side, bool locked)  { locked_[side] = locked; }
float SimHal::omega(int side) const            { return omega_[side]; }
float SimHal::readBusVoltage()                 { return vBus_; }
uint32_t SimHal::millisNow()                   { return tMs_; }

float SimHal::channelCurrent(int side) const {
  const float duty = side == 0 ? dutyL_ : dutyR_;
  const float applied = vBus_ * duty - kV0;
  if (applied <= 0.0f) return 0.0f;
  const float i = (applied - kKe * omega_[side]) / kR;
  return i > 0.0f ? i : 0.0f;
}

void SimHal::step(float dt) {
  for (int s = 0; s < 2; s++) {
    if (locked_[s]) { omega_[s] = 0.0f; continue; }
    const float i     = channelCurrent(s);
    const float load  = kFric + kGradeK * grade_;
    const float accel = (kKt * i - load - kB * omega_[s]) / kJ;
    omega_[s] += accel * dt;
    if (omega_[s] < 0.0f) omega_[s] = 0.0f;
  }
  tMs_ += (uint32_t)(dt * 1000.0f + 0.5f);
  noiseTick_++;
}

HalCurrents SimHal::readCurrents() {
  HalCurrents c{};
  // EN-pin PWM coasts the bridge during off-time, so the sensor sees the
  // duty-averaged value. The control core divides this back out.
  const float wobble = noise_ * ((noiseTick_ % 2) ? 1.0f : -1.0f);
  const float rawL = channelCurrent(0) * dutyL_ + wobble;
  const float rawR = channelCurrent(1) * dutyR_ - wobble;
  c.overflowL = channelCurrent(0) > kOverflowAmps;
  c.overflowR = channelCurrent(1) > kOverflowAmps;
  // A wrapped counter reads low - reproduce that, so the core's overflow
  // handling is exercised rather than bypassed.
  c.iL = c.overflowL ? rawL - kOverflowAmps * dutyL_ : rawL;
  c.iR = c.overflowR ? rawR - kOverflowAmps * dutyR_ : rawR;
  return c;
}
```

- [ ] **Step 6: Run the tests**

Run: `cd embedded && make test`
Expected: PASS, `0 failures`

- [ ] **Step 7: Commit**

```bash
git add embedded/hal/ embedded/control/test/
git commit -m "feat(hal): native motor, gearbox and grade simulator"
```

---

### Task 11: The eight scenario tests

This is the gate named in the design (§8) and in NFR-5: every scenario passes here
before the loop drives real motors in Task 13.

**Files:**
- Create: `embedded/control/test/test_scenarios.cpp`
- Modify: `embedded/control/test/test_main.cpp`

**Interfaces:**
- Consumes: `driveUpdate`/`driveReset` (Task 9), `SimHal` (Task 10)
- Produces: the passing gate for Task 13

- [ ] **Step 1: Write the scenario harness and all eight tests**

Create `embedded/control/test/test_scenarios.cpp`:

```cpp
#include "drive_core.h"
#include "hal_sim.h"
#include "test_util.h"
#include <cmath>

namespace {

// Generated by the dump step below - do not hand-write these. The scenarios
// only mean anything if the baseline is what this plant actually produces on
// flat ground.
const BaselinePoint kPts[] = {
  {0.20f, 0.130f, 0.435f},
  {0.30f, 0.146f, 1.535f},
  {0.40f, 0.163f, 2.634f},
  {0.50f, 0.179f, 3.733f},
};

DriveConfig cfg() {
  DriveConfig c{};
  c.motorL = {1.9f, 2.5f};
  c.motorR = {1.9f, 2.5f};
  c.baseline = {kPts, 4};
  c.load = {0.3f, 0.30f, 0.15f};
  c.safety = {5.0f, 1.8f, 1.5f, 0.5f, 200, 9.0f};
  c.pi = {0.15f, 0.4f, 0.50f, 2.0f, 0.20f};
  return c;
}

// Runs the closed loop against the simulator for the given wall time.
struct RunResult { DriveOutputs last; float peakBoost; uint32_t faultAtMs; };

RunResult run(SimHal *hal, DriveState *st, float cmd, float seconds,
              uint32_t *tMs) {
  RunResult r{};
  r.faultAtMs = 0;
  const float dt = 0.01f;                 // 100 Hz control rate
  const int steps = (int)(seconds / dt);
  for (int k = 0; k < steps; k++) {
    for (int i = 0; i < 5; i++) hal->step(dt / 5.0f);   // finer plant integration
    const HalCurrents hc = hal->readCurrents();
    DriveInputs in{};
    in.cmdL = cmd; in.cmdR = cmd;
    in.iL = hc.iL; in.iR = hc.iR;
    in.overflowL = hc.overflowL; in.overflowR = hc.overflowR;
    in.vBus = hal->readBusVoltage();
    *tMs += (uint32_t)(dt * 1000.0f);
    in.tMs = *tMs;
    r.last = driveUpdate(st, cfg(), in);
    hal->setDuty(r.last.dutyL, r.last.dutyR);
    if (r.last.boostPct > r.peakBoost) r.peakBoost = r.last.boostPct;
    if (r.last.fault != Fault::None && r.faultAtMs == 0) r.faultAtMs = *tMs;
  }
  return r;
}

}  // namespace

// 1 - flat ground must not produce a false positive
void testScenarioFlatNoBoost() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  hal.setNoiseAmps(0.01f);
  RunResult r = run(&hal, &st, 0.40f, 3.0f, &t);
  CHECK(r.peakBoost < 0.05f);
  CHECK(!r.last.compensating);
}

// 2 - a step grade must be answered within the 500 ms budget
void testScenarioStepGradeRespondsInBudget() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  run(&hal, &st, 0.40f, 3.0f, &t);
  const uint32_t gradeAt = t;
  hal.setGrade(0.15f);
  RunResult r = run(&hal, &st, 0.40f, 0.5f, &t);
  CHECK(r.last.boostPct > 0.03f);
  CHECK(t - gradeAt <= 500);
}

// 3 - cresting must not overshoot; this is the anti-windup test
void testScenarioCrestDecaysWithoutOvershoot() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  run(&hal, &st, 0.40f, 2.0f, &t);
  hal.setGrade(0.15f);
  run(&hal, &st, 3.0f, 3.0f, &t);
  hal.setGrade(0.0f);
  RunResult r = run(&hal, &st, 0.40f, 2.0f, &t);
  CHECK(r.last.boostPct < 0.05f);
}

// 4 - a locked wheel must fault within 200 ms and zero the output
void testScenarioStallFaultsAndZeroes() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  run(&hal, &st, 0.40f, 2.0f, &t);
  const uint32_t lockAt = t;
  hal.lockWheel(0, true);
  hal.lockWheel(1, true);
  RunResult r = run(&hal, &st, 0.40f, 1.0f, &t);
  CHECK(r.faultAtMs != 0);
  CHECK(r.faultAtMs - lockAt <= 400);
  CHECK_NEAR(r.last.dutyL, 0.0f, 1e-6);
}

// 5 - a one-sided snag is not a grade
void testScenarioOneSidedSnagSuppressesBoost() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  run(&hal, &st, 0.40f, 2.0f, &t);
  hal.lockWheel(0, true);
  RunResult r = run(&hal, &st, 0.40f, 0.3f, &t);
  CHECK(r.last.boostPct < 0.05f);
}

// 6 - a sagging pack means SAFE_HOLD
void testScenarioBrownoutFaults() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  run(&hal, &st, 0.40f, 1.0f, &t);
  hal.setBusVoltage(8.5f);
  RunResult r = run(&hal, &st, 0.40f, 0.5f, &t);
  CHECK(r.last.fault == Fault::Brownout);
  CHECK_NEAR(r.last.dutyL, 0.0f, 1e-6);
}

// 7 - the regression test for decision D5: firmware is the only protection
void testScenarioDutyNeverExceedsClamp() {
  SimHal hal; DriveState st{}; driveReset(&st); uint32_t t = 0;
  hal.setBusVoltage(12.6f);
  const float cap = dutyMaxAbs(cfg().safety, 12.6f, 1.9f);
  const float dt = 0.01f;
  for (int k = 0; k < 600; k++) {
    hal.step(dt);
    const HalCurrents hc = hal.readCurrents();
    DriveInputs in{};
    in.cmdL = 1.0f; in.cmdR = 1.0f;           // full stick throughout
    in.iL = hc.iL; in.iR = hc.iR;
    in.vBus = hal.readBusVoltage();
    t += 10; in.tMs = t;
    DriveOutputs out = driveUpdate(&st, cfg(), in);
    CHECK(std::fabs(out.dutyL) <= cap + 1e-4f);
    hal.setDuty(out.dutyL, out.dutyR);
  }
}

// 8 - a wrapped counter must never read as reduced load
void testScenarioOverflowIsNotReadAsLowCurrent() {
  DriveState st{}; driveReset(&st);
  DriveInputs in{};
  in.cmdL = 0.4f; in.cmdR = 0.4f;
  in.iL = 0.04f; in.iR = 0.04f;               // wrapped: looks tiny
  in.overflowL = true;
  in.vBus = 12.0f; in.tMs = 10;
  DriveOutputs out = driveUpdate(&st, cfg(), in);
  CHECK(out.fault == Fault::SensorOverflow);
  CHECK_NEAR(out.boostPct, 0.0f, 1e-6);
}
```

- [ ] **Step 2: Generate the scenario baseline from the simulator**

The `kPts` above are placeholders. A baseline that disagrees with the plant puts a
standing error into every scenario, so derive it rather than guessing.

Add this temporary main to `embedded/tools/dump_baseline.cpp`:

```cpp
#include "hal_sim.h"
#include <cstdio>

int main() {
  for (int k = 0; k < 4; k++) {
    const float d = 0.20f + 0.10f * k;
    SimHal hal;
    hal.setDuty(d, d);
    for (int i = 0; i < 2000; i++) hal.step(0.001f);   // 2 s to settle
    const HalCurrents c = hal.readCurrents();
    const float iNorm = c.iL / d;
    const float bemf  = 12.0f * d - 1.9f - iNorm * 2.5f;
    std::printf("  {%.2ff, %.3ff, %.3ff},\n", d, iNorm, bemf);
  }
}
```

Run it and paste the four rows over `kPts`:

```bash
cd embedded && g++ -std=c++17 -Ihal hal/hal_sim.cpp tools/dump_baseline.cpp -o build/dump && ./build/dump
```

- [ ] **Step 3: Register and run to verify failure**

Add the eight declarations and `RUN` lines to `test_main.cpp`.

Run: `cd embedded && make test`
Expected: FAIL — the scenarios exercise real closed-loop behavior, so expect
several to fail on the first run.

- [ ] **Step 4: Tune `Kp`, `Ki` and the sim constants until all eight pass**

Only these may be adjusted:
- `c.pi.kp`, `c.pi.ki` in `cfg()`, then mirrored into `calibration.h`
- the `kGradeK`, `kJ`, `kFric` constants in `hal_sim.cpp`

Do **not** relax a threshold in a test to make it pass. The 500 ms and 200 ms
budgets come from `spec.md` §8; the duty cap comes from the motor rating. If a
budget cannot be met, stop and report it — that is a real finding about the design,
not a test to loosen.

- [ ] **Step 5: Confirm the full suite is green**

Run: `cd embedded && make test`
Expected: `0 failures`, exit 0.

- [ ] **Step 6: Commit**

```bash
git add embedded/control/test/ embedded/hal/hal_sim.cpp embedded/control/calibration.h
git commit -m "test(control): eight closed-loop scenarios pass against the simulator"
```

---

### Task 12: Bus voltage divider and the ESP32 HAL

**Files:**
- Create: `embedded/hal/hal_esp32.h`, `embedded/hal/hal_esp32.cpp`
- Modify: `embedded/drive_test/drive_test.ino` (replace `vBusAssumed` with a real reading)

**Interfaces:**
- Consumes: `Hal` (Task 10), `HalCurrents` (Task 10)
- Produces: `class Esp32Hal` implementing `Hal`, used by Task 13

- [ ] **Step 1: Build the divider**

Use 100 kΩ (high side, to battery +) and 22 kΩ (low side, to GND), with the tap to
`GPIO34`. `GPIO34` is input-only, which suits a sense line.

At 12.6 V the tap sits at `12.6 * 22 / 122 = 2.27 V`, safely under the 3.3 V ADC
limit. At 16.8 V it reads `3.03 V` — still inside range, so a 4S pack needs no
change. Add a 100 nF capacitor from the tap to GND to quiet the PWM pickup.

The divider's ground must be the same ground as the ESP32.

- [ ] **Step 2: Verify the reading against a multimeter**

Add to `drive_test.ino`, replacing the fixed `vBusAssumed`:

```c
#define VBUS_ADC_PIN 34
// 100k / 22k divider. ESP32 ADC is non-linear at the rails, so this needs the
// two-point trim below rather than the nominal ratio alone.
float vbusScale  = 5.545f;      // (100+22)/22, refined by measurement
float vbusOffset = 0.0f;

float readBusVoltage() {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(VBUS_ADC_PIN);
  return (acc / 16.0f / 1000.0f) * vbusScale + vbusOffset;
}
```

Add a `b` command that prints `readBusVoltage()`, then compare against a multimeter
across the battery. Adjust `vbusScale` until they agree within 0.15 V.

Expected: agreement within 0.15 V across at least two different pack voltages.

- [ ] **Step 3: Point the duty clamp at the live reading**

Replace the body of `dutyMaxCounts()` from Task 1 so it uses `readBusVoltage()`
instead of `vBusAssumed`. Keep the `v` command as a manual override for bench work
with the divider disconnected.

```c
int dutyMaxCounts() {
  float vb = readBusVoltage();
  if (vb < 3.0f) vb = vBusAssumed;      // divider absent - fall back, fail safe
  float head = vb - v0Drop;
  if (head <= 0.1f) return 0;
  float d = V_MOTOR_MAX / head;
  if (d > 1.0f) d = 1.0f;
  return (int)(d * 255.0f);
}
```

- [ ] **Step 4: Write the ESP32 HAL**

Create `embedded/hal/hal_esp32.h`:

```cpp
#pragma once
#include "hal.h"

class Adafruit_INA219;

// Wraps the two INA219 channels, the divider, and the LEDC PWM peripheral.
// Duty is clamped inside setDuty(): with a 12 V rail feeding 6 V motors this is
// the only thing preventing over-voltage (design decision D5).
class Esp32Hal : public Hal {
 public:
  Esp32Hal(Adafruit_INA219 *left, Adafruit_INA219 *right);

  bool begin();
  HalCurrents readCurrents() override;
  float       readBusVoltage() override;
  void        setDuty(float dutyL, float dutyR) override;
  uint32_t    millisNow() override;

  void setClampVolts(float vMotorMax, float v0);

 private:
  Adafruit_INA219 *left_;
  Adafruit_INA219 *right_;
  float vMotorMax_ = 5.0f;
  float v0_        = 1.9f;
};
```

Create `embedded/hal/hal_esp32.cpp`:

```cpp
#include "hal_esp32.h"
#include <Adafruit_INA219.h>
#include <Arduino.h>
#include <Wire.h>

namespace {
constexpr int I2C_SDA = 21, I2C_SCL = 22;
constexpr int VBUS_ADC_PIN = 34;
constexpr int ENA = 25, IN1 = 26, IN2 = 27;    // right side
constexpr int ENB = 32, IN3 = 33, IN4 = 14;    // left side
constexpr int PWM_FREQ_HZ = 1000, PWM_BITS = 8;
constexpr float VBUS_SCALE = 5.545f;           // (100k+22k)/22k, trimmed in Step 2
}  // namespace

Esp32Hal::Esp32Hal(Adafruit_INA219 *left, Adafruit_INA219 *right)
    : left_(left), right_(right) {}

bool Esp32Hal::begin() {
  const int enPins[2] = {ENA, ENB};
  for (int i = 0; i < 2; i++) { pinMode(enPins[i], OUTPUT);
                                digitalWrite(enPins[i], LOW); }
  const int inPins[4] = {IN1, IN2, IN3, IN4};
  for (int i = 0; i < 4; i++) { pinMode(inPins[i], OUTPUT);
                                digitalWrite(inPins[i], LOW); }
  ledcAttach(ENA, PWM_FREQ_HZ, PWM_BITS);
  ledcAttach(ENB, PWM_FREQ_HZ, PWM_BITS);

  Wire.begin(I2C_SDA, I2C_SCL);
  bool ok = left_->begin() && right_->begin();
  if (ok) { left_->setCalibration_32V_2A(); right_->setCalibration_32V_2A(); }
  return ok;
}

void Esp32Hal::setClampVolts(float vMotorMax, float v0) {
  vMotorMax_ = vMotorMax;
  v0_ = v0;
}

float Esp32Hal::readBusVoltage() {
  uint32_t acc = 0;
  for (int i = 0; i < 16; i++) acc += analogReadMilliVolts(VBUS_ADC_PIN);
  return (acc / 16.0f / 1000.0f) * VBUS_SCALE;
}

uint32_t Esp32Hal::millisNow() { return millis(); }

HalCurrents Esp32Hal::readCurrents() {
  HalCurrents c{};
  c.iL = left_->getCurrent_mA()  / 1000.0f;
  c.iR = right_->getCurrent_mA() / 1000.0f;
  // Bit 0 of the bus-voltage register is the math-overflow flag. A wrapped
  // counter reads LOW, so an unchecked overflow looks like reduced load.
  c.overflowL = (left_->getBusVoltage_raw()  & 0x0001) != 0;
  c.overflowR = (right_->getBusVoltage_raw() & 0x0001) != 0;
  return c;
}

void Esp32Hal::setDuty(float dutyL, float dutyR) {
  // The clamp lives here: with a 12 V rail feeding 6 V motors, this function is
  // the only thing preventing over-voltage. Nothing upstream may bypass it.
  float vb = readBusVoltage();
  if (vb < 3.0f) vb = 16.8f;                   // divider absent - fail safe high
  const float head = vb - v0_;
  float cap = head > 0.1f ? vMotorMax_ / head : 0.0f;
  if (cap > 1.0f) cap = 1.0f;

  struct { int en, inFwd, inRev; float duty; } ch[2] = {
    {ENB, IN4, IN3, dutyL},                    // OUT4 = +, OUT3 = -
    {ENA, IN2, IN1, dutyR},                    // OUT2 = +, OUT1 = -
  };
  for (int i = 0; i < 2; i++) {
    float mag = fabsf(ch[i].duty);
    if (mag > cap) mag = cap;
    const bool fwd = ch[i].duty >= 0.0f;
    if (mag <= 0.0f) {
      digitalWrite(ch[i].inFwd, LOW);
      digitalWrite(ch[i].inRev, LOW);
    } else {
      digitalWrite(ch[i].inFwd, fwd ? HIGH : LOW);
      digitalWrite(ch[i].inRev, fwd ? LOW : HIGH);
    }
    ledcWrite(ch[i].en, (int)(mag * 255.0f));
  }
}
```

The pin map matches `drive_test.ino` exactly, including `inFwd` being the pin that
feeds the motor's POSITIVE lead, so HIGH is always forward.

- [ ] **Step 5: Verify the overflow bit is actually reachable**

`[VERIFY]` from the design's open items. Confirm `getBusVoltage_raw()` is public in
the installed Adafruit INA219 library:

```bash
grep -n "getBusVoltage_raw\|public:" ~/Arduino/libraries/Adafruit_INA219/Adafruit_INA219.h
```

If it is not public, read register `0x02` directly over `Wire` instead. Do not skip
this — Task 11 scenario 8 depends on the flag being real.

- [ ] **Step 6: Commit**

```bash
git add embedded/hal/hal_esp32.h embedded/hal/hal_esp32.cpp embedded/drive_test/drive_test.ino
git commit -m "feat(hal): bus-voltage divider and ESP32 HAL with overflow detection"
```

---

### Task 13: Firmware integration

**Files:**
- Create: `embedded/drive_ctl/drive_ctl.ino`
- Modify: none

**Interfaces:**
- Consumes: `driveUpdate`/`driveReset` (Task 9), `Esp32Hal` (Task 12), `kMotorLeft`/`kBaselinePoints` (Task 2)
- Produces: the running firmware

This is a new sketch rather than an edit to `drive_test`, so the bench tool stays
available for diagnosis when the loop misbehaves.

- [ ] **Step 1: Confirm the gate**

Run: `cd embedded && make test`
Expected: `0 failures`. Do not proceed otherwise — NFR-5 and the design's §8 both
make this the precondition for driving real motors.

- [ ] **Step 2: Write the sketch**

Create `embedded/drive_ctl/drive_ctl.ino` with symlinks or copies of the core files
into the sketch directory (the Arduino build only compiles sources beside the
`.ino`):

```bash
mkdir -p embedded/drive_ctl
for f in embedded/control/*.h embedded/control/*.cpp embedded/hal/hal.h \
         embedded/hal/hal_esp32.h embedded/hal/hal_esp32.cpp; do
  ln -sf ../../$f embedded/drive_ctl/$(basename $f)
done
```

The sketch runs the loop at a fixed 100 Hz and prints state at 5 Hz:

```c
#include <Adafruit_INA219.h>
#include "drive_core.h"
#include "calibration.h"
#include "hal_esp32.h"

Adafruit_INA219 inaL(0x40), inaR(0x41);
Esp32Hal hal(&inaL, &inaR);

DriveConfig cfg;
DriveState  state;
float cmdL = 0.0f, cmdR = 0.0f;

const uint32_t PERIOD_MS = 10;      // 100 Hz
uint32_t nextTick = 0, nextPrint = 0;

void setup() {
  Serial.begin(115200);
  delay(300);
  if (!hal.begin()) Serial.println(F("HAL init FAILED - check the INA219s"));

  cfg.motorL = kMotorLeft;
  cfg.motorR = kMotorRight;
  cfg.baseline = { kBaselinePoints, kBaselineCount };
  cfg.load   = { 0.3f, 0.30f, 0.15f };
  cfg.safety = { 5.0f, 1.8f, 1.5f, 0.5f, 200, 9.0f };
  cfg.pi     = { 0.15f, 0.4f, 0.50f, 2.0f, 0.20f };
  driveReset(&state);
  hal.setClampVolts(cfg.safety.vMotorMax, cfg.motorL.v0);
  Serial.println(F("drive_ctl ready. w/s/a/d to drive, space stops."));
}

void loop() {
  const uint32_t now = millis();
  if ((int32_t)(now - nextTick) < 0) return;
  nextTick = now + PERIOD_MS;

  while (Serial.available()) {
    switch (Serial.read()) {
      case 'w': cmdL =  0.4f; cmdR =  0.4f; break;
      case 's': cmdL = -0.4f; cmdR = -0.4f; break;
      case 'a': cmdL = -0.4f; cmdR =  0.4f; break;
      case 'd': cmdL =  0.4f; cmdR = -0.4f; break;
      case ' ': cmdL = cmdR = 0.0f; driveReset(&state); break;
      default: break;
    }
  }

  const HalCurrents hc = hal.readCurrents();
  DriveInputs in{};
  in.cmdL = cmdL; in.cmdR = cmdR;
  in.iL = hc.iL; in.iR = hc.iR;
  in.overflowL = hc.overflowL; in.overflowR = hc.overflowR;
  in.vBus = hal.readBusVoltage();
  in.tMs  = now;

  const DriveOutputs out = driveUpdate(&state, cfg, in);
  hal.setDuty(out.dutyL, out.dutyR);

  if ((int32_t)(now - nextPrint) >= 0) {
    nextPrint = now + 200;
    Serial.printf("V %.2f  duty %+.2f/%+.2f  boost %.0f%%  bemf %.2f/%.2f  f%d\n",
                  in.vBus, out.dutyL, out.dutyR, out.boostPct * 100.0f,
                  out.vBemfL, out.vBemfR, (int)out.fault);
  }
}
```

- [ ] **Step 3: Compile and upload**

```bash
~/bin/arduino-cli compile --fqbn esp32:esp32:esp32doit-devkit-v1 embedded/drive_ctl
~/bin/arduino-cli upload -p /dev/ttyUSB0 --fqbn esp32:esp32:esp32doit-devkit-v1 embedded/drive_ctl
```

- [ ] **Step 4: Bench-verify, wheels off the ground**

Press `w`. Expected: `boost` stays near 0%, `bemf` positive and steady, `f0`.

Squeeze one wheel by hand. Expected: `boost` stays near 0% — that is the asymmetry
suppression, not a bug.

Squeeze both wheels. Expected: `boost` climbs, then a `f2` stall fault within about
200 ms of the wheels actually stopping, and duty drops to zero.

- [ ] **Step 5: Ramp test on the floor**

Drive at a fixed `w` onto a shallow ramp. Expected: `boost` rises on the slope and
falls back near zero at the crest without a lurch. Watch that motor voltage stays at
or below 5.0 V throughout.

Stop immediately and report if boost saturates on the flat, or the robot lurches at
the crest — those are the two failure signatures this design is built to avoid.

- [ ] **Step 6: Commit**

```bash
git add embedded/drive_ctl/
git commit -m "feat(firmware): incline-compensating drive loop on the ESP32"
```

---

### Task 14: Documentation

**Files:**
- Modify: `spec.md` (§4, §6.1, §7, §8), `overview_controls.md` (§2), `UPDATES.md`

**Interfaces:**
- Consumes: measured constants from Task 2; bench results from Task 13
- Produces: resolved `[TBD]`s and the §6.1 contract change the cloud engineer is waiting on

- [ ] **Step 1: Resolve the `[TBD]` rows in `spec.md` §4**

- Drive motors: `Dual-shaft mini gear motor, SKU FAM1029, 48:1, rated 3.0-6.0 V`
- Motor driver: `L298N dual H-bridge, 2 A per channel; both motors of a side paralleled onto one channel`
- Voltage sensor: `100k/22k resistor divider to GPIO34, 100 nF to GND`
- Current sensor: append `range: setCalibration_32V_2A, overflow at 3.2 A; math-overflow bit checked every sample`

- [ ] **Step 2: Add the supply assumption to `spec.md` §7**

Add as assumption 7:

> **7. The 12 V supply exceeds the motors' 3–6 V rating**, and the motors are protected by a firmware duty clamp derived from measured `V_bus` rather than by a buck converter. This is a deliberate decision (design doc D5) accepting that firmware is the only protection. The clamp is enforced inside the PWM write so no code path can bypass it, and is recomputed every cycle so a fuller pack tightens it automatically. Incline compensation regulates back-EMF **voltage** directly rather than estimated speed, which removes the need to measure `k_e`.

- [ ] **Step 3: Update the incline row in `spec.md` §8**

Append to the existing criterion:

> ...and never commands a duty above `V_MOTOR_MAX / (V_bus − V0)`, verified by scenario 7 of the design's sim suite.

- [ ] **Step 4: Correct `overview_controls.md` §2**

Replace the error-signal bullet:

> - **Error signal**: deviation of measured back-EMF voltage from a flat-ground baseline curve indexed on commanded duty. **Not** current deviation — steady-state current is set by load torque rather than duty, so a current-error loop never sees its error return to zero and winds up to saturation. See §9 of `docs/superpowers/specs/2026-09-03-incline-compensation-design.md`. Current keeps its role as load detector, stall gate and hard ceiling.

- [ ] **Step 5: Add the telemetry fields to `spec.md` §6.1**

Add `boost_pct` (float, 0.0–0.5) and `compensating` (bool) to the telemetry message
example and field table. Both are additive; nothing is renamed or removed.

- [ ] **Step 6: Move `UPDATES.md` row 6 to resolved**

```
| 6 | Incline-compensation telemetry fields (`spec.md` §6.1) | Landed in §6.1: `boost_pct` (float 0.0-0.5) and `compensating` (bool) added to the telemetry message. Additive only - nothing renamed or removed, so existing GUI work keeps parsing. | 2026-09-03 |
```

- [ ] **Step 7: Commit**

```bash
git add spec.md overview_controls.md UPDATES.md
git commit -m "docs: resolve motor/driver/voltage TBDs, correct the loop structure, land telemetry fields"
```

---

## Notes for the executor

**The two failure signatures this design exists to prevent**, either of which means
stop and report rather than tune around:

1. Boost saturating on flat ground — the loop is not closing, which is the exact
   pathology §9 of the design describes.
2. A lurch at the crest of a ramp — anti-windup is not working.

**Never relax a threshold to make a test pass.** The 500 ms and 200 ms budgets are
from `spec.md` §8; the duty cap is from the motor's datasheet rating. A budget that
cannot be met is a finding about the design.

**Task 2's measurements are load-bearing.** Every constant downstream derives from
them. If `R_tot` or `V0` look implausible, redo the measurement rather than
proceeding — a wrong `V0` puts its full error into every `V_bemf` estimate and
into the duty clamp that protects the motors.
