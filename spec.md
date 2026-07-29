# Spec — Ground Robot + Cloud Control System

## 0. Status of this document

This is a Phase 1 spec, written before any AWS resource exists and before BOM is finalized. Unknown hardware is marked `[TBD: ...]`. Any technical claim about AWS/ROS2/Nav2/SLAM Toolbox/D*-Lite behavior that isn't fully verified is marked `[VERIFY]` with a note on how to check it. Architecture choices not dictated by the background (e.g. which transport connects ESP32↔phone) are marked `[ASSUMPTION]` and are load-bearing for later phases — if the user overrides one, downstream sections need re-checking.

---

## 1. Functional Requirements

**FR-1 — Three operating modes.** The system supports exactly three operating modes: Manual, Autonomous, Follow-me. No other mode exists as a peer to these (a safety interlock state, SAFE_HOLD, exists but is cross-cutting, not a fourth mode — see §5).

**FR-2 — Manual mode.** Operator drives the robot directly via phone GUI controls (e.g. virtual joystick/d-pad). Commands pass phone → ESP32 with no cloud dependency.

**FR-3 — Autonomous mode.** Operator specifies a goal (via GUI, e.g. tap-on-map or coordinate entry). EC2 (SLAM Toolbox + Nav2 + D*-Lite global planner) computes a global path against the current map and streams waypoints down to the ESP32. ESP32 performs local path-following and reflexive obstacle response between waypoint updates.

**FR-4 — Follow-me mode.** Robot autonomously tracks and follows a nearby moving target using onboard 2D lidar (leg/cluster detection — see §7 assumption), maintaining a standoff distance, without requiring a cloud round-trip for the tracking/following control loop itself.

**FR-5 — Payload pickup.** Operator (or an autonomous task step, in a later iteration) can trigger the electromagnet to pick up or release small magnetic payloads. Electromagnet control is a discrete on/off actuator command owned by the ESP32.

**FR-6 — Incline compensation.** The ESP32 autonomously detects increased motor load consistent with an incline (via current/voltage sensing used to infer back-EMF behavior — see §7) and closed-loop reallocates drive power to maintain commanded velocity, without per-terrain manual tuning.

**FR-7 — Mapping.** EC2 builds and maintains an occupancy/pose-graph map via SLAM Toolbox from lidar + odometry data relayed by the phone. The map is available to the GUI for operator situational awareness and to Nav2/D*-Lite for planning.

**FR-8 — Mode switching safety.** Switching modes at any time is safe: in-flight autonomous waypoints or follow-me target locks are discarded, not carried across a mode boundary (see §5).

**FR-9 — Comms-loss fail-safe.** Loss of ESP32↔phone or phone↔EC2 communication does not result in unsafe motion. See §5, §8.

---

## 2. Non-Functional Requirements

| ID | Requirement | Notes |
|---|---|---|
| NFR-1 | ESP32 control loop rate | `[ASSUMPTION]` 100–200 Hz for motor PID / current sampling. Actual achievable rate depends on `[TBD: ESP32 variant]` and sensor bus speeds. |
| NFR-2 | Safety reaction latency | Fault → SAFE_HOLD entry in `[ASSUMPTION] <100 ms`, computed entirely on ESP32, independent of phone or cloud availability. |
| NFR-3 | ESP32↔phone latency | `[ASSUMPTION] <50 ms` typical, wired local link (see §7). |
| NFR-4 | Phone↔EC2 latency | Not bounded tightly — variable over cellular/Wi-Fi, budgeted 100 ms–2000+ ms `[VERIFY: measure once real network path exists]`. Explicitly outside the safety path (NFR-2 must not depend on this). |
| NFR-5 | Local-first parity | Every subsystem exercised end-to-end on the local dev machine before any AWS resource is created, using identical interfaces/message contracts (hard constraint from project brief). |
| NFR-6 | Availability under cloud loss | Robot continues safe local operation (last waypoint plan, or SAFE_HOLD if plan is stale) when EC2 is unreachable. Never assumes cloud connectivity for basic safety. |
| NFR-7 | Cost control | No AWS resource runs unattended without a budget alarm; large/GPU instances are not left running idle (see plan.md 2b). |
| NFR-8 | Observability | Every subsystem emits enough telemetry (timestamps, sequence numbers, fault flags) to reconstruct a post-hoc timeline of a mode switch or fault. |

