# Spec — Ground Robot + Cloud Control System

## 0. Status of this document

This is a Phase 1 spec, written before any AWS resource exists and before BOM is finalized. Unknown hardware is marked `[TBD: ...]`. Any technical claim about AWS/ROS2/Nav2/SLAM Toolbox/D*-Lite behavior that isn't fully verified is marked `[VERIFY]` with a note on how to check it. Architecture choices not dictated by the background (e.g. which transport connects ESP32↔phone) are marked `[ASSUMPTION]` and are load-bearing for later phases — if the user overrides one, downstream sections need re-checking.

---

## 1. Functional Requirements

**FR-1 — Three operating modes.** The system supports exactly three operating modes: Manual, Autonomous, Follow-me. No other mode exists as a peer to these (a safety interlock state, SAFE_HOLD, exists but is cross-cutting, not a fourth mode — see §5).

**FR-2 — Manual mode.** Operator drives the robot directly via phone GUI controls (e.g. virtual joystick/d-pad). Commands pass phone → ESP32 with no cloud dependency.

**FR-3 — Autonomous mode.** Operator specifies a goal (via GUI, e.g. tap-on-map or coordinate entry). EC2 (SLAM Toolbox + Nav2 + D*-Lite global planner) computes a global path against the current map and streams waypoints down to the ESP32. ESP32 performs local path-following and reflexive obstacle response between waypoint updates.

**FR-4 — Follow-me mode.** Robot autonomously tracks and follows a nearby moving target using an onboard single-point range sensor mounted on a continuously sweeping servo, assembled into a scan for leg/cluster-style detection (see §7 assumption), maintaining a standoff distance, without requiring a cloud round-trip for the tracking/following control loop itself.

**FR-5 — Payload pickup.** Operator (or an autonomous task step, in a later iteration) can trigger the electromagnet to pick up or release small magnetic payloads. Electromagnet control is a discrete on/off actuator command owned by the ESP32.

**FR-6 — Incline compensation.** The ESP32 autonomously detects increased motor load consistent with an incline (via current/voltage sensing used to infer back-EMF behavior — see §7) and closed-loop reallocates drive power to maintain commanded velocity, without per-terrain manual tuning.

**FR-7 — Mapping.** EC2 builds and maintains an occupancy/pose-graph map via SLAM Toolbox from range-scan + odometry data relayed by the phone. The scan itself is assembled on the ESP32 from a single-point range sensor mechanically swept by a servo (see §4/§7), not a spinning 2D lidar. The map is available to the GUI for operator situational awareness and to Nav2/D*-Lite for planning.

**FR-8 — Mode switching safety.** Switching modes at any time is safe: in-flight autonomous waypoints or follow-me target locks are discarded, not carried across a mode boundary (see §5).

**FR-9 — Comms-loss fail-safe.** Loss of ESP32↔phone or phone↔EC2 communication does not result in unsafe motion. See §5, §8.

---

## 2. Non-Functional Requirements

| ID | Requirement | Notes |
|---|---|---|
| NFR-1 | ESP32 control loop rate | `[ASSUMPTION]` 100–200 Hz for motor PID / current sampling. Actual achievable rate depends on `[TBD: ESP32 variant]` and sensor bus speeds. |
| NFR-2 | Safety reaction latency | Fault → SAFE_HOLD entry in `[ASSUMPTION] <100 ms`, computed entirely on ESP32, independent of phone or cloud availability. |
| NFR-3 | ESP32↔phone latency | `[ASSUMPTION] <50 ms` typical, over the ESP32-hosted local Wi-Fi link (see §7). `[VERIFY]` more strongly than a wired link would need — RF conditions (distance, interference, contention) vary in a way a USB tether doesn't; measure on real hardware in the intended operating environment before trusting this budget. |
| NFR-4 | Phone↔EC2 latency | Not bounded tightly — variable over cellular/Wi-Fi, budgeted 100 ms–2000+ ms `[VERIFY: measure once real network path exists]`. Explicitly outside the safety path (NFR-2 must not depend on this). |
| NFR-5 | Local-first parity | Every subsystem exercised end-to-end on the local dev machine before any AWS resource is created, using identical interfaces/message contracts (hard constraint from project brief). |
| NFR-6 | Availability under cloud loss | Robot continues safe local operation (last waypoint plan, or SAFE_HOLD if plan is stale) when EC2 is unreachable. Never assumes cloud connectivity for basic safety. |
| NFR-7 | Cost control | No AWS resource runs unattended without a budget alarm; large/GPU instances are not left running idle (see plan.md 2b). |
| NFR-8 | Observability | Every subsystem emits enough telemetry (timestamps, sequence numbers, fault flags) to reconstruct a post-hoc timeline of a mode switch or fault. |

