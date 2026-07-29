# Plan — Ground Robot + Cloud Control System

This plan is sequenced 2a → 2b → 2c on purpose. 2b (AWS) must not start before 2a exists and passes its own acceptance criteria — this is the project's hard constraint, not a suggestion. 2c only swaps endpoints; it introduces no new logic.

---

## 2a. Local-First Environment

### Goal

Exercise the entire pipeline — mode switching, global planning (SLAM Toolbox + Nav2 + D*-Lite), local reflex control, incline compensation — on the local Linux machine, with no hardware and no AWS account, using the *exact* interfaces defined in `spec.md` §6, so that swapping in real hardware/EC2 later is an endpoint change, not a rewrite.

### 2a.1 Toolchain

| Component | Choice | Confidence |
|---|---|---|
| ROS2 distro | `[ASSUMPTION]` ROS2 Humble Hawksbill (LTS) | `[VERIFY]` confirm this is still the recommended LTS paired with the current Nav2/SLAM Toolbox release at implementation time — ROS2 LTS support windows and the Nav2/Gazebo/distro compatibility matrix shift over time; check `docs.nav2.org` and `github.com/SteveMacenski/slam_toolbox` compatibility notes before pinning versions in the Dockerfile. |
| Simulator | Gazebo (the modern "Gazebo" formerly branded Ignition, *not* Gazebo Classic) | `[VERIFY]` exact package/version name paired with the chosen ROS2 distro — the Gazebo project has renamed/re-versioned multiple times and Classic is in maintenance-only mode; confirm current name (e.g. "Gazebo Harmonic" or successor) before writing launch files. |
| SLAM | `slam_toolbox` ROS2 package | `[VERIFY]` package name/API stable across recent releases, but confirm against chosen distro |
| Global/local planning framework | `nav2_bringup`, `nav2_controller`, `nav2_planner` stack | `[VERIFY]` node/package names below |
| Global planner | Custom D*-Lite (see 2a.3) | not a stock Nav2 planner — see below |
| Message broker (local stand-in for EC2's broker) | Mosquitto (MQTT), Dockerized | mirrors `spec.md` §6.2 exactly |
| Containerization | Docker + docker-compose, one service per major node group (sim, SLAM+Nav2+planner, broker, simulated-ESP32, simulated-phone) | keeps the local stack close to how the EC2 side will likely be composed |
| E2E test harness | `launch_testing` (ROS2) or plain pytest driving the compose stack | validates the mode-transition table and fault-injection scenarios below |

### 2a.2 Simulating the sensor suite

None of these sensors get invented hardware — the simulation only needs to produce the *same message shapes* the real sensors will produce, per `spec.md` §4/§6.

- **Lidar**: Gazebo's built-in 2D lidar sensor plugin, publishing `sensor_msgs/LaserScan` on a topic that maps to `spec.md`'s `robot/{id}/scan` payload once relayed through the simulated-phone node.
- **IMU**: Gazebo's IMU sensor plugin for baseline orientation, *plus* a synthetic override node so incline-compensation tests can inject a specific pitch value directly (rather than only relying on Gazebo's terrain physics being perfectly tuned) — this decoupling matters because the incline controller needs to be testable independent of Gazebo terrain-modeling fidelity.
- **GPS**: Gazebo GPS plugin for the nominal case, plus a synthetic fault-injection node capable of simulating dropout (no fix) and multipath-style position jumps, specifically to exercise the GPS-dropout Warning below.
- **Current/voltage (back-EMF proxy)**: **no standard Gazebo plugin exists for this** — it's domain-specific to this project. Build a small synthetic node that models motor current as a function of (a) commanded duty cycle, (b) simulated terrain grade (read from Gazebo world elevation under the robot, or injected directly for controller unit tests), and (c) injected PWM-switching noise (see Warnings) so the incline-compensation controller and its noise filter can both be validated before any real current sensor exists.

### 2a.3 The ESP32 control loop, simulated

