# Design — Load-Inference Incline Compensation (FR-6)

Status: design approved 2026-09-03, not yet implemented.
Owner: embedded/firmware.
Implements: `spec.md` FR-6, `overview_controls.md` §2 (with one correction, see §9).

---

## 1. Scope

The closed-loop controller that detects sustained drive load above a flat-ground
baseline and raises PWM to hold speed, without per-terrain manual tuning. This is
the project's central control-systems contribution (`overview_controls.md`,
Novelty 1).

It also creates the portable control core and HAL boundary that `plan.md` 2a.3
requires. That is deliberate: the feature is the first resident of that core, and
the boundary is what allows validation on Linux before the robot meets a ramp.

**Out of scope**: mode state machine, SAFE_HOLD entry/exit logic, and the Wi-Fi
link. This design *raises faults* into the SAFE_HOLD path; it does not implement
it.

---

## 2. Decisions

| # | Decision | Rejected alternative |
|---|---|---|
| D1 | Signal is **load**, honestly named — not grade | True incline classification, which needs the `[TBD]` IMU |
| D2 | Speed signal from **back-EMF via a resistor divider** on the battery rail | Encoders (BOM change); a blind time-and-current guard (trips mid-climb) |
| D3 | **Back-EMF speed-hold PI**; current is detector, stall gate and ceiling | Current-error PI (does not close — §9); bounded feedforward map |
| D4 | Active in **all three modes**, with GUI indication | Autonomous/Follow-me only; operator-toggleable |
| D5 | 12 V supply retained; motors protected by a **firmware duty clamp** | Buck converter to 6 V |

D5 is the human's explicit decision, taken after the over-voltage risk in §3 was
raised. Its consequence is that firmware is the *only* protection between a 12 V
rail and 3–6 V motors, which is why §7 treats the clamp as a safety mechanism
rather than a configuration value.

---

## 3. Hardware context

| Item | Value | Source |
|---|---|---|
| Drive motors | Dual-shaft mini gear motor, SKU `FAM1029`, 48:1 | datasheet, 2026-09-03 |
| Motor rating | **3.0–6.0 V**, no-load 200 mA @ 6 V, 200 rpm, 0.8 kg·cm | datasheet |
| Motor stall current | ~1.1–1.5 A each @ 6 V `[VERIFY — measured by §6.1]` | estimate |
| Driver | L298N, single board, 2 A per channel | bench |
| Topology | 2 motors paralleled per channel; 1 channel per side | `UPDATES.md` #5 |
| Supply | 12 V to L298N `VS` | bench |
| Current sense | 2× INA219, one per channel, in the motor return lead | `spec.md` §4 |
| Bus voltage | Resistor divider → ESP32 ADC (D2) | this document |

### 3.1 The two constraints that shape everything below

**Over-voltage.** With the L298N's drop, the motors see `≈ (12 − V0) · d`. At the
bench default of `speed = 180` that is ~7.1 V against a 6.0 V maximum; at
`speed = 255` it is ~10.1 V. `drive_test.ino` currently permits both.

**Over-current.** Two paralleled motors stalling on one channel draw an estimated
2.4–3 A at 6 V, against the L298N's 2 A per-channel rating. This is the most
likely cause of the board failure on 2026-08-29. It is *not* prevented by anything
currently in the firmware.

These matter more here than elsewhere because **the compensator's function is to
push more power into a high-load condition** — precisely the regime that destroys
the bridge. A naive implementation on this hardware is a board-destroying machine.
§7 exists for that reason.

---

## 4. Architecture

```
embedded/
  control/            portable core - no Arduino headers, compiles on Linux
    motor_model.*     affine V_bemf model; V0, R_tot
    baseline.*        flat-ground table + interpolation
    load_detector.*   normalized current, baseline deviation, L/R symmetry
    incline_comp.*    PI, saturation, anti-windup, slew limit
    safety.*          duty clamp, current ceiling, stall, brownout -> Fault
    drive_core.*      orchestrator
  hal/
    hal.h             readCurrents / readBusVoltage / setDuty / millis
    hal_esp32.cpp     INA219 + ADC divider + LEDC
    hal_sim.cpp       motor + gearbox + grade model, native Linux
  drive_test/         unchanged; bench tool, gains the `k` calibration command
```