---

## 3. System Boundary Diagram

```
                         (local, wired — see §7 ASSUMPTION)
   ┌───────────────┐   USB-serial, framed JSON   ┌──────────────────────┐
   │     ESP32     │◄───────────────────────────►│   Android Phone       │
   │               │                              │   (bridge + GUI)     │
   │ - motor PID   │                              │                      │
   │ - IMU (I2C)   │                              │  - GUI (Manual /     │
   │ - GPS (UART)  │                              │    Autonomous /      │
   │ - curr/volt   │                              │    Follow-me)        │
   │   sensing     │                              │  - relays telemetry  │
   │ - electromag  │                              │  - relays commands   │
   │   driver      │                              │  - lidar USB-OTG ────┼──┐
   │ - SAFE_HOLD   │                              │    host (raw scans)  │  │
   │   interlock   │                              └──────────┬───────────┘  │
   └───────────────┘                                         │              │
                                                    MQTT/TLS  │      2D Lidar
                                          (cellular or Wi-Fi) │      [TBD]
                                                               ▼
                                                  ┌──────────────────────────┐
                                                  │   EC2 (Linux instance)   │
                                                  │                          │
                                                  │  - SLAM Toolbox          │
                                                  │  - Nav2                  │
                                                  │  - D*-Lite global        │
                                                  │    planner loop          │
                                                  │  - map store             │
                                                  └──────────────────────────┘
```

Boundary ownership:
- **ESP32**: real-time control, real-time safety, local reflex behavior. Nothing on this side of the boundary may block on phone or cloud availability.
- **Phone**: bridge + GUI + lidar host. No control authority of its own beyond forwarding operator intent and a local e-stop button press; not a safety-critical compute node (see Warnings in plan.md 2c for the implication of a phone-software crash).
- **EC2**: heavy math only — SLAM, global planning. Never in the real-time safety path.

---

## 4. Sensor List

| Sensor | Purpose | Attaches to | Status |
|---|---|---|---|
| IMU | Orientation, pitch/roll for incline detection, gyro for local dead-reckoning | ESP32 (I2C/SPI) | `[TBD: exact model — e.g. 6-DoF vs 9-DoF class, needs to be decided before driver code is written]` |
| GPS | Coarse global position (outdoor), goal-reachability context | ESP32 (UART) | `[TBD: exact module, e.g. update rate / accuracy class]` |
| 2D Lidar | SLAM scan input, obstacle detection, Follow-me target tracking | Phone (USB-OTG host) — see §7 ASSUMPTION | `[TBD: exact model, range/FOV/scan-rate class]` |
| Current sensor | Per-motor (or bus-level) current for stall detection and back-EMF-derived load inference | ESP32 (I2C/ADC) | `[TBD: exact IC — e.g. shunt+amp class]` |
| Voltage sensor | Bus voltage for back-EMF inference and battery brownout detection | ESP32 (I2C/ADC) | `[TBD: exact IC]` |
| Drive motors | Locomotion, 4-wheel | — | `[TBD: brushed DC gearmotor assumed — see §7 — vs BLDC; this changes the back-EMF sensing method]` |
| Motor driver | PWM drive of 4 motors | ESP32 (PWM/GPIO) | `[TBD: exact H-bridge/driver IC]` |
| Electromagnet + driver | Payload pickup actuator | ESP32 (GPIO via MOSFET/relay) | `[TBD: coil spec, driver transistor, and whether a flyback/snubber is included]` |
| Battery | Power source | — | `[TBD: chemistry, capacity, and whether a BMS with brownout cutoff exists]` |
| ESP32 variant | Compute for real-time control | — | `[TBD: e.g. WROOM-32 vs S3 — affects available bus count, USB-OTG availability, core count for FreeRTOS task split]` |

---

## 5. Mode State Machine