The important design decision here: **write the control-loop logic once, behind a hardware abstraction layer (HAL), and run it two ways** — natively as a Linux process/ROS2 node now, and later compiled for ESP-IDF/Arduino on real hardware. The mode state machine, incline-compensation controller, and fault/SAFE_HOLD logic from `spec.md` §5–§7 live in this portable core; only the HAL (I2C/UART/PWM drivers vs. simulated topic I/O) differs between the two builds. This is the same "drop-in replacement" principle the project applies to EC2, pushed down one more level — without it, the local sim would validate a *different* control loop than what eventually ships to hardware, defeating the point of simulating first.

The simulated-phone node implements the same JSON-over-serial contract from `spec.md` §6.1, but instead of a real UART, use either (a) a `socat`-created virtual serial pair if the ESP32-core build is compiled to talk to an actual serial device even in sim, or (b) a plain ROS2 topic carrying the same JSON payloads as `std_msgs/String`, which is simpler to wire up and sufficient for validating message-contract correctness and control logic (it does not validate real UART throughput/timing, which is acceptable to defer to 2c hardware bring-up).

### 2a.4 D*-Lite in the Nav2 loop — design decision

Nav2 supports pluggable global planners via a `nav2_core`-defined interface loaded through `pluginlib` `[VERIFY: confirm exact base class name and required method signatures against the chosen distro's Nav2 docs — this is exactly the kind of API-shape claim that drifts across releases]`. D*-Lite is **not** a planner shipped in Nav2's default planner set (which centers on Dijkstra/A*-family planners like `nav2_navfn_planner` and `nav2_smac_planner`) `[VERIFY — confirm no existing maintained D*-Lite Nav2 plugin exists before building one from scratch, to avoid reinventing it]`.

Two viable integration shapes, in order of recommended build sequence:

1. **Standalone D*-Lite node (recommended starting point)**: runs as its own ROS2 node, subscribes to the SLAM Toolbox map + goal, publishes a `nav_msgs/Path`. Nav2's controller server (local trajectory follower) is pointed at this externally-published path instead of going through Nav2's own planner server. Simpler to build and debug in isolation; loses some of Nav2's built-in planner-lifecycle management.
2. **True Nav2 global-planner plugin (stretch goal)**: wraps the same D*-Lite core in the `nav2_core` planner interface so it participates in Nav2's standard `compute_path_to_pose` action and lifecycle. More idiomatic, more integration work, higher API-drift risk given `[VERIFY]` above.

Start with (1), prove correctness and replan behavior against the acceptance criteria in `spec.md` §8, then migrate to (2) only if Nav2's native lifecycle/recovery behaviors are needed.

### 2a.5 End-to-end exercises (what "done" looks like for 2a)

Bring up the full docker-compose stack and run, at minimum:

1. **Manual teleop** — phone-sim → ESP32-sim → simulated motor commands → Gazebo robot moves as commanded.
2. **Autonomous goal** — set a goal, confirm SLAM Toolbox map exists, confirm D*-Lite path is computed and streamed down, confirm ESP32-sim follows waypoints and requests a replan when a synthetic obstacle is injected into the map.
3. **Follow-me** — synthetic moving target in Gazebo, confirm lidar-based lock acquisition/tracking/standoff distance per `spec.md` FR-4, and confirm safe fallback to MANUAL on lock loss.
4. **Incline compensation** — inject a grade (via Gazebo terrain or direct synthetic override), confirm the current-rise-based controller boosts power proportionally and never exceeds the modeled max-current limit.
5. **Full mode-transition matrix** — mechanically exercise every row of `spec.md` §5's transition table, asserting the in-flight-command-handling behavior specified for each.
6. **Fault injection** — kill the simulated-phone process (ESP32↔phone comms loss), kill the Mosquitto broker (phone↔cloud loss), and confirm the system degrades exactly as `spec.md` NFR-2/NFR-6 require: SAFE_HOLD on the former, continued local operation on last-known-plan (or SAFE_HOLD if stale) on the latter — **crucially, confirm the fault-detection-to-SAFE_HOLD path still works with the entire cloud stack killed**, which is the concrete test of the latency-budget argument in `spec.md` §9.

