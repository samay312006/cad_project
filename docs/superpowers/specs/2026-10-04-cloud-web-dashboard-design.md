# Design — Cloud-Hosted Drive Dashboard (ESP32 ↔ EC2 direct, replaces Android app plan)

Status: design pending user review, not yet implemented. No AWS resource created yet.
Owner: cloud/GUI engineer (this session).

Supersedes, pending user sign-off and a follow-up edit pass:
- `spec.md` §6.1 (ESP32↔Phone UDP contract) — the phone-as-bridge role is removed.
- `spec.md` §6.2 (Phone↔EC2 MQTT contract) — becomes ESP32↔EC2 directly; phone drops out.
- `overview_cloud.md`'s architecture diagram and `plan.md` §2c's "point the phone at EC2"
  bring-up step.
- The planned Android app deliverable itself — replaced by this browser dashboard.

None of those files are edited by this document; this is the design to review first.
Edits land in the implementation plan.

---

## 0. Role-boundary note

Same situation as `2026-09-03-web-drive-gui-design.md` §0: this session is configured as
cloud/GUI (`CLAUDE.local.md`), and `ONBOARDING.md` assigns ESP32 firmware to the embedded
engineer. This design again requires ESP32-side changes (swapping the AP+local-server role
for a Wi-Fi-station+MQTT-client role) — done under the user's explicit direction for this
work, same normal exception as before. **A row goes into `UPDATES.md` immediately** (not
deferred to implementation) because, unlike the bring-up tool, this one unilaterally changes
a shared interface contract (`spec.md` §6.1/§6.2) the embedded engineer's future work depends
on — per `CLAUDE.md`'s rule, that can't land silently.

---

## 1. Scope

Replace the planned Android-app + phone-bridge remote-control path with a browser-based
dashboard, hosted in the cloud, controlling the ESP32 directly over MQTT/TLS — reachable
from any phone with a browser and internet access, not just one physically tied to the
robot's local AP.

**In scope (v1)**: everything `embedded/web_drive/` already does (directional drive,
speed cap, live current/voltage telemetry, command watchdog) — ported from local
WebSocket to cloud MQTT. **Not** the full `spec.md` feature set.

