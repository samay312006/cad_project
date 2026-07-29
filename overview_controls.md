# Overview — Controls & Embedded Systems

## Abstract

A hobby-scale 4-wheel ground robot pairs a real-time ESP32 controller with an Android phone bridge/GUI and a cloud "brain" (AWS EC2 running SLAM Toolbox, Nav2, and a custom D*-Lite global planner) to deliver three operating modes — Manual, Autonomous, and Follow-me — plus an electromagnet actuator for small magnetic payload pickup. The system's defining control property is a closed-loop, autonomous incline-compensation controller: rather than relying on manual per-terrain tuning, the robot infers increased drive load from current/voltage sensing (a back-EMF-informed signal) and reallocates motor power in real time. This is enforced by a strict architectural split — the ESP32 owns all real-time control and safety locally; the cloud is used only for heavy math (mapping, global path planning) and is never in the safety-critical path.

## Application

Intended as a low-cost platform for terrain-crossing payload retrieval (e.g. picking up small ferrous objects across mixed indoor/outdoor terrain including ramps/inclines) and as a full-stack robotics reference build — embedded real-time control through cloud-hosted SLAM/planning — at hobby-project cost. The control patterns here (load-based terrain adaptation without manual tuning, a hard local/cloud safety boundary) are the same shape used in larger field/warehouse robots; this project validates them cheaply before any larger investment.

## Novelty

1. **Load-inference incline compensation** — grade is inferred from motor electrical behavior (current rise / back-EMF drop under constant commanded duty cycle), not from a pre-tuned slope lookup table or IMU-pitch-only heuristic, so the response generalizes to loads and surfaces that weren't hand-tuned in advance.
2. **A hard local-reflex / global-planning boundary**, enforced by an explicit latency budget and message contract (not just an implied "the cloud is slow so be careful") — the ESP32's fault-to-safe-state path is provably independent of cloud round-trip time (see §4).
3. **Local-simulation-first development extended down to firmware**: the ESP32 control-loop logic is written once behind a hardware abstraction layer and validated as a native Linux process before it ever touches real silicon — the same "prove it locally before it's expensive to change" discipline the project applies to AWS is applied one level lower, to hardware.

*(Cloud/AWS architecture is summarized in §6 below; full depth is in `overview_cloud.md`, which shares this same abstract/application/novelty.)*

## Architecture

```
   ESP32 (real-time control + safety)  ◄──USB serial, JSON──►  Phone (bridge/GUI)
        │  IMU, GPS, current/voltage,                              │ lidar (USB-OTG)
        │  motor PWM, electromagnet                                │
        └── owns: mode arbitration, SAFE_HOLD,                     └── MQTT/TLS ──► EC2
            incline-compensation loop, stall/                          (SLAM Toolbox,
            brownout protection                                        Nav2, D*-Lite —
                                                                         heavy math only)
```

Full interface contracts, message schemas, and the sensor list (with `[TBD]` BOM gaps) are in `spec.md` §4/§6. This document focuses on what runs on the ESP32 and why it's structured the way it is.

---

## 1. Sensor Fusion

Inputs available to the ESP32's state estimate (per `spec.md` §4): IMU (orientation/rate), GPS (coarse absolute position, outdoor only), and current/voltage (indirect load/terrain signal, not a position sensor).

**Open gap, flagged rather than papered over**: the current sensor list has **no dedicated wheel encoders**. This means the odometry input SLAM Toolbox consumes (`spec.md` §6.2 `robot/{id}/odom`) is, as currently scoped, IMU-integration-based dead reckoning — which drifts over time in a way wheel odometry typically doesn't. SLAM Toolbox can still function from lidar scan-matching alone `[VERIFY: confirm slam_toolbox's tolerance for odometry-free or low-quality-odometry operation in the chosen mode]`, but robustness (especially during Follow-me, where the robot itself is moving reactively rather than along a smooth planned path) would likely benefit from adding low-cost wheel encoders. This is a recommendation to evaluate, not a decided BOM change — noted here because it's a real design gap, not because encoders were promised anywhere in the background.

**Fusion approach** `[ASSUMPTION — not yet implemented, this is the recommended starting design]`: a complementary or extended Kalman filter fusing gyro (rate) with accel (gravity-vector-derived tilt) for orientation, and GPS position (when fix quality is good) with IMU-integrated dead-reckoning for position, with GPS *down-weighted or excluded* during detected fix-quality drops (see Warnings, `plan.md` 2a.6) rather than trusted blindly. `[VERIFY]` exact filter choice (complementary filter is simpler/cheaper on an ESP32's compute budget; EKF is more principled but more compute) should be a deliberate tradeoff decision made once the ESP32 variant `[TBD]` and its available compute headroom is known.

---

## 2. Back-EMF-Derived Incline Compensation — Closed-Loop Controller

This is the project's central control-systems contribution. Full derivation of why back-EMF isn't directly readable and what proxy signal is actually used is in `spec.md` §7 ("On back-EMF, specifically") — summarized and extended here as a control loop.

### Loop structure

```
 grade change (disturbance)
        │
        ▼
 motor mechanical load ↑ → motor speed ↓ (at constant duty) → back-EMF ↓, current ↑
        │
        ▼
 current/voltage sense ──► filter (PWM-noise rejection, plan.md 2a.6) ──► ΔI estimate
        │                                                                    │
        │                                                                    ▼
        │                                            PI controller (ΔI → power_boost_pct)
        │                                                    │
        │                              saturation + anti-windup + hard current ceiling
        │                                                    │
        └────────────────────────── motor duty cycle ◄───────┘