### 2a.6 Warnings & Edge Cases

- **GPS dropout/multipath (indoors, near structures)**: simulate via the synthetic GPS-fault node (2a.2) injecting no-fix periods and position jumps. The control loop must not treat a GPS jump as real motion — validate that IMU/odometry-based dead-reckoning is weighted appropriately (or GPS is simply de-weighted/ignored) during a fix-quality drop, and that Autonomous mode doesn't accept a corrupted pose as a valid state to plan from. This is a design point to nail down now: confirm the sensor-fusion approach doesn't blindly trust GPS `[TBD: exact fusion method — see overview_controls.md]`.
- **Lidar occlusion by the robot's own chassis or a carried payload**: define, in sim, a "self-occlusion" mask for known blind angles (e.g. directly behind if the electromagnet arm blocks part of the FOV) so SLAM/obstacle-detection code doesn't misinterpret a self-occlusion as a real nearby obstacle. Validate this mask against the actual lidar mounting position once chosen `[TBD]`.
- **ESP32↔phone comms loss and fail-safe**: validated directly in 2a.5 exercise 6. The important invariant: the ESP32-sim (and later, real ESP32) must detect *absence* of a heartbeat itself — it cannot rely on the phone sending an explicit "I'm about to disconnect" message, since real failures (crash, app killed, phone reboot) don't announce themselves.
- **Back-EMF sensing noise from PWM switching**: the synthetic current/voltage node (2a.2) should inject realistic PWM-frequency-correlated noise, not just clean signal, so the filter design (e.g. sampling gated to the PWM off-phase, or a low-pass/moving-average tuned below PWM frequency) is validated against something resembling the real problem before hardware exists. A filter tuned only against clean synthetic data will likely be undersized for real switching noise.
- **Incline power-reallocation runaway / motor stall protection**: this is a closed-loop controller and closed loops can misbehave. Test explicitly for: (a) the controller continuing to increase power against a *stalled* motor (current high, speed ~zero) rather than recognizing stall and cutting power — stall must be an explicit detected state, not just "more current = more power," or the controller will cook the motor/driver on a real stall; (b) oscillation if the gain is tuned too aggressively against the simulated grade step; (c) a hard current ceiling that the compensation controller cannot exceed regardless of computed target, independent of the compensation logic itself (defense in depth).
- **D*-Lite replanning livelock/thrashing**: without rate-limiting, small map updates near the current path can trigger constant replanning, especially if the new plan differs only marginally from the old one and lidar noise causes the map to flicker near the path. Rate-limit replanning (e.g. minimum interval between replans) and add path-hysteresis (don't replan for a marginally-shorter path, only for one that's blocked or meaningfully better) — validate this explicitly in sim with a noisy/flickering obstacle near the path, not just a clean single obstacle-appears test.
- **Battery brownout risk at electromagnet-fire moment**: even before real hardware exists, the synthetic voltage node can inject a voltage sag correlated with a `magnet_trigger` command (coil inrush), so the ESP32-core's brownout-detection/response logic (e.g. brief power derating, or refusing to fire the magnet if the pre-trigger voltage margin is already thin) is exercised in sim rather than discovered for the first time on hardware.
- **Latency-budget decoupling**: exercise 6 above (killing the whole cloud-side stack) is the direct sim-level proof of `spec.md` §9's core claim. Don't skip it — it's cheap to test in sim and expensive to discover is wrong on hardware.

---

## 2b. AWS Setup From Scratch

**Nothing in this section is to be executed against a real AWS account without explicit user confirmation.** This remains a written plan until the user says otherwise, per the project's operating principles. It is written now, in full, so that 2c can reference concrete steps — not as a signal to start provisioning.

### 2b.1 What changes vs. what stays identical, relative to 2a

