# Overview — Cloud Architecture

## Abstract

A hobby-scale 4-wheel ground robot pairs a real-time ESP32 controller with a handheld Android phone bridge/GUI (connected wirelessly, not robot-mounted) and a cloud "brain" (AWS EC2 running SLAM Toolbox, Nav2, and a custom D*-Lite global planner) to deliver three operating modes — Manual, Autonomous, and Follow-me — plus an electromagnet actuator for small magnetic payload pickup. The system's defining control property is a closed-loop, autonomous incline-compensation controller: rather than relying on manual per-terrain tuning, the robot infers increased drive load from current/voltage sensing (a back-EMF-informed signal) and reallocates motor power in real time. This is enforced by a strict architectural split — the ESP32 owns all real-time control and safety locally; the cloud is used only for heavy math (mapping, global path planning) and is never in the safety-critical path.

## Application

Intended as a low-cost platform for terrain-crossing payload retrieval (e.g. picking up small ferrous objects across mixed indoor/outdoor terrain including ramps/inclines) and as a full-stack robotics reference build — embedded real-time control through cloud-hosted SLAM/planning — at hobby-project cost. From a cloud-architecture standpoint, this is also a reference for running a real-time-adjacent (but not safety-critical) robotics compute backend on minimal AWS infrastructure, with an explicit local-first validation methodology and a cost posture appropriate to a hobby budget rather than a production fleet.

## Novelty

1. **Load-inference incline compensation** — grade is inferred from motor electrical behavior (current rise / back-EMF drop under constant commanded duty cycle), not from a pre-tuned slope lookup table or IMU-pitch-only heuristic. (Full depth in `overview_controls.md`.)
2. **A hard local-reflex / global-planning boundary**, enforced by an explicit latency budget and message contract — the cloud stack is architected knowing it is never on the robot's safety-critical path, which shapes several cloud decisions below (e.g. why a broker blip is an acceptable, tolerated failure mode rather than an incident).
3. **Local-simulation-first development, including the cloud interface itself**: the same MQTT broker software (e.g. Mosquitto) runs identically in a local Docker container and later on EC2 — only the hostname/port/credentials change — so the cloud integration step (`plan.md` 2c) is a configuration change, not new code, and every cloud-facing interface is proven against a local stand-in before any AWS resource exists.

*(Controls-systems depth — sensor fusion, the incline controller, ESP32 real-time structure — is in `overview_controls.md`, which shares this same abstract/application/novelty; kept light here.)*

## Architecture

```
ESP32 ──Wi-Fi (AP)── Phone (handheld bridge) ──MQTT/TLS── EC2 instance, single public subnet, VPC
                                                              │
                                                              ├── Mosquitto broker (TLS + auth)
                                                              ├── SLAM Toolbox  ← range scan + odom (relayed via phone)
                                                              ├── Nav2 (costmaps, controller server, lifecycle mgmt)
                                                              └── D*-Lite global planner (standalone node, see §2)
```

The scan SLAM Toolbox consumes is now assembled on the ESP32 from a single-point range sensor on a continuously sweeping servo (not a 2D lidar, dropped for cost — `spec.md` §4/§7) and relayed up through the ESP32↔phone Wi-Fi link before reaching the phone↔EC2 hop shown above; this doesn't change anything on the cloud side of the diagram, only the input rate/resolution SLAM Toolbox should be tuned to expect.

Security group: MQTT(TLS) port + SSH (source-IP-restricted) only. No other inbound. IAM: a scoped least-privilege user/role, not root, not `AdministratorAccess`. Full ESP32/phone-side detail is in `overview_controls.md`; full interface contracts are in `spec.md` §6.

---

## 1. AWS Topology