**Authority**: the ESP32 is the single source of truth for the *current confirmed mode*. The phone GUI only *requests* a mode; the ESP32 accepts or rejects the request and reports the confirmed mode back. This follows directly from "ESP32 owns real-time safety" — mode arbitration is a safety-relevant decision (e.g. don't enter Autonomous without a valid plan) and must not depend on the phone or cloud being correct.

**States (three operating modes)**: `MANUAL`, `AUTONOMOUS`, `FOLLOW_ME`.

**Cross-cutting interlock (not a fourth operating mode)**: `SAFE_HOLD`. This supersedes whichever mode is active on entry and is exited only back into `MANUAL`, never auto-resuming `AUTONOMOUS`/`FOLLOW_ME`. It exists because the system must have *some* well-defined behavior on fault/comms-loss, but is deliberately not counted as a peer operating mode per the project's "exactly three modes" constraint — it's a safety layer underneath the modes, not a mode the operator selects.

### Transition table

| From | To | Trigger | In-flight command handling |
|---|---|---|---|
| MANUAL | AUTONOMOUS | Operator selects Autonomous **and** EC2 has published a fresh, non-empty waypoint list for the current goal **and** no active fault | N/A (entering) |
| MANUAL | FOLLOW_ME | Operator selects Follow-me **and** lidar target-lock acquired within `[ASSUMPTION] 5 s` | N/A (entering) |
| AUTONOMOUS | MANUAL | Operator touches manual control input, or explicit mode switch | Bounded ramp-down of current velocity command (not instant stop — avoids tip-over); queued waypoints discarded; new manual commands start from **zero**, not from the last autonomous command |
| FOLLOW_ME | MANUAL | Operator override, or target lock lost for `[ASSUMPTION] 3 s` | Same ramp-down-to-zero behavior; GUI alerted on auto-fallback |
| AUTONOMOUS | FOLLOW_ME | **Not allowed directly** `[ASSUMPTION — design choice for safety simplicity]` | Must pass through MANUAL first |
| FOLLOW_ME | AUTONOMOUS | **Not allowed directly** `[ASSUMPTION]` | Must pass through MANUAL first |
| any mode | SAFE_HOLD | Heartbeat loss (ESP32↔phone), overcurrent/stall fault, IMU fault, e-stop command | Motors bounded-stop (or immediate cutoff on overcurrent trip); queued waypoints/target-lock discarded; electromagnet state **latched**, not auto-dropped, unless brownout-protection logic requires load shedding (see plan.md Warnings) |
| SAFE_HOLD | MANUAL | Explicit operator acknowledgment/reset action only | Starts from zero command |

Rejected mode-change requests return a reason code to the phone (e.g. `NO_VALID_PLAN`, `NO_TARGET_LOCK`, `ACTIVE_FAULT`) so the GUI can explain the refusal rather than silently ignoring it.

---

## 6. Interface Contracts

### 6.1 ESP32 ↔ Phone

- **Transport**: `[ASSUMPTION]` USB CDC-ACM serial (wired). Chosen over BLE/Wi-Fi for determinism, power delivery, and because the phone is assumed mounted on the robot as a permanent bridge (see §7). If the real build instead mounts the phone off-robot, this assumption breaks and needs revisiting — flag before firmware bring-up.
- **Framing**: newline-delimited JSON. Chosen over a binary protocol for debuggability (a hobby build benefits from being able to `cat` the serial port and read it) at the cost of some bandwidth/parse overhead — acceptable given payload sizes are small (lidar does **not** transit this link, see §7).
- **ESP32 → Phone (telemetry)**, target `[ASSUMPTION] 10–50 Hz`:
  ```json
  {"seq": 1234, "ts_ms": 88213001, "mode": "AUTONOMOUS",
   "imu": {"roll": 0.4, "pitch": 6.1, "yaw": 122.3},
   "gps": {"lat": 0.0, "lon": 0.0, "fix": true, "hdop": 1.2},
   "motors": {"fl": {"duty": 0.62, "current_a": 1.1}, "fr": {...}, "rl": {...}, "rr": {...}},
   "incline_comp": {"active": true, "est_grade_deg": 8.5, "power_boost_pct": 18},
   "electromagnet": {"state": "off"},
   "battery": {"voltage": 11.8, "soc_pct": 64},
   "fault_flags": []}
  ```
- **Phone → ESP32 (commands)**, event-driven, plus mandatory heartbeat `[ASSUMPTION] ≥2 Hz`:
  ```json
  {"seq": 5567, "ts_ms": 88213050, "cmd_type": "waypoint_list",
   "payload": {"waypoints": [{"x":1.2,"y":0.4},{"x":2.0,"y":1.1}], "plan_id": "p-991"}}
  ```
  `cmd_type` ∈ `{mode_request, manual_drive, waypoint_list, estop, magnet_trigger, heartbeat}`.
- **Fail-safe**: no heartbeat within `[ASSUMPTION] 500 ms` → ESP32 enters `SAFE_HOLD` unilaterally. This is a hard requirement, not best-effort: the ESP32 must not need phone cooperation to detect phone-side failure.
- **Latency budget**: NFR-3 (<50 ms typical).
- **Who owns real-time safety**: ESP32, unconditionally.

### 6.2 Phone ↔ EC2

- **Transport**: `[ASSUMPTION]` MQTT over TLS. Chosen over raw WebSocket for built-in reconnect/QoS semantics and because the *same broker software* can run identically in the local dev sim (Mosquitto in Docker) and on EC2 (Mosquitto on the instance) — this is the mechanism that satisfies the "drop-in replacement" hard constraint: only the broker's hostname/port changes between local and cloud, not the protocol, topic structure, or payload schema.
- **Topics** (namespaced by robot id, `[ASSUMPTION]` single robot, id fixed for now):
  | Topic | Direction | Content | Rate |
  |---|---|---|---|
  | `robot/{id}/telemetry` | phone→cloud | aggregated ESP32 telemetry + confirmed mode | `[ASSUMPTION] 1–5 Hz` |
  | `robot/{id}/scan` | phone→cloud | raw 2D lidar scan relay | sensor-native rate `[TBD, depends on lidar model]` |
  | `robot/{id}/odom` | phone→cloud | odometry estimate (IMU/wheel-derived) for SLAM input | `[ASSUMPTION] matches scan rate or faster` |
  | `robot/{id}/map` | cloud→phone | current map, for GUI display (not control-critical) | on-change, low rate |
  | `robot/{id}/waypoints` | cloud→phone→ESP32 | D*-Lite output path | event-driven, on (re)plan |
  | `robot/{id}/cmd` | phone→cloud | e.g. "compute path to goal (x,y)" | event-driven |
  | `robot/{id}/mode` | phone→cloud | mode changes, for cloud-side context (e.g. pause planning in Manual) | event-driven |
- **Latency budget**: NFR-4. Explicitly **not** part of the safety path — see §9 latency-budget argument (also elaborated in plan.md).
- **Who owns real-time safety**: neither side. This link only ever carries planning/mapping data and non-real-time telemetry.

### 6.3 Phone ↔ Lidar (USB-OTG)

- **Transport**: `[ASSUMPTION]` USB, phone as USB host. Chosen so that high-bandwidth raw scan data doesn't have to transit the ESP32's UART link, since the ESP32 doesn't need the full scan for its reflex loop (see §7).
- Raw scan is relayed upward to EC2 (§6.2) largely unmodified; the phone may additionally compute a cheap derived signal (e.g. "nearest obstacle distance in forward cone") to push down to the ESP32 at low rate over §6.1, so the ESP32 isn't fully blind to obstacles despite not owning the full scan. `[ASSUMPTION — this derived-signal path is a design recommendation, not yet a firm requirement]`.

---

## 7. Key Architecture Assumptions (flagged for early review)

These aren't in the original background and were decided here to make the spec concrete. None require new BOM beyond what's already listed; all are reversible if the user disagrees.

1. **Phone is permanently mounted on the robot** as the bridge (not carried separately by an operator). This is what makes Follow-me need to work via onboard sensing rather than "follow the phone's GPS."
2. **Follow-me uses 2D-lidar-based leg/cluster tracking**, not a phone-GPS-follow or camera-based approach (no camera is in the sensor list). `[VERIFY]` — lidar leg-detection for person-following is an established technique in mobile robotics, but its reliability (false locks on furniture/other legs, indoor vs outdoor performance) needs empirical validation in the local sim and later on hardware; don't treat it as guaranteed-robust without testing.
3. **Drive motors are assumed brushed DC gearmotors** (typical for hobby 4WD chassis), not BLDC. This matters because the back-EMF sensing method differs significantly between the two (§ incline compensation below). If the actual motors are BLDC, this section needs rework — flag before writing ESP32 motor-control firmware.
4. **ESP32↔phone is wired USB serial**, not BLE/Wi-Fi (§6.1). Revisit if the physical mounting plan changes.
5. **Phone↔EC2 uses self-hosted MQTT (Mosquitto-class broker)**, not a managed service like AWS IoT Core, specifically so the local-sim and cloud endpoints are interface-identical (hard constraint). This is a deliberate simplicity/parity choice over IoT Core's extra managed features (device shadows, rules engine) which the project doesn't currently need.

### On back-EMF, specifically

"Back-EMF sensing" isn't a plug-and-play sensor — it needs to be derived, not read directly, while the motor is under PWM drive:

- For a brushed DC motor driven by PWM, back-EMF is proportional to motor speed. While the driver is actively sourcing current (PWM "on" phase), the measured terminal voltage reflects `V_applied = I·R + L·dI/dt + V_bemf`, so back-EMF isn't directly separable from a single voltage reading.
- The two standard approaches: (a) sample bus current/voltage during the PWM **off-time** (recirculation/freewheel phase), when applied voltage is ~0 and the remaining decaying voltage more directly reflects back-EMF; or (b) use a motor electrical model and subtract the `I·R` term from the on-phase measurement to estimate `V_bemf`. `[VERIFY]` — exact method depends on the motor driver topology chosen `[TBD]` and needs to be nailed down once that part is picked.
- **What this system actually uses as the incline-detection signal**: at constant commanded duty cycle, an incline increases mechanical load, which slows the motor, which *drops* back-EMF and *raises* current draw for the same applied voltage. The practical, driver-topology-agnostic proxy is: **current rise at constant commanded duty cycle**, filtered (see plan.md Warnings for PWM-switching-noise filtering), used as the error signal for the closed-loop power-reallocation controller. True back-EMF estimation (method a/b above) is a refinement, not a hard prerequisite, and can be added once the motor driver IC is chosen.

---

## 8. Acceptance Criteria

| Subsystem | Criterion |
|---|---|
| ESP32 control loop | Maintains commanded loop rate within `[ASSUMPTION] ±10%`; detects stall (current > threshold with near-zero measured speed) within `[ASSUMPTION] 200 ms`; enters SAFE_HOLD within NFR-2 (<100 ms) of any qualifying fault |
| Incline compensation | Increases power allocation proportionally to detected load rise within `[ASSUMPTION] 500 ms` of a grade change; never exceeds motor/driver max current rating (no runaway — see plan.md Warnings for the stall/runaway failure mode) |
| ESP32↔Phone comms | Heartbeat loss detected within 500 ms (§6.1), triggers SAFE_HOLD reliably (tested by physically/synthetically breaking the link in sim and on hardware) |
| Phone↔EC2 comms | Robot continues safe local operation (holds last valid plan, or SAFE_HOLD if plan is stale beyond `[ASSUMPTION] 10 s`) when cloud is unreachable; GUI clearly indicates "cloud offline" |
| SLAM (EC2, SLAM Toolbox) | Produces map + pose estimate at `[TBD/VERIFY: exact achievable rate depends on lidar spec and instance sizing]` given scan+odom input |
| Global planning (D*-Lite via Nav2) | Replans within `[ASSUMPTION] 2 s` of new obstacle information reaching the map; replanning is rate-limited/hysteresis-bounded to avoid thrashing (see plan.md Warnings) |
| Mode state machine | Every transition in §5's table is exercised and passes in the local sim harness before any hardware bring-up |
| Follow-me | Maintains lock within `[ASSUMPTION] 0.5 m` of target under tested lidar conditions; degrades to MANUAL safely (not a lunge or freeze) on lock loss |
| Electromagnet | Activates/deactivates within `[ASSUMPTION] 100 ms` of command; verified not to brown out the bus under worst-case (low battery + peak coil inrush) load test before it's ever tested on hardware carrying a real payload |

---

## 9. Latency-Budget Argument (summary)

The chain that must never depend on cloud round-trip time: **fault detection → SAFE_HOLD entry → motor command zeroing**. This entire chain is computed and executed on the ESP32 using only locally-available signals (current/voltage, IMU, heartbeat timers). The EC2 link only ever supplies a *waypoint list* or *map update* — inputs that are consumed opportunistically and cached; their absence degrades capability (no new plan) but never removes safety (the fault/SAFE_HOLD path doesn't reference them at all). This is why NFR-2 (<100 ms) and NFR-4 (100–2000+ ms, unbounded-ish) can coexist: they're different budgets for different responsibilities, not the same number measured twice.

Full elaboration of this argument, plus the specific failure modes it protects against, is in `plan.md` under each phase's Warnings & Edge Cases section.