| Aspect | 2a (local) | 2b (EC2) | Changes? |
|---|---|---|---|
| MQTT broker software | Mosquitto in Docker, localhost | Mosquitto (same software) on EC2 | Host/port only |
| Topic structure & payload schemas | `spec.md` §6.2 | identical | No change |
| SLAM Toolbox / Nav2 / D*-Lite node code | runs against simulated scan/odom | runs against real (relayed) scan/odom | No code change — same nodes, real input |
| Networking | all localhost/docker-network | real network (VPC, security groups, public/elastic IP or bastion) | New — see 2b.3 |
| Auth on the broker | `[ASSUMPTION]` open/local for dev convenience | **must not stay open** — TLS + credentials required (see Warnings) | New — must be added, don't carry the dev default forward |

### 2b.2 Account, IAM, and instance setup

- **Account creation**: standard AWS account signup (email, payment method, identity verification). `[VERIFY]` current signup flow specifics (AWS periodically changes the free-tier/verification flow) — check `aws.amazon.com` at time of signup rather than relying on a remembered flow.
- **IAM — least privilege**: do not use the root account for day-to-day work. Create an IAM user (or, better, use IAM Identity Center / SSO if available) with a policy scoped to only what this project needs: EC2 (start/stop/describe/create for the specific instance(s)), the specific VPC/security-group resources, and CloudWatch (for budget/billing alarms). Avoid `AdministratorAccess` for routine use even though it's the path of least resistance — the project has no need for broad IAM/org-level permissions. `[VERIFY]` exact minimal policy JSON at implementation time against current EC2 API actions.
- **EC2 instance sizing**: SLAM Toolbox + Nav2 + a D*-Lite loop is CPU-bound graph/optimization work, not GPU-bound (no ML inference in this stack as currently scoped). `[ASSUMPTION]` a general-purpose or compute-optimized instance (e.g. `t3.large`/`t3.xlarge` class for light/dev use, `c6i.xlarge` class if SLAM proves CPU-hungry under real lidar rates) is a reasonable starting point — **do not default to a GPU instance**, it isn't justified by this workload. `[VERIFY]` actual instance-type names/specs/pricing at implementation time; right-size empirically (start smaller, watch CPU/memory under real load, resize) rather than guessing large up front.
- **AMI**: `[ASSUMPTION]` start from a standard Ubuntu LTS AMI matching the ROS2 distro's supported OS version (check the ROS2 distro's REP-2000-style platform support table `[VERIFY]`), then install ROS2/Nav2/SLAM Toolbox and the project's Docker images on top — this keeps the EC2 environment reproducible from the same Dockerfiles used in 2a rather than hand-configuring the instance.
- **Networking**: a VPC with a single public subnet is sufficient for this project's scale — no need for a multi-AZ/private-subnet architecture for a hobby-scale single robot. Security group: only the MQTT port (with TLS) and SSH (restricted to the user's own IP, not `0.0.0.0/0`) should be open. No other inbound ports.
- **SSH/bastion access**: for a single instance at this scale, a full separate bastion host is likely overkill — direct SSH to the instance with key-based auth (no password auth) and the security-group IP restriction above is a reasonable simplification `[design judgment, not a hard fact]`. Revisit if the architecture grows to multiple instances.
- **Cost controls**:
  - Set an AWS Budget with an alarm (e.g. email/SNS notification) at a low dollar threshold — this should be one of the first things configured, before any instance is left running.
  - **Do not leave the instance running idle.** Stop (not just disconnect from) the instance between work sessions; an EC2 instance accrues compute cost while running regardless of whether anything is connected to it. A stopped instance still accrues EBS storage cost, but that's small relative to compute.
  - Avoid GPU or large compute-optimized instances entirely for this workload (see sizing above) — they're the single biggest source of accidental cost, and this project's CPU-bound workload doesn't need one.
  - `[ASSUMPTION]` consider a scheduled auto-stop (e.g. a simple cron-triggered stop via a Lambda, or just manual discipline at this hobby scale) to bound worst-case cost from a forgotten running instance — treat this as a recommendation, not yet a decided implementation.

### 2b.3 Warnings & Edge Cases