```

- **Error signal**: deviation of measured current from an expected current-vs-duty baseline curve (calibrated on flat, unloaded ground), *not* raw current magnitude — this makes the controller respond to unexpected load, not to normal operating current.
- **Controller**: `[ASSUMPTION]` PI (proportional-integral), not full PID — a derivative term on a current signal is noise-sensitive (current is already the noisiest signal in the system, see PWM-switching-noise Warning in `plan.md` 2a.6) and isn't obviously needed for what's fundamentally a slow-varying disturbance (grade doesn't change instantaneously). Revisit only if step response proves too sluggish without it.
- **Saturation & anti-windup**: `power_boost_pct` is bounded (can't command more than the driver/battery can deliver); the integral term must stop accumulating once output saturates, or the controller will overshoot badly when the disturbance clears (e.g. cresting the incline) — a classic and easy-to-miss bug in any PI implementation with output limits.
- **Defense in depth against runaway/stall** (elaborated in `plan.md` 2a.6): the compensation loop's output is bounded by a **hard current ceiling that exists independently of the loop's own logic** — even if the PI controller's internal state is somehow wrong, this ceiling can't be commanded past. Separately, **stall must be an explicitly detected state** (high current *and* near-zero derived speed, sustained past a threshold), not inferred implicitly from "current is high" — a stalled motor and a heavily-loaded-but-moving motor both show elevated current, and treating them the same means the controller will keep increasing power into an actual stall, which is exactly backwards.

### Where this runs

Entirely on the ESP32, at the control loop's native rate (`spec.md` NFR-1, `[ASSUMPTION] 100–200 Hz`). No part of this loop reads from or waits on the phone or EC2 link — it is local reflex behavior by definition (see §3).

---

## 3. Local-Reflex vs. Global-Planning Split

| | ESP32 (local reflex) | EC2 (global planning) |
|---|---|---|
| Owns | mode arbitration, SAFE_HOLD, incline compensation, stall/brownout protection, path-following *between* waypoint updates | SLAM map building, D*-Lite global path computation |
| Timescale | milliseconds | seconds |
| Availability requirement | must always work | best-effort — absence degrades capability, not safety |
| Input | IMU, GPS, current/voltage, heartbeat state | lidar scan, odometry, goal |

The split isn't just "cloud is for expensive computation" — it's specifically that **nothing on the fault-detection-to-SAFE_HOLD path may reference cloud-sourced data**, per `spec.md` §9. The ESP32 caches the last valid waypoint list and continues local path-following/reflex behavior against it if the cloud link drops; it only enters SAFE_HOLD from a stale plan if that staleness itself crosses a threshold (`spec.md` §8, `[ASSUMPTION] 10 s`), which is a local timer check, not a cloud dependency.

---

## 4. Mode State Machine

Full transition table is in `spec.md` §5. The controls-relevant point: the ESP32 is the **sole authority** over the confirmed mode, not the phone GUI — because mode arbitration is itself a safety decision (e.g., refusing to enter Autonomous without a fresh valid plan), and safety decisions can't be delegated to a non-real-time, non-safety-rated bridge device. On any mode exit, in-flight commands (queued waypoints, a Follow-me target lock) are discarded, and the new mode starts from a **zero** command state rather than continuing the previous mode's last command — this avoids a surprise-motion failure mode where, e.g., switching out of Autonomous mid-turn continues that turn under manual control before the operator has given fresh input.

The cross-cutting `SAFE_HOLD` interlock (not counted as a fourth mode, per the project's exactly-three-modes constraint) supersedes whichever mode is active on fault, and requires an explicit operator acknowledgment to exit — it never auto-resumes autonomy, on the principle that a fault that already happened once shouldn't be silently retried without a human noticing.

---

## 5. ESP32 Real-Time Guarantees

`[ASSUMPTION/VERIFY]` — the following describes the recommended real-time structure; exact APIs depend on the ESP32 variant `[TBD]` and whether the firmware is built on ESP-IDF (FreeRTOS-based) or Arduino-on-ESP-IDF.

- **Task structure**: a high-priority control task (sensor sampling, incline-compensation loop, mode/SAFE_HOLD logic) should run at a fixed, guaranteed rate, separated from lower-priority tasks like phone-link JSON parsing — so a slow or malformed serial parse can't introduce jitter into the control loop. `[VERIFY]` ESP-IDF's FreeRTOS supports pinning tasks to specific cores on dual-core variants, which — if the chosen variant is dual-core — is a reasonable way to physically isolate the control task from comms handling; confirm against the specific variant chosen `[TBD]`, since some ESP32 variants are single-core.
- **Deterministic sampling**: current/voltage (and ideally IMU) sampling should be timer/ISR-triggered rather than sampled opportunistically inside a task loop, so sample timing doesn't jitter with whatever else the CPU is doing — this matters directly for the PWM-off-phase-gated sampling technique discussed in `spec.md` §7, which depends on sampling at a specific phase of the PWM cycle.
- **Watchdog**: a hardware or software watchdog timer should force a defined fail-safe state (motors to zero, not a silent reset that leaves motors mid-command) if the control task hangs — this is the last line of defense underneath the SAFE_HOLD logic itself, for the case where SAFE_HOLD logic can't run because something has already gone more seriously wrong (e.g. a task deadlock).
- **Fail-safe default**: on boot, and on any unhandled fault, the default output state is zero motor command and electromagnet off — never an unspecified/last-held state, which could be arbitrary garbage after a crash/reset.

---

## 6. Cloud Summary (light — see `overview_cloud.md` for depth)

EC2 runs SLAM Toolbox (mapping from relayed lidar scan + odometry) and Nav2 with a custom D*-Lite global planner (`plan.md` 2a.4), reachable over MQTT/TLS from the phone bridge. The whole stack is validated locally (Dockerized, same broker software, same message contracts) before any AWS resource exists, per the project's hard constraint. Full AWS topology, cost, and security detail is in `overview_cloud.md`.