The core is a pure function of its inputs: no globals, no clock reads, no I/O.
This is what makes the sim honest — the Linux build runs the same code that ships,
not a reimplementation of it.

```c
struct DriveInputs  { float cmdL, cmdR;      // operator command, -1..1
                      float iL, iR;          // A, duty-normalized
                      float vBus;            // V, from the divider
                      bool  iOverflowL, iOverflowR;
                      uint32_t tMs; };

struct DriveOutputs { float dutyL, dutyR;    // what reaches the bridge
                      float boostPct;        // telemetry + GUI
                      float vBemfL, vBemfR;
                      bool  compensating;
                      Fault fault; };
```

---

## 5. Signal chain and control law

```
INA219 x2 ──► duty-normalize ──► LPF ──┬──► load detector ──► arm / suppress
                                       │                            │
                                       │                            ▼
                                       └──► V_bemf ──► PI(target - V_bemf) ──► boost
ADC divider ──► V_bus ────────────────────────┘                            │
                                                                           ▼
                                        safety: clamp / ceiling / stall / brownout
                                                                           │
                                                            duty = cmd x (1 + boost)
```

### 5.1 Regulate on V_bemf, not ω

`ω̂ = V_bemf / k_e`, and the loop compares `ω̂` against a target derived the same
way, so `k_e` is a common scale factor and cancels. Regulating on back-EMF voltage
directly removes the need to measure `k_e` — which without a tachometer would need
a manual RPM measurement — and removes a constant that could be wrong.

```
V_bemf = V_bus · d_final − V0 − I_norm · R_tot
```

`d_final` is the actual applied duty (post-boost); `I_norm` is the duty-normalized
current already implemented in `drive_test.ino`.

`ω̂` is still reported in telemetry for human readability, computed with a
datasheet `k_e`. Nothing in the loop depends on that number being correct.

The stall gate transfers cleanly: since `k_e > 0`, `V_bemf ≈ 0` **is** near-zero
speed.

### 5.2 The loop

```
e        = V_bemf_flat(d_cmd) − V_bemf_measured        // volts, + means lagging
boost    = clamp( Kp·e + Ki·∫e dt , 0 , BOOST_MAX )
d_final  = min( d_cmd · (1 + boost) , DUTY_MAX_ABS )
```

On flat ground `boost = 0`, so `d_final = d_cmd` and `V_bemf` equals its target:
`e = 0`. That is the equilibrium, and it is why this loop closes where a
current-error loop does not (§9).

**Boost is common-mode** — a single scalar applied to both sides, driven by the
mean of the two speed errors. Skid-steer turning *is* a commanded left/right
asymmetry; per-side boost would fight it and corrupt the turn radius. The left/
right difference is used only to *suppress* boost (§7).

**Boost is non-negative.** The loop holds speed; it never reduces below what the
operator commanded. Cutting power under command is a surprise-motion failure.

### 5.3 Parameters

| Name | Starting value | Notes |
|---|---|---|
| Control rate | 100 Hz | sensing + estimation |
| PI rate | 50 Hz | grade is a slow disturbance |
| `Kp` | 0.15 /V | sim-tuned |
| `Ki` | 0.4 /(V·s) | sim-tuned |
| `BOOST_MAX` | 0.50 | |
| Boost slew limit | 2.0 /s | reaches max in 250 ms, inside the 500 ms budget |
| `D_CMD_MIN` | 0.20 | below this, duty normalization is invalid; loop idles |
| `DUTY_NORM_MIN` | 0.15 | already in `drive_test.ino` |

No derivative term: current is the noisiest signal in the system
(`plan.md` 2a.6) and grade does not change instantaneously
(`overview_controls.md` §2).

---

## 6. Calibration

Two bench procedures, both on hardware that exists today. Both produce constants
committed to git rather than stored in NVS — a versioned table is easier to diff
when a number looks wrong. NVS can come later.

### 6.1 V0 and R_tot — locked rotor, two points

With the rotor held, `V_bemf = 0`, so `V_bus · d = V0 + I · R_tot`. Drive at
`d = 0.12` and `d = 0.20`, record `V_bus` and `I_norm` at each, solve the two
equations.

