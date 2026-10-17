# Spec — Ground Robot + Cloud Control System

## 0. Status of this document

This is a Phase 1 spec, written before any AWS resource exists and before BOM is finalized. Unknown hardware is marked `[TBD: ...]`. Any technical claim about AWS/ROS2/Nav2/SLAM Toolbox/D*-Lite behavior that isn't fully verified is marked `[VERIFY]` with a note on how to check it. Architecture choices not dictated by the background (e.g. which transport connects ESP32↔EC2) are marked `[ASSUMPTION]` and are load-bearing for later phases — if the user overrides one, downstream sections need re-checking.

---

## 1. Functional Requirements

**FR-1 — Three operating modes.** The system supports exactly three operating modes: Manual, Autonomous, Follow-me. No other mode exists as a peer to these (a safety interlock state, SAFE_HOLD, exists but is cross-cutting, not a fourth mode — see §5).

**FR-2 — Manual mode.** Operator drives the robot directly via browser dashboard controls (virtual joystick, `cloud/dashboard/`). Commands pass dashboard → EC2 broker → ESP32 (§6.1) — unlike the earlier phone-bridge design, this path now **does** cross the cloud broker; §9 explains why that's still an acceptable trade (the watchdog-based fail-safe doesn't depend on the broker's cooperation).

**FR-3 — Autonomous mode.** Operator specifies a goal (via GUI, e.g. tap-on-map or coordinate entry). EC2 (SLAM Toolbox + Nav2 + D*-Lite global planner) computes a global path against the current map and streams waypoints down to the ESP32. ESP32 performs local path-following and reflexive obstacle response between waypoint updates.

**FR-4 — Follow-me mode.** Robot autonomously tracks and follows a nearby moving target using an onboard single-point range sensor mounted on a continuously sweeping servo, assembled into a scan for leg/cluster-style detection (see §7 assumption), maintaining a standoff distance, without requiring a cloud round-trip for the tracking/following control loop itself.

**FR-5 — Payload pickup.** Operator (or an autonomous task step, in a later iteration) can trigger the electromagnet to pick up or release small magnetic payloads. Electromagnet control is a discrete on/off actuator command owned by the ESP32.

**FR-6 — Incline compensation.** The ESP32 autonomously detects increased motor load consistent with an incline (via current/voltage sensing used to infer back-EMF behavior — see §7) and closed-loop reallocates drive power to maintain commanded velocity, without per-terrain manual tuning.

