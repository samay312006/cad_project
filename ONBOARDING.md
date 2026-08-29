# Onboarding — Ground Robot + Cloud Control Project

Welcome. This project is split between two engineers, each running their own Claude Code
session, working out of this same shared git repo. This doc tells you what you own, what
you don't touch, and how to get your session configured.

Read `spec.md`, `plan.md`, `overview_cloud.md` (or `overview_controls.md` depending on
which role you are, below) before writing anything. Then read `UPDATES.md` — it's the
live list of cross-dependencies between the two of you, and both sessions are expected to
read it at the start of every session.

---

## 1. The two roles

| | Embedded/Firmware engineer | Cloud/GUI engineer |
|---|---|---|
| **Owns** | ESP32 firmware: mode state machine + `SAFE_HOLD` (`spec.md` §5), incline-compensation control loop (`overview_controls.md` §2), all sensor drivers (IMU, GPS, current/voltage, the single-point range sensor + sweep servo), motor + electromagnet drivers, the ESP32 side of the wireless link (Wi-Fi AP hosting, packet framing), the local HAL-based sim of the control core (`plan.md` 2a.3), ESP32-side hardware bring-up (`plan.md` 2c.3) | AWS account/IAM/EC2 (`plan.md` 2b), SLAM Toolbox + Nav2 + D*-Lite (`plan.md` 2a.4/2b), the MQTT broker (local Docker Mosquitto and EC2, `plan.md` 2a.1/2b.3), the Android phone app — GUI (Manual/Autonomous/Follow-me, map display), phone-side Wi-Fi client to the ESP32 AP, phone-side MQTT client to EC2 |
| **Validates locally via** | Native-Linux build of the ESP32 control core behind a HAL, exercised against synthetic sensor nodes (`plan.md` 2a.2/2a.3) | Gazebo + Dockerized SLAM/Nav2/broker stack (`plan.md` 2a.1), before any AWS resource exists |
| **Does not touch** | AWS/cloud config of any kind, SLAM/Nav2/D*-Lite code, MQTT broker config beyond a local stub needed to test against, Android/GUI app code | ESP32 firmware or its HAL, sensor/servo/motor driver code, anything that changes the ESP32-side wire format unilaterally |
| **Hard rule** | — | Never creates a real AWS resource / spends money without the human's explicit go-ahead first (`plan.md` 2b, `CLAUDE.md`) |

**Shared, neither side owns alone:** `spec.md` §6 interface contracts (the ESP32↔phone
message schema and the phone↔EC2 MQTT topics/payloads). Either engineer can propose a
change, but it goes through `spec.md` + a row in `UPDATES.md` before the other side's code
depends on it — not a silent change on one side.

**Physical hardware:** the embedded engineer leads physical bring-up (chassis, motors,
ESP32, sensors, servo+rangefinder, battery/power wiring) since it's inherent to firmware
bring-up (`plan.md` 2c.3/2c.4). The cloud/GUI engineer's physical involvement is lighter
now that the phone is a handheld device rather than robot-mounted — mainly phone
setup/testing and general assembly help as needed. This isn't a rigid 50/50 split; say so
in `UPDATES.md` if it needs rebalancing.

---

## 2. Repo mechanics

- **One shared git repo** (this one) — both engineers clone/pull/push here. Root-level
  docs (`spec.md`, `plan.md`, `overview_*.md`, `ONBOARDING.md`, `UPDATES.md`) are jointly
  maintained; keep them in sync with whichever side changes.
- **Suggested code layout**, once code starts (not yet present): an `embedded/` directory
  for ESP32 firmware + HAL sim, and a `cloud_gui/` directory for AWS/SLAM/Nav2/D*-Lite
  code and the Android app. Each engineer works primarily in their own directory.
- Don't push straight to `main` for anything beyond docs without the other engineer at
  least aware — small PRs/branches per engineer are recommended once code exists, so a
  broken build on one side doesn't block the other.

---

## 3. Setting up your Claude Code session

`CLAUDE.md` at the repo root is shared and read by both sessions — it doesn't say which
role *your* session plays. That comes from a **local, gitignored `CLAUDE.local.md`** that
each of you creates once in your own checkout. Create yours now:

**If you're the embedded/firmware engineer**, create `CLAUDE.local.md` with:

```markdown
# Local role config (not committed)

You are the embedded/firmware engineer on this project. You own ESP32 firmware, sensor/
servo/motor drivers, the ESP32-side wireless link implementation, the local HAL-based sim
of the control core, and ESP32-side hardware bring-up — see ONBOARDING.md for the full
boundary. Do not write AWS, SLAM/Nav2/D*-Lite, MQTT-broker, or Android/GUI code — if a task
seems to need that, stop and flag it in UPDATES.md instead of doing it yourself.
```

**If you're the cloud/GUI engineer**, create `CLAUDE.local.md` with:

```markdown
# Local role config (not committed)

You are the cloud/GUI engineer on this project. You own AWS/EC2 setup, SLAM Toolbox +
Nav2 + D*-Lite, the MQTT broker (local + EC2), and the Android phone app (GUI, phone-side
Wi-Fi client, phone-side MQTT client) — see ONBOARDING.md for the full boundary. Do not
write or edit ESP32 firmware, its HAL, or sensor/servo/motor driver code. Never create a
real AWS resource or spend money without the human's explicit confirmation first, no
matter how small. If a task seems to need touching the embedded side, stop and flag it in
UPDATES.md instead of doing it yourself.
```

Add `CLAUDE.local.md` to your global git excludes or confirm it's in this repo's
`.gitignore` (it already is) — it must never get committed, since the other engineer's
session would then load your role instructions too.

---

## 4. Daily workflow

1. Start of session: read `UPDATES.md`. Check whether anything blocking your track got
   resolved, or whether you're the one blocking the other side.
2. Do your work inside your role's boundary (§1 above).
3. If you touch a shared interface (`spec.md` §6) or finish something the other engineer
   was waiting on, update `UPDATES.md` before ending the session.
4. If you hit something that needs the other side's code/config to test end-to-end, don't
   guess at their side — stub it, note the assumption in `UPDATES.md`, and move on.