- **Account**: dedicated project account (or a separate IAM boundary within an existing account) — standard signup, `[VERIFY]` current AWS signup/verification flow at implementation time, since it periodically changes.
- **IAM**: an IAM user (or IAM Identity Center principal) scoped to only what this project needs — EC2 lifecycle actions on the project's instance(s), the project's VPC/security-group resources, and CloudWatch/Budgets for cost alarms. No root-account routine use. `[VERIFY]` exact minimal policy JSON against the current EC2 API action set at implementation time; the policy should be re-checked periodically for accidental scope creep, not written once and forgotten.
- **VPC**: a single public subnet is sufficient at this project's scale (one robot, one instance) — no multi-AZ, no private-subnet/NAT-gateway complexity, which would add cost and operational surface with no corresponding benefit here.
- **EC2 instance**: `[ASSUMPTION]` general-purpose or compute-optimized, non-GPU (e.g. `t3.large`/`t3.xlarge`-class for dev, `c6i.xlarge`-class if SLAM proves CPU-hungry under real sensor rates) — SLAM Toolbox + Nav2 + D*-Lite is CPU-bound graph/optimization work with no ML-inference component as currently scoped, so a GPU instance is not justified and would be the single largest avoidable cost. `[VERIFY]` exact instance type names/specs/pricing at implementation time; right-size empirically rather than provisioning large up front.
- **AMI**: `[ASSUMPTION]` Ubuntu LTS matching the ROS2 distro's supported platform list `[VERIFY against the distro's platform support table]`, with the project's Docker images installed on top — this keeps EC2 environment setup reproducible from the same Dockerfiles validated locally in `plan.md` 2a, rather than a hand-configured, undocumented instance.
- **SSH/bastion**: direct key-based SSH to the instance, security-group-restricted to the user's own IP, no password auth — a full separate bastion host is reasonable to skip at single-instance hobby scale `[design judgment]`; revisit only if the architecture grows to multiple instances or multiple operators.

---

## 2. SLAM Toolbox / Nav2 / D*-Lite Architecture

- **SLAM Toolbox**: subscribes to relayed `sensor_msgs/LaserScan` (and odometry, if available — see the wheel-encoder gap noted in `overview_controls.md` §1) and publishes an occupancy map plus the `map → odom` transform. `[VERIFY]` exact mode (e.g. online-async vs. lifelong mapping) appropriate for a single continuously-operating robot vs. session-based mapping — this is a real configuration decision to make deliberately against current `slam_toolbox` documentation, not to leave at a default.
- **Nav2**: standard components — a costmap stack (static layer from the SLAM map, obstacle layer from live scan data, inflation layer for safety margin around obstacles), a controller server for local trajectory following, and a lifecycle manager orchestrating each node through the standard ROS2 managed-lifecycle states (unconfigured → inactive → active). `[VERIFY]` exact default controller plugin (commonly DWB or a regulated-pure-pursuit-style controller in recent Nav2 releases) — pin and confirm against the chosen distro rather than assuming a specific one, since Nav2's shipped defaults have changed across releases.
- **D*-Lite global planner**: **not** a planner shipped in Nav2's default set. Per the design decision in `plan.md` 2a.4, build it first as a standalone ROS2 node (subscribes to the map + goal, publishes `nav_msgs/Path`, feeds Nav2's controller server directly) rather than as a full `nav2_core` plugin, deferring the more idiomatic-but-higher-API-risk plugin integration to a later stretch goal. This keeps the highest-uncertainty piece of the stack (a from-scratch planner implementation) decoupled from Nav2's plugin lifecycle while it's being debugged.
- **Why D*-Lite specifically**: `[design rationale, not a verified external claim]` D*-Lite is an incremental replanning algorithm — it reuses prior search state rather than replanning from scratch on each map update, which is a reasonable fit for a robot whose map changes incrementally as it explores/moves rather than being fully known upfront. This must be paired with the rate-limiting/hysteresis measures in `plan.md` 2a.6 to avoid replan thrashing, which is a known failure mode of any live-replanning approach, not specific to D*-Lite.

---

## 3. Scaling & Cost Tradeoffs

At current scope (one robot, hobby use, intermittent operating sessions rather than 24/7 operation), the dominant cost driver is **EC2 uptime and instance size**, not data transfer — scan/telemetry volumes at this scale are small (smaller still now that scan data comes from a servo-swept single-point sensor rather than a spinning 2D lidar) relative to typical AWS data-transfer pricing tiers `[VERIFY: confirm against current AWS data-transfer pricing if this assumption is ever load-bearing for a cost estimate]`. The architecture doesn't need to scale out today, but the topic-namespacing already anticipated in `spec.md` §6.2 (`robot/{id}/...`) means a second robot could, in principle, share a broker and either share or get its own compute — that's a future decision, not one this plan needs to make now, and shouldn't be over-engineered for prematurely (see the project's general anti-premature-abstraction stance).

**Cost controls, in priority order**:
1. A Budgets alarm, configured *before* anything else in AWS setup — this is the actual backstop against the most likely real-world failure mode (an instance left running, not a security incident).
2. Never a GPU or large compute-optimized instance for this workload — see §1 sizing.
3. Stop (not just disconnect from) the instance between sessions — a stopped instance stops accruing compute cost; only small EBS storage cost remains.
4. `[ASSUMPTION]` a scheduled auto-stop (e.g. cron-triggered Lambda) is worth considering as a backstop against forgetting step 3, but is a recommendation, not a committed implementation detail yet.

---

## 4. Security Posture

- **Transport**: MQTT **over TLS**, not plaintext — the local dev broker (`plan.md` 2a) can reasonably stay open/unauthenticated on localhost for convenience, but that default must not carry forward to the EC2 broker, which is reachable from the public internet.
- **Auth**: username/password or certificate-based client auth on the broker. IP-restriction alone isn't sufficient here since the phone is often on cellular with a changing IP — auth is the real control, not network position.
- **Exposed surface**: only the MQTT(TLS) port and SSH (source-IP-restricted) are open on the security group. No other inbound ports, no default-open management interfaces.
- **The ESP32↔phone hop is a separate wireless surface from anything in this section**, and shouldn't be overlooked just because it's not cloud-side: the ESP32 now hosts its own Wi-Fi AP (`spec.md` §6.1, since the phone is handheld rather than robot-mounted) and must require WPA2/WPA3 auth. An open AP there would let anyone in range inject `mode_request`/`estop`/`magnet_trigger` commands directly at the ESP32 — upstream of, and independent from, the MQTT broker auth described above.
- **IAM**: least-privilege scoped policy (§1), reviewed periodically for scope creep rather than granted broadly once "to get unblocked."
- **Why this matters concretely**: an unauthenticated, internet-reachable broker would let anyone who finds the port inject fake telemetry, fake waypoints, or — more seriously — a fake `magnet_trigger` or spoofed mode-change command. Because the ESP32 is architected to never trust the cloud link for safety (`overview_controls.md` §3–4), a compromised broker can't directly cause an unsafe *physical* action outside of what the phone/ESP32 would otherwise accept as a normal command — but it could still cause nuisance behavior (unwanted electromagnet triggers, corrupted maps, bogus waypoints) and should not be treated as low-stakes just because it isn't in the direct safety path.

---

## 5. Controls Summary (light — see `overview_controls.md` for depth)

The ESP32 owns real-time control, mode arbitration, and the closed-loop back-EMF/current-derived incline-compensation controller, entirely independent of the cloud link — the cloud never sits on the fault-detection-to-safe-state path (`spec.md` §9). This is why several of the cloud-side decisions above (tolerating a broker blip, treating Spot interruption as a data-loss-of-progress issue rather than a safety issue, keeping the latency budget wide) are acceptable: nothing physically unsafe depends on this stack being available or fast. Full derivation of the incline controller, sensor fusion approach, and ESP32 real-time task structure is in `overview_controls.md`.