---

## 3. System Boundary Diagram

```
                       (wireless — ESP32 hosts its own Wi-Fi AP, see §7 ASSUMPTION)
   ┌─────────────────┐   Wi-Fi, UDP framed JSON   ┌──────────────────────┐
   │      ESP32       │◄──────────────────────────►│   Android Phone      │
   │   (on-robot)      │                            │  (handheld bridge   │
   │                    │                            │   + GUI)             │
   │ - motor PID         │                            │                      │
   │ - IMU (I2C)          │                            │  - GUI (Manual /     │
   │ - GPS (UART)          │                            │    Autonomous /      │
   │ - curr/volt sensing    │                            │    Follow-me)        │
   │ - range sensor +         │                            │  - relays telemetry  │
   │   sweep servo (scan)      │                            │  - relays scan +     │
   │ - electromag driver         │                          │    commands          │
   │ - SAFE_HOLD interlock         │                        └──────────┬───────────┘
   │ - Wi-Fi AP host                 │                                  │
   └─────────────────┘                                        MQTT/TLS │
                                                     (cellular or Wi-Fi) │
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
- **ESP32**: real-time control, real-time safety, local reflex behavior, *and* now the range-sensing hardware (single-point sensor + sweep servo, see §4/§7) since the phone is no longer robot-mounted. Nothing on this side of the boundary may block on phone or cloud availability.
- **Phone**: handheld bridge + GUI. No control authority of its own beyond forwarding operator intent and a local e-stop button press; not a safety-critical compute node (see Warnings in plan.md 2c for the implication of a phone-software crash). No longer hosts any sensor directly — it relays scan data the ESP32 assembles, same as it relays telemetry.
- **EC2**: heavy math only — SLAM, global planning. Never in the real-time safety path.

---

## 4. Sensor List

| Sensor | Purpose | Attaches to | Status |
|---|---|---|---|
| IMU | Orientation, pitch/roll for incline detection, gyro for local dead-reckoning | ESP32 (I2C/SPI) | `[TBD: exact model — e.g. 6-DoF vs 9-DoF class, needs to be decided before driver code is written]` |
| GPS | Coarse global position (outdoor), goal-reachability context | ESP32 (UART) | `[TBD: exact module, e.g. update rate / accuracy class]` |
| Range sensor (single-point, ToF or ultrasonic) | Point-distance reading; combined with the sweep servo below to build a SLAM scan input, obstacle detection, and Follow-me target tracking | ESP32 (UART/I2C) | `[TBD: ToF vs ultrasonic, exact model — range/accuracy/sample-rate class directly sets achievable scan rate, see §7]` |
| Sweep servo | Mechanically pans the range sensor to emulate a 2D scan (continuous back-and-forth sweep) | ESP32 (PWM) | `[TBD: exact servo model — slew speed is the other half of the achievable-scan-rate budget, see §7]` |
| Current sensor | Per-channel (left/right drive, 2x) current for stall detection and back-EMF-derived load inference — not per-wheel, see §6.1 note on `current_a` | ESP32 (I2C) | INA219, one per channel, distinct I2C addresses via A0/A1 |
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
| MANUAL | FOLLOW_ME | Operator selects Follow-me **and** range-sensor target-lock acquired within `[ASSUMPTION] 5 s` | N/A (entering) |
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

- **Transport**: `[ASSUMPTION]` Wi-Fi, with the ESP32 hosting its own access point and the phone connecting as a client station. Chosen over BLE for the bandwidth/latency headroom needed to relay scan data (see below) and over relying on external Wi-Fi infrastructure (no field router/hotspot is assumed to exist). This replaces the earlier wired-USB-serial design now that the phone is a handheld device carried by the operator rather than robot-mounted (see §7). Requires WPA2/WPA3 with a pre-shared key `[ASSUMPTION]` — unlike the old wired link, this is now an open-air RF interface with a real eavesdropping/injection surface; an open AP would let anyone in range spoof `mode_request`, `estop`, or `magnet_trigger` commands directly, upstream of whatever auth exists on the phone↔EC2 link (§6.2, `overview_cloud.md` §4).
- **Socket/framing**: `[ASSUMPTION]` UDP, one JSON message per datagram (no delimiter needed — UDP preserves message boundaries). Chosen over TCP because the existing `seq` numbers and mandatory heartbeat already give the protocol what it needs to detect loss/staleness, and UDP avoids head-of-line blocking from a stalled TCP retransmit sitting in front of a fresher, more useful packet on a lossy RF link. Keep individual payloads well under the practical Wi-Fi UDP MTU (`[ASSUMPTION] ~1200 bytes` to leave headroom) — this is why scan data is streamed as small per-sample messages (below) rather than batched into one large scan payload.
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
  **Note on `motors.*.current_a` granularity**: sensing is per-drive-channel (2x current sensor, left/right), not per-wheel. The chassis is skid-steer, so each *side*'s wheels are mechanically driven together — `fl`/`rl` (left side) report the identical value (same physical sensor, parallel-wired motor pair), and likewise `fr`/`rr` (right side). The schema keeps 4 wheel keys for GUI/telemetry symmetry, but only 2 independent current readings actually exist. Don't build UI or logic that expects `fl`/`rl` (or `fr`/`rr`) to diverge.
- **ESP32 → Phone (scan sample)**, new — one message per range reading as the sweep servo produces it:
  ```json
  {"seq": 1301, "ts_ms": 88213010, "msg_type": "scan_sample",
   "angle_deg": 42.5, "range_m": 1.83, "valid": true}
  ```
  Rate `[TBD/VERIFY]` — gated by (sensor sample time) × (servo slew speed), neither of which is chosen yet (see §4, §7). The phone accumulates a run of `scan_sample` messages into one sweep before relaying it upward as `robot/{id}/scan` (§6.2); no change to that topic's shape, just its input cadence/resolution, which will be well below a real spinning 2D lidar's.
- **Phone → ESP32 (commands)**, event-driven, plus mandatory heartbeat `[ASSUMPTION] ≥2 Hz`:
  ```json
  {"seq": 5567, "ts_ms": 88213050, "cmd_type": "waypoint_list",
   "payload": {"waypoints": [{"x":1.2,"y":0.4},{"x":2.0,"y":1.1}], "plan_id": "p-991"}}
  ```
  `cmd_type` ∈ `{mode_request, manual_drive, waypoint_list, estop, magnet_trigger, heartbeat}`.
- **Fail-safe**: no heartbeat within `[ASSUMPTION] 500 ms` → ESP32 enters `SAFE_HOLD` unilaterally. This is a hard requirement, not best-effort: the ESP32 must not need phone cooperation to detect phone-side failure. The operator walking out of the AP's Wi-Fi range now looks identical to any other heartbeat loss — no special-cased range detection is needed, it's just another cause of the same fault. Re-association after moving back into range does **not** auto-resume the prior mode, per §5 (`SAFE_HOLD` exits only to `MANUAL` via explicit operator action).
- **Latency budget**: NFR-3 (<50 ms typical, `[VERIFY]` on real hardware — see NFR-3 note in §2).
- **Who owns real-time safety**: ESP32, unconditionally.

### 6.2 Phone ↔ EC2

- **Transport**: `[ASSUMPTION]` MQTT over TLS. Chosen over raw WebSocket for built-in reconnect/QoS semantics and because the *same broker software* can run identically in the local dev sim (Mosquitto in Docker) and on EC2 (Mosquitto on the instance) — this is the mechanism that satisfies the "drop-in replacement" hard constraint: only the broker's hostname/port changes between local and cloud, not the protocol, topic structure, or payload schema.
- **Topics** (namespaced by robot id, `[ASSUMPTION]` single robot, id fixed for now):
  | Topic | Direction | Content | Rate |
  |---|---|---|---|
  | `robot/{id}/telemetry` | phone→cloud | aggregated ESP32 telemetry + confirmed mode | `[ASSUMPTION] 1–5 Hz` |
  | `robot/{id}/scan` | phone→cloud | assembled range-scan relay (phone batches ESP32 `scan_sample` messages, §6.1, into a `LaserScan`-shaped payload) | rate gated by sensor+servo sweep speed `[TBD/VERIFY]`, well below a spinning 2D lidar's |
  | `robot/{id}/odom` | phone→cloud | odometry estimate (IMU/wheel-derived) for SLAM input | `[ASSUMPTION] matches scan rate or faster` |
  | `robot/{id}/map` | cloud→phone | current map, for GUI display (not control-critical) | on-change, low rate |
  | `robot/{id}/waypoints` | cloud→phone→ESP32 | D*-Lite output path | event-driven, on (re)plan |
  | `robot/{id}/cmd` | phone→cloud | e.g. "compute path to goal (x,y)" | event-driven |
  | `robot/{id}/mode` | phone→cloud | mode changes, for cloud-side context (e.g. pause planning in Manual) | event-driven |
- **Latency budget**: NFR-4. Explicitly **not** part of the safety path — see §9 latency-budget argument (also elaborated in plan.md).
- **Who owns real-time safety**: neither side. This link only ever carries planning/mapping data and non-real-time telemetry.

### 6.3 [Removed] Phone ↔ Lidar (USB-OTG)

Superseded — the range sensor now lives on the ESP32 (single-point sensor + sweep servo, §4/§7), not on the phone via USB-OTG. Scan data instead flows ESP32 → Phone as `scan_sample` messages over the wireless link in §6.1, and Phone → EC2 unmodified in shape via §6.2's `robot/{id}/scan` topic. This heading number is kept as a pointer for readers of earlier revisions rather than reused for something unrelated.

---

## 7. Key Architecture Assumptions (flagged for early review)

These aren't in the original background and were decided here to make the spec concrete. None require new BOM beyond what's already listed; all are reversible if the user disagrees.

1. **Phone is a handheld device carried by the operator**, not mounted on the robot. This is why Follow-me needs to work via onboard sensing rather than "follow the phone's GPS" — that part of the original design is unchanged. It's also why the range-sensing hardware had to move onto the robot itself rather than staying on the phone (see assumption 2 and §4) — a USB-OTG-hosted sensor doesn't work if the host isn't on the robot.
2. **Follow-me uses single-point-range-sensor-on-a-sweep-servo-based leg/cluster tracking** (the sensor+servo assembling a scan, §4), not a phone-GPS-follow or camera-based approach (no camera is in the sensor list) and not a real spinning 2D lidar (dropped for cost — see BOM constraint noted in project updates). `[VERIFY]` — leg-detection for person-following from a *real* 2D lidar is an established technique, but this project's scan is far coarser and slower (mechanically swept, one point at a time) than what that technique was validated against; treat Follow-me's reliability, not just its existence, as unproven until tested in sim and on hardware — don't assume it inherits 2D-lidar-class robustness.
3. **Drive motors are assumed brushed DC gearmotors** (typical for hobby 4WD chassis), not BLDC. This matters because the back-EMF sensing method differs significantly between the two (§ incline compensation below). If the actual motors are BLDC, this section needs rework — flag before writing ESP32 motor-control firmware.
4. **ESP32 hosts its own Wi-Fi access point; the phone connects to it directly** as a client (§6.1), replacing the earlier wired-USB-serial design now that the phone is handheld (assumption 1). Chosen over BLE for the throughput/latency headroom scan relay needs, and over depending on external Wi-Fi infrastructure. Requires WPA2/WPA3 auth — see the security note in §6.1.
5. **Phone↔EC2 uses self-hosted MQTT (Mosquitto-class broker)**, not a managed service like AWS IoT Core, specifically so the local-sim and cloud endpoints are interface-identical (hard constraint). This is a deliberate simplicity/parity choice over IoT Core's extra managed features (device shadows, rules engine) which the project doesn't currently need.
6. **Operator range is bounded by the ESP32 AP's Wi-Fi range** (open-air, tens of meters typical, less through obstacles — `[VERIFY]` once an ESP32 variant/antenna is chosen). Walking out of range is not a distinct failure mode requiring new logic: it presents as heartbeat loss and is handled by the existing `SAFE_HOLD` path (§6.1, §5). This is a new hard operational constraint versus the old wired design (which had no meaningful range limit) and should be communicated to the operator via the GUI, not just silently discovered as a `SAFE_HOLD` event.

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
| ESP32↔Phone comms | Heartbeat loss detected within 500 ms (§6.1), triggers SAFE_HOLD reliably — tested by synthetically breaking the link in sim, and on hardware by both a hard link kill *and* physically walking the phone out of AP range/into RF interference (§7 assumption 6), since wireless failure modes are broader than a wired link's |
| Phone↔EC2 comms | Robot continues safe local operation (holds last valid plan, or SAFE_HOLD if plan is stale beyond `[ASSUMPTION] 10 s`) when cloud is unreachable; GUI clearly indicates "cloud offline" |
| SLAM (EC2, SLAM Toolbox) | Produces map + pose estimate at `[TBD/VERIFY: exact achievable rate depends on the chosen range sensor + sweep servo's combined scan rate, and instance sizing]` given scan+odom input — this rate is expected to be markedly lower than a spinning 2D lidar's, and SLAM Toolbox's tolerance for that needs empirical validation, not assumption |
| Global planning (D*-Lite via Nav2) | Replans within `[ASSUMPTION] 2 s` of new obstacle information reaching the map; replanning is rate-limited/hysteresis-bounded to avoid thrashing (see plan.md Warnings) |
| Mode state machine | Every transition in §5's table is exercised and passes in the local sim harness before any hardware bring-up |
| Follow-me | Maintains lock within `[ASSUMPTION] 0.5 m` of target — **flagged for revalidation**: this number was set against real-2D-lidar-class scan rate/resolution assumptions and may not be achievable with the coarser, slower sweep-servo sensor (§7 assumption 2); treat it as a target to test against, not a guarantee, and revise downward in this table once bench data exists. Must degrade to MANUAL safely (not a lunge or freeze) on lock loss regardless. |
| Electromagnet | Activates/deactivates within `[ASSUMPTION] 100 ms` of command; verified not to brown out the bus under worst-case (low battery + peak coil inrush) load test before it's ever tested on hardware carrying a real payload |

---

## 9. Latency-Budget Argument (summary)

The chain that must never depend on cloud round-trip time: **fault detection → SAFE_HOLD entry → motor command zeroing**. This entire chain is computed and executed on the ESP32 using only locally-available signals (current/voltage, IMU, heartbeat timers). The EC2 link only ever supplies a *waypoint list* or *map update* — inputs that are consumed opportunistically and cached; their absence degrades capability (no new plan) but never removes safety (the fault/SAFE_HOLD path doesn't reference them at all). This is why NFR-2 (<100 ms) and NFR-4 (100–2000+ ms, unbounded-ish) can coexist: they're different budgets for different responsibilities, not the same number measured twice. Moving the ESP32↔phone link from wired serial to Wi-Fi (§6.1, §7) doesn't change this argument's structure — the fault→SAFE_HOLD chain is still ESP32-local and still doesn't reference the phone or cloud link's *content*, only the presence/absence of a heartbeat. It does mean that link is now less deterministic than a USB tether was, which is precisely why NFR-3 is marked `[VERIFY]` rather than assumed, and why the heartbeat-timeout mechanism (not a wired link's inherent reliability) is what's actually load-bearing for safety here.

Full elaboration of this argument, plus the specific failure modes it protects against, is in `plan.md` under each phase's Warnings & Edge Cases section.