- **AWS EC2 spot interruption or network blip mid-SLAM**: if a Spot instance is used for cost savings `[not currently recommended as the default — On-Demand is simpler and this project's usage pattern (short dev sessions) doesn't obviously benefit from Spot's savings vs. its interruption risk]`, a mid-SLAM interruption loses in-progress map state unless it's persisted incrementally. Even on On-Demand, transient network blips will drop the MQTT connection — the broker/client must handle reconnect gracefully (MQTT's built-in session/reconnect semantics help here), and SLAM Toolbox's map should be checkpointed periodically so a restart doesn't lose all mapping progress. This is exactly the kind of failure the phone/robot side must tolerate per `spec.md` NFR-6 — the robot doesn't stop being safe just because EC2 had a blip.
- **Cost runaway from a forgotten running instance**: the single most likely real-world failure mode for a hobby project — not a security incident, just an instance left running for days. Mitigated by the budget alarm (set it up *first*, before anything else in 2b) and by the discipline of stopping the instance after each session. Treat "instance is running" as a state that should always be intentional, never incidental.
- **Security of the phone↔EC2 link (unauthenticated MQTT broker, exposed ports)**: the local dev broker in 2a is reasonably left open for convenience (localhost-only, no real exposure). **The EC2 broker must not carry that default forward** — an internet-reachable, unauthenticated MQTT broker is a real exposure (anyone who finds the port can inject fake telemetry, fake waypoints, or a fake `magnet_trigger`/`estop` command). Minimum bar before the EC2 broker is ever reachable from the public internet: TLS (not plaintext MQTT), username/password or certificate-based client auth, and a security group restricting the port to expected source IPs where feasible (harder for a phone on cellular with a changing IP, so auth is doing the real work here, not IP restriction). Flag this explicitly as a must-do in 2c before the phone is ever pointed at a real EC2 endpoint, not an optional hardening pass for later.
- **IAM overprivilege**: re-check the IAM policy in 2b.2 isn't quietly broader than needed (a common failure mode is starting with a broad policy "to get unblocked" and never narrowing it) — this is a recurring check, not a one-time setup step.

---

## 2c. Integration

### 2c.1 Swapping local stand-ins for the real EC2 endpoint

Because 2a and 2b deliberately share the same broker software and topic/payload contracts (`spec.md` §6.2, plan.md 2b.1), this swap should be limited to: (1) changing the MQTT broker hostname/port/credentials the phone app points at, from the local Mosquitto container to the EC2 instance's address, and (2) enabling TLS + auth on the client side to match the now-mandatory server-side security (2b.3). No topic, payload, or node-logic changes should be required — if a change beyond configuration turns out to be necessary, that's a signal the 2a/2b interface parity wasn't actually maintained and should be fixed rather than patched around.

### 2c.2 Phone-to-cloud networking