This folds motor winding resistance and the L298N's current-dependent drop into a
single affine model. Treating the bridge drop as a fixed 1.9 V would push that
error straight into `V_bemf`.

Applied voltage at these duties is ~1.2–2.0 V, so locked-rotor current stays
modest. **Keep each burst under two seconds** — locked rotor is worst case for both
motor and bridge.

This procedure also yields the measured stall current per channel, resolving the
`[VERIFY]` in §3 and setting `I_CEIL` against data rather than an estimate.

### 6.2 The flat-ground table

New `k` command in `drive_test.ino`. For `d` in `0.20 … DUTY_MAX_ABS` in 8 steps,
driving straight on flat ground with no payload: settle 500 ms, average 500 ms,
record `I_flat(d)` and `V_bemf_flat(d)`. Emit as a C array for `baseline.cpp`.

One procedure produces both the load baseline and the speed target. Values between
table points are linearly interpolated.

---

## 7. Safety interlocks

Ordered by priority. Each is independent of the controller's internal state — per
`overview_controls.md` §2, a corrupted PI must not be able to command past them.

### 7.1 Duty clamp (D5 makes this load-bearing)

```c
DUTY_MAX_ABS = V_MOTOR_MAX / (V_bus − V0)     // recomputed every cycle
```

with `V_MOTOR_MAX = 5.0 V`, one volt below the datasheet maximum.

Two properties matter:

**It lives inside the PWM write**, not at the command layer — one choke point that
no code path, the compensator included, can route around.

**It is derived from measured `V_bus`, not hardcoded.** A charged pack reads higher
than a flat one, and if the 4S/16.8 V pack under consideration is fitted, the clamp
tightens automatically instead of silently over-volting by 40%.

`SPEED_MAX` in `drive_test.ino` drops from 255 to 128.

### 7.2 Current ceiling

`I_CEIL = 1.8 A` per channel — set by the L298N's 2 A rating with margin, not by
the motors. This is below the paralleled stall current, which is what makes it
protective rather than decorative.

On breach: boost is forced to decay on an independent code path. If still breached
after `T_CEIL`, duty is cut and a fault raised.

### 7.3 INA219 overflow

`setCalibration_32V_2A` overflows at 3.2 A — inside the fault region this design
needs to measure. **On overflow the reading wraps and current reads low**, so the
loop would see reduced load and boost harder into a real overcurrent.

The math-overflow bit in the bus-voltage register must be checked every sample and
treated as a ceiling breach, never as data. `[VERIFY]` whether the Adafruit library
exposes that bit; if not, read the register directly.

Re-shunting for a ~5 A range is the real fix and is recommended, but the overflow
check stays regardless.

### 7.4 Stall

`I_norm > I_STALL` **and** `V_bemf < V_STALL`, sustained beyond `T_STALL = 200 ms`
(`spec.md` §8) → zero boost, zero duty, fault → SAFE_HOLD.

Both conditions are required. A stalled motor and a heavily-loaded-but-climbing
motor both show elevated current; treating them the same means boosting into a
stall, which is exactly backwards (`overview_controls.md` §2).

### 7.5 Brownout

`V_bus < V_MIN` → SAFE_HOLD. `V_MIN` is `[TBD]` pending the battery decision in
`spec.md` §4; 9.0 V for a 3S Li-ion pack is the placeholder.

There is a feedback path worth naming: boosting raises current, which sags the
battery, which lowers `V_bus`, which lowers applied voltage — the loop can chase
its own supply down. `V_MIN` bounds it; it is also a reason to keep `BOOST_MAX`
modest.

### 7.6 Asymmetry suppression

When commanded duties are equal but `|I_L − I_R|` exceeds a threshold, the load is
one-sided — a snag or a bound wheel, not a grade. Boost is suppressed.

This is the one discrimination two current sensors genuinely support: a climb loads
both sides equally, a snag does not.

### 7.7 Anti-windup and reset

Conditional integration: the integral term freezes whenever the output is saturated
*and* the error would drive it further into saturation. Without this, cresting a
ramp produces a large overshoot — the classic failure of a PI with output limits.

The integrator resets to zero on SAFE_HOLD entry and on every mode exit, which is
`overview_controls.md` §4's zero-command rule applied to controller state.