**Explicitly out of scope for this change**: mode state machine, `SAFE_HOLD`, electromagnet
control, IMU/GPS telemetry, SLAM/Nav2/D*-Lite integration. Those still don't exist on the
ESP32 yet (per `plan.md` §2c.3's bring-up order: drive → sensors → autonomy) and aren't
blocked or unblocked by this change — this is still "v1 of the bring-up tool," just now
reachable over the internet instead of over a local AP.

---

## 2. Decisions

| # | Decision | Rejected alternative |
|---|---|---|
| D1 | ESP32 becomes a Wi-Fi **station** (joins whatever Wi-Fi/internet is available), drops its own AP | Keeping the AP and tunneling/exposing it (reverse proxy, ngrok-style) — rejected: still ends at the ESP32's own zero-auth HTTP/WS server, much larger exposure than reusing the broker's TLS+auth, and a tunnel client is a bad fit for an ESP32 |
| D2 | ESP32 is MQTT/TLS client, outbound-only, to the already-planned EC2 Mosquitto broker | A bespoke WebSocket relay server on EC2 — rejected: duplicates reconnect/auth work the broker design already solves, abandons `plan.md` §2c.2's MQTT choice (made for exactly this reconnect/session reasoning) without new justification |
| D3 | Dashboard (HTML/JS/CSS, evolved from `web_drive`'s frontend) served statically from the **same** EC2 instance via nginx, which also reverse-proxies MQTT-over-WebSocket to Mosquitto | S3+CloudFront — rejected for now: adds a second AWS surface for a hobby project where the dashboard being unreachable while the EC2 instance is stopped (no robot connected anyway) is acceptable, not a bug |
| D4 | Self-signed TLS cert (no domain purchase) | Real domain + Let's Encrypt — user's explicit call; accepted trade-off is per-device manual cert trust instead of a few $/yr |
| D5 | MQTT auth: username/password on the broker (per `plan.md` §2b.3's existing "must not stay open" requirement) | Cert-based client auth — more setup than this project's scale needs right now; revisit if multi-operator/multi-robot ever happens |
| D6 | ESP32-side watchdog timeout increases from `web_drive`'s local-AP `400ms [ASSUMPTION]` to `[ASSUMPTION] 1500ms`, matching NFR-4's stated latency floor for cloud-relayed links | Keeping 400ms — would false-trip constantly on ordinary internet jitter, defeating the point of remote control |
| D7 | v1 feature parity only (§1) — no pull-forward of modes/safety/magnet into this change | Building full `spec.md` feature set now — rejected as scope creep; this change is about *where* the link goes, not what it carries |

---

## 3. Architecture

```
Any phone (browser, anywhere with internet)
        │  HTTPS (dashboard page, self-signed cert — one-time manual trust)
        │  WSS (MQTT-over-WebSocket, same cert)
        ▼
EC2 instance (single public subnet, VPC — same instance plan.md §2b.2 already specs)
        ├── nginx — serves dashboard static files; reverse-proxies the WSS path to Mosquitto
        ├── Mosquitto (TLS + username/password auth) — already-planned broker, now the
        │    ONLY path to the ESP32 (no more phone-as-bridge)
        └── (later, unaffected by this change) SLAM Toolbox / Nav2 / D*-Lite
        ▲
        │  MQTT/TLS, outbound-only from the ESP32 — it never accepts inbound connections
ESP32 (Wi-Fi station — joins whatever Wi-Fi/internet is available on-site)
```

Security group: 443 (HTTPS+WSS) and 22 (SSH, IP-restricted) only — same minimal posture
as `plan.md` §2b.2, just no separate MQTT port since it's proxied through 443 via nginx.

---

## 4. Message protocol

Reuses `spec.md` §6.2's topic-namespacing idea, collapsed onto one hop since there's no
longer a phone relay in between:

| Topic | Direction | Content | Rate |
|---|---|---|---|
| `robot/{id}/drive` | dashboard→ESP32 | `{"left": -255, "right": 255}` (pre-mixed duty, same shape `web_drive`'s `app.js`/`mix.js` already produce) | event-driven, on joystick move, keepalive while held |
| `robot/{id}/telemetry` | ESP32→dashboard | `{"left_a":0.42,"right_a":0.39,"left_duty":180,"right_duty":178,"vbus":12.1}` (unchanged from `web_drive`'s current shape) | `[ASSUMPTION] 5 Hz`, unchanged |

Single-robot id, fixed for now (`[ASSUMPTION]`, consistent with `spec.md` §6.2).

---

## 5. Safety

**This is the one thing in this design that's a genuine step down in safety margin versus
the original architecture, and it needs to be stated plainly rather than engineered around
quietly.** `overview_controls.md` §5 and `spec.md` §9 both rest on manual drive commands
never depending on a link that can see internet-scale latency or an outage. Routing manual
drive through MQTT-over-internet breaks that assumption — a drive command can now be
delayed up to NFR-4's stated 100ms–2000ms+ range, or simply not arrive.

What keeps this acceptable rather than reckless:

- **The ESP32 still fails safe unilaterally, same mechanism as `web_drive` D3**: no valid
  `drive` message within the watchdog window (D6: `[ASSUMPTION] 1500ms`) → forced
  `drive(0,0)`. This doesn't need the cloud's cooperation — a dead broker connection or a
  stalled network looks identical to "no command arrived," and the ESP32 stops regardless.
- **Boot-safe default** unchanged: motors start at zero duty, `stopAll()` before anything
  else, same as `drive_test.ino`/`drive_calib.ino`/`web_drive`.
- **Duty clamp** (voltage-based, from `drive_calib.ino`) stays in place, unchanged from
  `web_drive`.
- What this does **not** protect against: a command that arrives *late but valid* (e.g. a
  "turn left" sent 1.4s ago, delivered just under the watchdog window) still executes —
  the watchdog bounds how long a *stale link* can leave the robot moving unsupervised, not
  how stale an individual command can be before execution. That's an inherent property of
  any internet-relayed teleop and isn't specific to this design; flagging it so it's a known
  property, not a surprise at bring-up.
- Not implemented here, deliberately (same as `web_drive`): `SAFE_HOLD`, overcurrent trip,
  stall detection.

**Recommendation for bring-up**: validate this on a short-range/low-latency Wi-Fi hop first
(same room as the EC2 instance's actual network path) before trusting it over real cellular,
and bench-test wheels-off-the-ground with an artificially delayed/dropped connection
(e.g. throttle the ESP32's Wi-Fi) to confirm the watchdog actually trips before ever trusting
this outdoors.

---

## 6. AWS setup (what actually happens in the console)

Per `plan.md` §2b.2, simplified since D4 removes the domain/ACM step. **Nothing below is
executed without your explicit go-ahead on each resource — this is the checklist, not
authorization to proceed.**

1. AWS account (if none exists yet) — signup, payment method, identity verification.
2. **Budget alarm first** — low-dollar threshold, before any instance exists.
3. IAM user, scoped to EC2 + this project's VPC/security-group + CloudWatch/Budgets. No
   `AdministratorAccess`.
4. VPC, single public subnet.
5. Security group: inbound 443 and 22 (IP-restricted) only.
6. EC2 instance (`t3.large`-class to start, right-size later) + Elastic IP, Ubuntu LTS AMI.
7. On the instance: Mosquitto (TLS + WebSocket listener, self-signed cert, username/password
   auth) + nginx (serves dashboard static files, reverse-proxies the WSS path).
8. Stop the instance between sessions (cost control, unchanged from `plan.md`).

---

## 7. File layout

```
embedded/web_drive/              # evolves in place, same sketch, not a new one
  web_drive.ino                  # AP+server role → Wi-Fi-station+MQTT-client role
  ws_handler.h                   # replaced by an MQTT client (e.g. PubSubClient or
                                  #  esp-mqtt), not a WebSocket server anymore
  drive_core.h                   # unchanged — motor drive + duty clamp
  data/ → moves to the EC2 host, served by nginx instead of ESP32 LittleFS
    index.html / style.css / app.js   # app.js swaps its raw WebSocket client for an
                                        # MQTT-over-WebSocket client (e.g. MQTT.js) against
                                        # the topics in §4
```

---

## 8. Testing plan

1. Local Mosquitto-in-Docker rehearsal first (same pattern `plan.md` §2a/§2b.1 already
   established for the rest of the project) — point both a dev ESP32 build and the
   dashboard at a local broker before touching EC2 at all.
2. Wheels off the ground (per `drive_test.ino`'s existing bench rule).
3. Confirm drive directions + speed cap behave as commanded, now via MQTT.
4. Confirm the re-tuned watchdog (D6) actually trips on: Wi-Fi disconnect, broker
   connection drop, and an artificially throttled/delayed link — not just "browser tab
   closed," which was the easy case locally.
5. Cross-check telemetry values against `drive_test.ino`'s known-good bench output.
6. Only after 1–5 pass on the local broker: repeat against the real EC2 instance.

---

## 9. Open assumptions / TBD

- Watchdog timeout `1500ms` (D6) — a reasoned starting point from NFR-4, not yet
  bench-verified against a real cellular link's actual worst-case latency. `[VERIFY]`
  at bring-up, same spirit as `web_drive`'s own `[ASSUMPTION] 400ms` note.
- MQTT username/password credential values/rotation — not yet defined, same open item
  `plan.md` §2b.3 already flagged (blocker #4 in `UPDATES.md`) and still unresolved.
- Self-signed cert generation details (validity period, regeneration process) — `[TBD]`
  at implementation time.
- This design doesn't yet touch `spec.md`/`overview_cloud.md` text — those edits, and the
  `UPDATES.md` cross-team flag, are called out as required follow-ups (§0) but left to the
  implementation plan rather than made here.