- **Cellular vs. local Wi-Fi**: cellular gives the robot mobility independent of a fixed Wi-Fi network (important if the robot is meant to operate outdoors/away from a home network), but has more variable and generally higher latency, potential carrier-side NAT/firewall behavior, and data cost considerations. Local Wi-Fi gives lower/more consistent latency but ties operation to Wi-Fi coverage range. `[ASSUMPTION]` design for cellular as the general case (since a ground robot's whole point is mobility) and treat Wi-Fi as a lower-latency special case that works the same way — this is exactly why NFR-4's latency budget in `spec.md` was deliberately left wide (100 ms–2000+ ms) rather than tightly bounded, and why nothing safety-relevant may depend on it (§9).
- **MQTT vs. WebSocket**: MQTT was chosen in `spec.md` §6.2 specifically for its reconnect/session/QoS semantics, which matter more over a cellular link with intermittent connectivity than over a stable local network. This choice should be re-validated once real cellular behavior is observed — if reconnect behavior under real carrier NAT turns out to be worse than expected, that's a concrete, testable reason to revisit, not a reason to preemptively switch now.

### 2c.3 ESP32 firmware bring-up order

1. Bring up the ESP32-core logic (from 2a.3's HAL-separated build) on real hardware with **no sensors attached yet** — validate the mode state machine and SAFE_HOLD logic run and the heartbeat-timeout fail-safe fires correctly, using only a serial console for stimulus.
2. Bring up motor driver output on the bench, wheels off the ground, current-limited — validate PWM output and current sensing before any sensor-driven autonomy exists.
3. Add IMU, then GPS, then the current/voltage sensing chain one at a time, validating each against the sim's expected message shape from `spec.md` §6.1 before adding the next.
4. Add electromagnet control last, and only once brownout protection (validated in sim, 2a.6) is confirmed against real battery behavior on the bench, before it's ever tested with the robot mobile.
5. Only after 1–4 pass individually: connect the phone bridge and re-run the full mode-transition matrix from `spec.md` §5 against **real** hardware, the same test suite already proven in sim (2a.5).
6. Only after 5 passes: point the phone at the real EC2 endpoint (2c.1) and re-run the Autonomous/Follow-me exercises end-to-end.

### 2c.4 Hardware bring-up order (general)

**Power/motor driver → sensors → autonomy stack — never the reverse.** Concretely: verify power delivery and motor driver behavior in isolation (bench-safe, wheels off ground) before any sensor is trusted to feed the control loop, and don't bring up SLAM/Nav2/D*-Lite against real hardware until the ESP32-side reflex/safety loop (steps 1–4 above) is independently proven — the autonomy stack is the *last* thing added, not the first thing tested, because it's the layer with the least direct control over immediate physical safety.

### 2c.5 Warnings & Edge Cases

- **Electromagnet EMI affecting the IMU/compass when triggered**: this is a real hardware phenomenon that cannot be meaningfully simulated in Gazebo (no EMI model). Concrete mitigations to plan for at hardware-bring-up time: physical separation between the electromagnet and the IMU on the chassis layout, checking IMU readings for a characteristic glitch/spike correlated with `magnet_trigger` timing during bring-up step 4 above, and — if a spike is observed — either shielding, relocating the IMU, or having the control loop briefly disregard/hold IMU input for a short window around a trigger event (a software mitigation of last resort, since it creates a brief blind window in exactly the sensor used for incline detection). Flag this explicitly as a bring-up checklist item, not something to assume away.
- **Battery brownout at the electromagnet-firing moment**: the sim-level defense (2a.6) needs real validation here — bench-test the actual coil inrush against the actual battery's real internal resistance and the real brownout-detection threshold, since a synthetic voltage-sag model is necessarily an approximation. Test this before the robot is ever mobile with a real payload, not as an afterthought once everything else works.
- **ESP32↔phone comms loss, real hardware**: re-validate the exact scenario proven in sim (2a.6) — physically disconnect the USB link during Autonomous/Follow-me operation and confirm SAFE_HOLD entry within the NFR-2 budget on real hardware, not just in sim timing.
- **GPS dropout/multipath, real environment**: test near actual structures/indoors where the sim's synthetic fault injection was necessarily a simplification — confirm the sensor-fusion behavior designed against synthetic dropout (2a.6) holds against real multipath artifacts, which can be noisier/weirder than a clean "no fix" simulation.
- **Lidar occlusion by the robot's own chassis/payload, real hardware**: re-derive the self-occlusion mask (2a.6) against the actual lidar mounting position and actual electromagnet arm geometry once both are physically fixed — the sim mask was necessarily provisional.
- **Latency-budget argument, restated for this phase specifically**: this is the phase where it's most tempting to accidentally violate it — e.g. by having the phone app wait on an EC2 acknowledgment before forwarding an e-stop press, or by routing any part of the SAFE_HOLD path through the cloud "just for logging." Don't. The entire fault-detection-to-SAFE_HOLD chain (`spec.md` §9) must remain fully local to the ESP32 through this integration phase exactly as it was in 2a — integration should only ever be *adding* the cloud path for planning/mapping, never *inserting* the cloud path into anything already proven safe locally.