**FR-7 — Mapping.** EC2 builds and maintains an occupancy/pose-graph map via SLAM Toolbox from range-scan + odometry data published directly by the ESP32 (no phone relay — see §6.1; not yet implemented, see that section's status table). The scan itself is assembled from a single-point range sensor mechanically swept by a servo (see §4/§7), not a spinning 2D lidar. The map is available to the dashboard for operator situational awareness and to Nav2/D*-Lite for planning.

**FR-8 — Mode switching safety.** Switching modes at any time is safe: in-flight autonomous waypoints or follow-me target locks are discarded, not carried across a mode boundary (see §5).

**FR-9 — Comms-loss fail-safe.** Loss of the ESP32↔EC2 MQTT link does not result in unsafe motion (no phone-relay hop exists anymore — see §6.1). See §5, §8.

---

## 2. Non-Functional Requirements

| ID | Requirement | Notes |
|---|---|---|
| NFR-1 | ESP32 control loop rate | `[ASSUMPTION]` 100–200 Hz for motor PID / current sampling. Actual achievable rate depends on `[TBD: ESP32 variant]` and sensor bus speeds. |
| NFR-2 | Safety reaction latency | Fault → SAFE_HOLD entry in `[ASSUMPTION] <100 ms`, computed entirely on ESP32, independent of cloud availability. |
| NFR-3 | `[REMOVED]` ESP32↔phone latency | Superseded — no ESP32↔phone link exists anymore (`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`). The ESP32's only wireless hop now is ESP32↔EC2, covered by NFR-4. Row number kept as a pointer for readers of earlier revisions, same convention as §6.3. |
| NFR-4 | ESP32↔EC2 latency | Not bounded tightly — variable over Wi-Fi (the ESP32 itself has no cellular radio in the current sensor list, §4/§7 — outdoor range is now bounded by reachable Wi-Fi, e.g. a mobile hotspot near the robot, until that's revisited), budgeted 100 ms–2000+ ms `[VERIFY: measure once real network path exists]`. Explicitly outside the safety path (NFR-2 must not depend on this) — **except** manual-drive commands (FR-2) now ride this same link, which they didn't in the old phone-bridge design; see §9. |
| NFR-5 | Local-first parity | Every subsystem exercised end-to-end on the local dev machine before any AWS resource is created, using identical interfaces/message contracts (hard constraint from project brief). |
| NFR-6 | Availability under cloud loss | Robot continues safe local operation (last waypoint plan, or SAFE_HOLD if plan is stale) when EC2 is unreachable. Never assumes cloud connectivity for basic safety. |
| NFR-7 | Cost control | No AWS resource runs unattended without a budget alarm; large/GPU instances are not left running idle (see plan.md 2b). |
| NFR-8 | Observability | Every subsystem emits enough telemetry (timestamps, sequence numbers, fault flags) to reconstruct a post-hoc timeline of a mode switch or fault. |

---

## 3. System Boundary Diagram

```
   ┌─────────────────────┐
   │        ESP32         │
   │      (on-robot)       │
   │                        │
   │ - motor PID             │
   │ - IMU (I2C)              │
   │ - GPS (UART)              │
   │ - curr/volt sensing        │
   │ - range sensor +             │
   │   sweep servo (scan)          │
   │ - electromag driver             │
   │ - SAFE_HOLD interlock             │
   │ - Wi-Fi STATION (joins local Wi-Fi) │
   └─────────────────┬────────────────┘
                      │  MQTT/TLS, outbound-only from the ESP32
                      │  (Wi-Fi; see §7 assumption 6 on the cellular-
                      │   mobility consequence of dropping the phone bridge)
                      ▼
          ┌──────────────────────────────────┐
          │       EC2 (Linux instance)        │
          │                                    │
          │  - Mosquitto broker (TLS + auth)    │
          │  - nginx (serves browser dashboard)  │
          │  - SLAM Toolbox                       │
          │  - Nav2                                │
          │  - D*-Lite global planner loop          │
          │  - map store                              │
          └───────────────────┬──────────────────────┘
                               │  HTTPS/WSS
                               ▼
               Any phone/tablet/laptop (browser dashboard,
               anywhere with internet — no longer a bridge
               device, just a viewer/controller of EC2)
```

Boundary ownership:
- **ESP32**: real-time control, real-time safety, local reflex behavior, and the range-sensing hardware (single-point sensor + sweep servo, see §4/§7). Nothing on this side of the boundary may block on cloud availability (no phone hop exists to block on either).
- **Browser dashboard** (replaces the Android-app-on-a-phone-bridge role entirely — `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`): runs in any browser, served from EC2. No control authority of its own beyond forwarding operator intent; not a safety-critical compute node, and not in the data path between ESP32 and EC2 at all — it's a client of the same MQTT broker the ESP32 talks to, not a relay sitting between them.
- **EC2**: heavy math (SLAM, global planning) plus now hosting the MQTT broker and serving the browser dashboard's static files (design doc §3). Never in the real-time safety path regardless.

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

**Authority**: the ESP32 is the single source of truth for the *current confirmed mode*. The browser dashboard only *requests* a mode; the ESP32 accepts or rejects the request and reports the confirmed mode back. This follows directly from "ESP32 owns real-time safety" — mode arbitration is a safety-relevant decision (e.g. don't enter Autonomous without a valid plan) and must not depend on the dashboard or cloud being correct.

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
| any mode | SAFE_HOLD | Overcurrent/stall fault, IMU fault, e-stop command, and (`[TBD]`, see note below) loss of ESP32↔EC2 heartbeat once that mechanism exists | Motors bounded-stop (or immediate cutoff on overcurrent trip); queued waypoints/target-lock discarded; electromagnet state **latched**, not auto-dropped, unless brownout-protection logic requires load shedding (see plan.md Warnings) |
| SAFE_HOLD | MANUAL | Explicit operator acknowledgment/reset action only | Starts from zero command |

Rejected mode-change requests return a reason code to the dashboard (e.g. `NO_VALID_PLAN`, `NO_TARGET_LOCK`, `ACTIVE_FAULT`) so it can explain the refusal rather than silently ignoring it.

**Note on ESP32↔EC2 loss, SAFE_HOLD, and NFR-6 (added 2026-10-06, fixing a contradiction introduced by the §6.1/§6.2 merge)**: the row above's "loss of ESP32↔EC2 heartbeat → SAFE_HOLD" is a *different* fault condition from NFR-6/§8's "robot continues safe local operation (holds last valid plan) when EC2 is unreachable" — they are not in conflict once treated as two separate signals, but nothing currently implemented distinguishes them, which is why this needs flagging rather than silently picking one. The old (pre-merge) design had two links with two policies: ESP32↔Phone heartbeat loss → SAFE_HOLD (tight, safety-critical), Phone↔EC2 loss → degrade-but-continue (loose, non-safety-critical). Collapsing both into one ESP32↔EC2 link means a single link's loss could plausibly map to either policy, and the right answer likely depends on *what* is lost — total communication loss (no heartbeat at all) plausibly still warrants SAFE_HOLD, while *planning data specifically* going stale (NFR-6's actual concern) plausibly doesn't need to interrupt an already-good last plan. Resolving this requires defining the heartbeat mechanism §6.1's Fail-safe bullet flags as `[TBD]` first. Not resolved here — flagged in `UPDATES.md` for whoever builds SAFE_HOLD/the heartbeat mechanism on the ESP32.

---

## 6. Interface Contracts

### 6.1 ESP32 ↔ EC2

Single hop now — this section replaces the former two-hop ESP32↔Phone (UDP)
+ Phone↔EC2 (MQTT) design (`docs/superpowers/specs/2026-10-04-cloud-web-
dashboard-design.md`). The Android-app-as-bridge role is cancelled; any
browser (phone, tablet, laptop) can run the dashboard, but it is a client of
the same broker the ESP32 talks to, not an intermediary in this contract.

- **Transport**: `[ASSUMPTION]` MQTT over TLS, ESP32 as an outbound-only
  client (never accepts inbound connections) — chosen (design D1/D2) over
  keeping the ESP32's own Wi-Fi AP and tunneling it, specifically to reuse
  the broker's TLS+auth instead of exposing a second, separately-secured
  surface. Same broker software locally (Mosquitto in Docker) and on EC2
  (Mosquitto on the instance) — only the hostname/port/credentials change,
  not the protocol or topic/payload shapes (same "drop-in replacement" hard
  constraint as before).
- **Topics** (namespaced by robot id, `[ASSUMPTION]` single robot, id fixed
  for now):

  | Topic | Direction | Content | Rate | Status |
  |---|---|---|---|---|
  | `robot/{id}/drive` | dashboard→ESP32 | pre-mixed duty, `{"left": -255, "right": 255}` | event-driven, keepalive while held | **Implemented** (`embedded/web_drive/`, `cloud/dashboard/`) |
  | `robot/{id}/telemetry` | ESP32→dashboard/cloud | `{"left_a":0.42,"right_a":0.39,"left_duty":180,"right_duty":178,"vbus":12.1,"magnet":false}` (`magnet` additive 2026-10-17) | `[ASSUMPTION] 5 Hz` | **Implemented** — reduced shape (drive bring-up only); the full target shape below is still `[TBD]`, not built |
  | `robot/{id}/scan` | ESP32→cloud | range-scan, `LaserScan`-shaped | rate gated by sensor+servo sweep speed `[TBD/VERIFY]` | **Not implemented.** `[TBD]` — previously assembled by the phone from per-sample messages; with no phone, whether the ESP32 assembles a full scan itself before publishing, or publishes raw samples for EC2 to assemble, is an open design question, not decided by the 2026-10-04 design |
  | `robot/{id}/odom` | ESP32→cloud | odometry estimate (IMU/wheel-derived) | `[ASSUMPTION] matches scan rate or faster` | Not implemented |
  | `robot/{id}/magnet` | dashboard→ESP32 | explicit on/off, `{"on": true}` / `{"on": false}` (never a toggle); QoS 1; malformed payloads are ignored and leave the state unchanged. State is **latched** (§5) — the drive watchdog, stop, and link loss never change it; ESP32 boots with it OFF. The confirmed state is reported back as the additive telemetry field `"magnet": true/false` | event-driven | **Implemented** (`embedded/web_drive/magnet.h` + `mqtt_handler.h`, GPIO23 relay, wired to D23; `cloud/dashboard/` button) |
  | `robot/{id}/map` | cloud→dashboard | current map, GUI display | on-change, low rate | Not implemented |
  | `robot/{id}/waypoints` | cloud→ESP32 | D*-Lite output path | event-driven, on (re)plan | Not implemented |
  | `robot/{id}/cmd` | dashboard→cloud | e.g. "compute path to goal (x,y)" | event-driven | Not implemented |
  | `robot/{id}/mode` | ESP32→cloud, dashboard→ESP32 (request) | mode changes/requests | event-driven | Not implemented — mode state machine (§5) doesn't exist on the ESP32 yet |

  **Target telemetry shape** (not yet implemented — current implementation
  is the reduced `robot/{id}/telemetry` row above):
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
  **Note on `motors.*.current_a` granularity** (unchanged from before):
  sensing is per-drive-channel (2x current sensor, left/right), not
  per-wheel — `fl`/`rl` (left side) always report the identical value, and
  likewise `fr`/`rr` (right side).
- **Commands** (target shape, not yet implemented beyond `drive`):
  ```json
  {"seq": 5567, "ts_ms": 88213050, "cmd_type": "waypoint_list",
   "payload": {"waypoints": [{"x":1.2,"y":0.4},{"x":2.0,"y":1.1}], "plan_id": "p-991"}}
  ```
  `cmd_type` ∈ `{mode_request, manual_drive, waypoint_list, estop, magnet_trigger, heartbeat}`.
  Today's implemented `robot/{id}/drive` message is a simplified stand-in
  for `manual_drive` — `[TBD]` whether it's folded into this `cmd_type`
  envelope later or stays its own topic.
- **Fail-safe**: no valid `drive` message within the ESP32's watchdog window
  (`[ASSUMPTION] 1500 ms`, design D6 — raised from the old ESP32↔phone
  link's 500ms to tolerate cloud-relay latency/jitter) → ESP32 forces
  `drive(0,0)` unilaterally. This doesn't need the broker's cooperation: a
  dead broker connection looks identical to "no command arrived." The full
  mode-machine version of this (SAFE_HOLD, §5) isn't implemented yet —
  today's watchdog only zeroes drive output, it doesn't have a mode to
  transition. **This is a drive-command watchdog, not a general heartbeat**:
  the old §6.1's mandatory `[ASSUMPTION] ≥2 Hz` heartbeat with its own
  500ms timeout was dropped in the §6.1/§6.2 merge and not redefined here —
  `heartbeat` is still listed as a `cmd_type` above but its rate/timeout are
  `[TBD]`. This matters because it's only the drive watchdog, not a
  heartbeat, that's implemented today — see the note under §5's transition
  table for why that distinction is load-bearing once SAFE_HOLD exists.
- **Latency budget**: NFR-4 (renamed from "Phone↔EC2" — see NFR table).
  Manual-mode drive commands now cross this budget too (they didn't in the
  old phone-bridge design, where phone→ESP32 was a separate, tighter-budget
  local link) — see §9 for why this is still an acceptable trade, and the
  design doc's §5 for the full safety argument.
- **Who owns real-time safety**: ESP32, unconditionally — same as before.

### 6.2 [Removed] Phone ↔ EC2

Superseded — merged into §6.1 above now that the ESP32 talks to EC2
directly. Heading number kept as a pointer for readers of earlier
revisions, same convention as §6.3 below.

### 6.3 [Removed] Phone ↔ Lidar (USB-OTG)

Superseded — the range sensor now lives on the ESP32 (single-point sensor + sweep servo, §4/§7), not on the phone via USB-OTG. Scan data flows directly ESP32 → EC2 via §6.1's `robot/{id}/scan` topic (not yet implemented, see §6.1's status table). This heading number is kept as a pointer for readers of earlier revisions rather than reused for something unrelated.

---

## 7. Key Architecture Assumptions (flagged for early review)

These aren't in the original background and were decided here to make the spec concrete. None require new BOM beyond what's already listed; all are reversible if the user disagrees.

1. **No phone-bridge device exists in this architecture.** The browser dashboard (any phone/tablet/laptop with a browser) talks to the ESP32 only indirectly, via the EC2 MQTT broker (§6.1), never a direct local link to the robot — superseding the earlier "phone is a handheld bridge" assumption entirely (`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`). The range-sensing hardware still lives on the ESP32, not on any operator device — that placement decision doesn't depend on whether a phone bridge exists, only on the sensor needing to be robot-mounted to be useful for Follow-me/SLAM.
2. **Follow-me uses single-point-range-sensor-on-a-sweep-servo-based leg/cluster tracking** (the sensor+servo assembling a scan, §4), not a phone-GPS-follow or camera-based approach (no camera is in the sensor list) and not a real spinning 2D lidar (dropped for cost — see BOM constraint noted in project updates). `[VERIFY]` — leg-detection for person-following from a *real* 2D lidar is an established technique, but this project's scan is far coarser and slower (mechanically swept, one point at a time) than what that technique was validated against; treat Follow-me's reliability, not just its existence, as unproven until tested in sim and on hardware — don't assume it inherits 2D-lidar-class robustness.
3. **Drive motors are assumed brushed DC gearmotors** (typical for hobby 4WD chassis), not BLDC. This matters because the back-EMF sensing method differs significantly between the two (§ incline compensation below). If the actual motors are BLDC, this section needs rework — flag before writing ESP32 motor-control firmware.
4. **ESP32 is a Wi-Fi *station* (not an access point)**, joining whatever Wi-Fi network has internet access on-site, and connects outbound-only to the EC2 MQTT broker over TLS (§6.1) — replacing the earlier "ESP32 hosts its own AP, phone connects to it" design now that there's no phone to connect. Chosen (design D1/D2) over keeping the AP and tunneling it, specifically to reuse the broker's TLS+auth instead of exposing a second, weaker-auth surface.
5. **ESP32↔EC2 uses self-hosted MQTT (Mosquitto-class broker)**, TLS + username/password auth (design D4/D5), not a managed service like AWS IoT Core — same broker software and topic/payload-contract parity argument as before, just one hop instead of two (no phone relay in between).
6. **Operator/robot range is now bounded by Wi-Fi availability, not a dedicated AP's range.** Since the ESP32 is a Wi-Fi station rather than an AP, the robot must be within range of *some* Wi-Fi network with internet access (home Wi-Fi, a mobile hotspot carried near the robot, etc.). **New consequence not present in the phone-bridge design**: the ESP32 itself has no cellular radio in the current sensor list (§4, `ESP32 variant` still `[TBD]`) — outdoor/away-from-fixed-Wi-Fi operation now requires an external Wi-Fi hotspot reachable by the robot, or a cellular-capable ESP32 variant/add-on not currently planned. The old design's cellular-mobility assumption (`plan.md` 2c.2, written when the *phone* carried the cellular radio) needs re-review in light of this — flagged in `UPDATES.md` (this plan's Task 7), not resolved here. Losing Wi-Fi range still presents as heartbeat/link loss and is handled by the existing fail-safe path (§6.1, §5).

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
| ESP32↔EC2 comms | No valid `drive` message within 1500 ms (§6.1) forces motors to zero, reliably — tested by synthetically breaking the link against the local Mosquitto rehearsal stack (`cloud/docker-compose.yml`), and (once hardware exists) on real hardware by killing Wi-Fi, killing the broker, and an artificially throttled/delayed link (design doc §8). Once the full mode machine exists: robot continues safe local operation (holds last valid plan, or SAFE_HOLD if plan is stale beyond `[ASSUMPTION] 10 s`) when cloud is unreachable; dashboard clearly indicates "link offline." |
| SLAM (EC2, SLAM Toolbox) | Produces map + pose estimate at `[TBD/VERIFY: exact achievable rate depends on the chosen range sensor + sweep servo's combined scan rate, and instance sizing]` given scan+odom input — this rate is expected to be markedly lower than a spinning 2D lidar's, and SLAM Toolbox's tolerance for that needs empirical validation, not assumption |
| Global planning (D*-Lite via Nav2) | Replans within `[ASSUMPTION] 2 s` of new obstacle information reaching the map; replanning is rate-limited/hysteresis-bounded to avoid thrashing (see plan.md Warnings) |
| Mode state machine | Every transition in §5's table is exercised and passes in the local sim harness before any hardware bring-up |
| Follow-me | Maintains lock within `[ASSUMPTION] 0.5 m` of target — **flagged for revalidation**: this number was set against real-2D-lidar-class scan rate/resolution assumptions and may not be achievable with the coarser, slower sweep-servo sensor (§7 assumption 2); treat it as a target to test against, not a guarantee, and revise downward in this table once bench data exists. Must degrade to MANUAL safely (not a lunge or freeze) on lock loss regardless. |
| Electromagnet | Activates/deactivates within `[ASSUMPTION] 100 ms` of command; verified not to brown out the bus under worst-case (low battery + peak coil inrush) load test before it's ever tested on hardware carrying a real payload |

---

## 9. Latency-Budget Argument (summary)

The chain that must never depend on cloud round-trip time: **fault detection → SAFE_HOLD entry → motor command zeroing**. This entire chain is computed and executed on the ESP32 using only locally-available signals (current/voltage, IMU, heartbeat timers). The EC2 link only ever supplies a *waypoint list* or *map update* — inputs that are consumed opportunistically and cached; their absence degrades capability (no new plan) but never removes safety (the fault/SAFE_HOLD path doesn't reference them at all). This is why NFR-2 (<100 ms) and NFR-4 (100–2000+ ms, unbounded-ish) can coexist: they're different budgets for different responsibilities, not the same number measured twice. Collapsing the ESP32↔phone + phone↔EC2 two-hop design into a single ESP32↔EC2 MQTT link (§6.1, `docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`) doesn't change this argument's structure — the fault→SAFE_HOLD chain is still ESP32-local and still doesn't reference the cloud link's *content*, only the presence/absence of a valid command within the watchdog window. It does mean manual-drive commands themselves now cross the same link whose latency/availability was previously isolated from them (the old phone→ESP32 hop was a separate, tighter-budget local link) — this is the one genuine safety-margin trade-off of the new design, discussed in full in the design doc's §5. NFR-4 (renamed from "Phone↔EC2") is what's now load-bearing for the drive-command path's responsiveness, not just planning/mapping data.

Full elaboration of this argument, plus the specific failure modes it protects against, is in `plan.md` under each phase's Warnings & Edge Cases section.