---

## 8. Validation

`hal_sim.cpp` models motor, 48:1 gearbox, and a scriptable grade profile using
datasheet constants and the measured `R_tot`. The core runs first as a plain native
test binary; the ROS2 node in `plan.md` 2a.3 wraps that same core later rather than
being built twice.

Per NFR-5, every scenario passes in sim before the loop drives real motors.

| # | Scenario | Passes when |
|---|---|---|
| 1 | Flat, constant duty | Boost stays ≈ 0 — no false positive |
| 2 | Step to 15% grade | `V_bemf` recovers within 500 ms (`spec.md` §8) |
| 3 | Cresting the ramp | Boost decays, no overshoot — the anti-windup test |
| 4 | Locked wheel | Fault in < 200 ms, boost and duty zeroed |
| 5 | One-side snag | Asymmetry suppresses boost |
| 6 | `V_bus` sag | SAFE_HOLD |
| 7 | Max command + max boost | Final duty never exceeds `DUTY_MAX_ABS` |
| 8 | Injected INA219 overflow | Read as ceiling breach, not as low current |

Tests 7 and 8 are load-bearing: both encode failures that are otherwise silent, and
7 is the regression test for decision D5.

Per `plan.md` 2a.6, the synthetic current signal must carry PWM-frequency-correlated
noise. A filter tuned against clean data will be undersized for the real thing.

---

## 9. Correction to `overview_controls.md` §2

§2 specifies the error signal as "deviation of measured current from an expected
current-vs-duty baseline." **That loop does not close.** At steady state:

```
k_t · I = τ_load + b·ω + friction
```

Current is set by load torque; duty sets speed. So on a ramp: grade raises
`τ_load`, current rises, the PI raises boost, speed recovers — and current stays at
`τ_load / k_t`, right where it was. The error never returns to zero because the load
never went away, so the integrator winds up until it saturates. The `b·ω` term makes
current tick up slightly as speed recovers, marginally the wrong way.

Anti-windup bounds the damage but does not fix the structure: that loop is a
feedforward map wearing a PI costume. Its feedback variable is not moved by its own
output in the corrective direction.

The `V_bemf` formulation in §5 closes properly: load rises → `V_bemf` drops → boost
→ `V_bemf` recovers → error goes to zero and the integrator settles.

`overview_controls.md` §2 needs updating to match. Current keeps its role there as
detector, stall gate and ceiling.

---

## 10. Document changes this requires

Mostly resolving `[TBD]`s rather than altering agreed contracts:

| Document | Change |
|---|---|
| `spec.md` §4 | Motor row → `FAM1029`, 3–6 V; driver row → L298N; voltage-sensor row → resistor divider; current-sensor row → range note |
| `spec.md` §7 | New assumption: 12 V supply with 3–6 V motors, protected by firmware clamp (D5). Note that regulation is on `V_bemf`, not `ω` |
| `spec.md` §8 | Incline-compensation criterion gains the `DUTY_MAX_ABS` bound |
| `overview_controls.md` §2 | Loop-structure correction (§9) |
| **`spec.md` §6.1** | **Shared contract**: telemetry gains `boost_pct` and `compensating` for the GUI indication in D4 |

The §6.1 change is the only one touching a shared wire format. Per `CLAUDE.md` it
is not made unilaterally: an `UPDATES.md` row goes in alongside it so the cloud/GUI
engineer sees the new fields before any dashboard work binds to the old shape.

---

## 11. Open items

| Item | Status |
|---|---|
| Motor stall current per channel | `[VERIFY]` — measured by §6.1 |
| INA219 math-overflow bit exposure in Adafruit library | `[VERIFY]` — else read register directly |
| INA219 common-mode voltage during PWM off-time | `[VERIFY]` — outputs are high-Z, held by body diodes; must stay within 0–26 V |
| `V_MIN` brownout threshold | `[TBD]` — pending battery decision, `spec.md` §4 |
| Divider resistor values | `[TBD]` — sized for the final pack, must not exceed 3.3 V at full charge |
| `Kp` / `Ki` | Starting values only; tuned in sim (§8) |
| Re-shunt INA219 to ~5 A | Recommended, not blocking — §7.3 |
