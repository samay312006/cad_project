# Handoff — cloud/GUI session, 2026-10-12 → 2026-10-13

Written for the next engineer's Claude Code session. Read `CLAUDE.md`, `UPDATES.md`, then this
file. Nothing here changes `spec.md`; where it affects shared state, `UPDATES.md` has a row.

**State in one paragraph:** the MQTT-based ESP32 firmware (`embedded/web_drive/`) and the browser
dashboard (`cloud/dashboard/`) were bench-tested on a real ESP32 against a local Mosquitto broker
and mostly pass. The next goal — a public EC2 broker so the phone dashboard and the ESP32 can be
on any network — is **blocked on getting a working AWS account**. No AWS resource has been created.

> **UPDATE 2026-10-17 — supersedes the "no AWS resource exists" / "blocked" statements below
> (§4 and the state paragraph above are historical).** Working AWS account: 687633314412,
> profile `robot2`, region **ap-southeast-2 (Sydney)** — not ap-south-1 as §4 planned.
> Live: EC2 `robot-broker-2` (`i-076e0f36edc586e82`, t3.small, Ubuntu 24.04) + Elastic IP
> `52.65.238.172`; Mosquitto TLS 8883 + nginx 443 (dashboard + `/mqtt` WSS proxy), self-signed cert
> (CN/SAN = the IP, 10 y), users `esp32`/`dashboard` (passwords only in the human's
> `~/.robot-broker-credentials`, not in git). Config + installer: `cloud/ec2/` (`setup.sh`).
> SSH key `~/.ssh/robot-broker-2.pem`; port 22 is open to 0.0.0.0/0 at the human's request.
> The dashboard does `wss://` + login on https (plain http = local anonymous rehearsal).
> The ESP32 firmware in `embedded/web_drive/` now does TLS (pinned cert) + login; it reads
> `secrets.h` (gitignored; template `secrets.h.example`). Flashed and running.
> **Bench over the real link (wheels off):** TLS+login connect pass; drive over WAN pass;
> telemetry pass; watchdog fires 1.5 s after the last *received* drive message, but end-to-end
> stop is ~2.4-2.9 s after the operator's last message because link latency is ~1 s (see below);
> inbound black-hole (broker->ESP dropped via iptables) -> duty 0 ~2 s after the block began, not
> the library's 15 s stall; broker restart -> ESP reconnects in ~2 s unattended.
> **Not done (needs the human at the bench):** Wi-Fi drop/auto-reconnect (toggle the hotspot),
> telemetry cross-check vs `drive_test.ino` under real load, direction check by an observer.
> **Latency finding:** TCP connect from the dev PC to Sydney is ~0.43 s and the ESP32 is on a
> mobile carrier, so the drive path adds ~1 s. A Mumbai (ap-south-1) broker would likely cut
> this a lot; moving it = new resources, needs the human's confirmation. `WiFi.setSleep(false)`
> was tried and did not help (reverted).

---

## 1. Goal

Phone opens the dashboard at `https://<elastic-ip>`, ESP32 connects to the same EC2 Mosquitto on
`8883` (TLS + username/password). Phone and ESP32 can each be on any network. Design:
`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md` (D1–D7, §6 is the AWS checklist).
Plan: `docs/superpowers/plans/2026-10-04-cloud-web-dashboard.md`.

## 2. Bench results (real ESP32, wheels off the ground, local broker)

Hardware: ESP32-D0WD-V3 on `/dev/ttyUSB0` (CP210x), 2x INA219 at 0x40/0x41, vbus ~12.6 V.
Test setup: Mosquitto on the PC, firmware built from a **scratchpad copy** with Wi-Fi/broker
settings edited (repo files still have the placeholders `your-dev-wifi` / `192.0.2.1`).

| Checklist item (plan Task 4 Step 4) | Result |
|---|---|
| 1. Wi-Fi + MQTT connect | **Pass** |
| 2. Drive commands move motors | **Partial** — duty applied correctly (telemetry shows it). Duty 30/255 only stalls the motors (beep, no rotation, ~0.3-0.4 A). Duty 60 spins them. Direction match **not confirmed by a human observer**. |
| 3. Watchdog on command silence | **Pass** — ESP32's own `millis()` prints show the stop at 1501-1502 ms after the last command, 7 trials |
| 4. Broker killed while spinning | **Pass** — motors stop at ~1.5 s (watchdog), no extra stall |
| 5. Broker restart | **Pass** — reconnect in ~4.2 s with no manual reset |
| 6. Telemetry | **Pass** — ~5 Hz (200 ms), plausible values |

**Still open (blocker #9 in `UPDATES.md` stays open for these):**
- Mid-packet link drop (the `setSocketTimeout(1)` case) — not tested. Needs a raw-socket test that
  sends half an MQTT PUBLISH then goes silent.
- Wi-Fi drop/recover (turn the hotspot off/on while running) — not tested.
- **Left/right current sign differs** at the same duty (left ≈ −0.6 A, right ≈ +0.6 A). Not compared
  to `drive_test.ino`'s known-good values. Could be INA219 orientation/wiring.
- Duplicate telemetry messages seen once in an early test — never isolated (possibly my test script).

## 3. Gotchas that cost time (don't repeat)

- **Opening `/dev/ttyUSB0` resets the ESP32** (CP210x DTR/RTS) even with `dtr=False; rts=False`
  set before open. Wait for the `MQTT connected` serial line before starting a test, or the first
  commands are lost while it reboots.
- **Never `pgrep -f` / `pkill -f` a pattern that appears in your own command line** — it matches
  and kills the shell running it (exit 144). Use `pgrep -x`, a pid file, or an explicit PID.
- The system `mosquitto` service already holds `127.0.0.1:1883`. The bench broker ran on a
  separate config: listener `1884` (TCP) + `9001` (websockets), anonymous, **bench only**.
- Telemetry arrives every 200 ms; watchdog timing from telemetry alone has ±200 ms+ error and
  script lag. Measure from ESP32-side `millis()` prints instead.
- A phone hotspot network may isolate clients or give USB-tether and Wi-Fi different subnets; the
  PC needed a Wi-Fi adapter on the same SSID as the ESP32 for local tests.
- Flashing: `arduino-cli` was installed to `~/.local/bin`. `arduino-cli compile/upload --fqbn
  esp32:esp32:esp32`. First upload attempt sometimes fails with "No serial data received" — retry.
  PubSubClient 2.8 installed as an Arduino library.

## 4. AWS: status and plan

**No AWS resource exists. Nothing was created.** The AWS CLI v2 is at `~/.local/bin/aws`.

What was found:
- The existing AWS account (about a month old) is registered but **every EC2 API call fails with
  `OptInRequired` in every region tried** (us-east-1, us-west-2, eu-west-1, ap-southeast-1,
  ap-south-1). Opening the EC2 console redirected to the "Complete your account setup" page.
  Likely cause: signup/verification not finished (payment verification hold on a UPI AutoPay
  mandate, identity verification, support-plan choice). Not a region problem — the `us-east-1` in
  billing/sign-in URLs is normal. The account is on the free plan with US$100 credits, 183 days.
- A scoped IAM user (`robot-deploy`) was created on that account and a CLI profile configured.
  The CLI profile was later removed locally; **the IAM user and its access key may still exist in
  that account — review/delete in the IAM console if the account is abandoned.**
- Decision: the human is switching to a **different AWS account where EC2 works**, via
  `aws login --profile robot2` (browser sign-in; profile `robot2`, region `ap-south-1`). That
  login was started but may not have been completed.

AWS rules (from `CLAUDE.md`): **no AWS resource, spend, or cloud command without the human's
explicit confirmation.** Even with a general go-ahead, list the exact resources and get a yes.

Planned resources (not yet confirmed by the human), region `ap-south-1`:
1. Budget alarm — $10/month, email address **not yet provided** (ask; don't assume).
2. Key pair (new, saved to `~/.ssh`, `chmod 600`).
3. Security group — inbound 443, 8883 from anywhere; 22 from the human's current IP only.
4. EC2 `t3.small` Ubuntu LTS, 20 GB gp3, default VPC (design doc said a single-subnet VPC; the
   default VPC is a deliberate simplification — record it if kept).
5. Elastic IP attached.
6. On the instance: Mosquitto (TLS 8883 + WSS behind nginx 443 + username/password auth), nginx
   serving `cloud/dashboard/`, self-signed cert for the Elastic IP, random generated broker
   password printed once. Stop the instance when not in use (note: a stopped instance means the
   robot and dashboard can't connect).
Prices/free-plan limits quoted in the session were from memory — verify before relying on them.

First thing to do on a new account: `aws sts get-caller-identity --profile robot2` (confirm it is
**not** root), then a read-only `aws ec2 describe-vpcs --profile robot2 --region ap-south-1`. If it
also returns `OptInRequired`, stop and tell the human instead of trying more accounts.

## 5. Code work still to do (needs no AWS)

1. **Dashboard** (`cloud/dashboard/`, cloud/GUI-owned): switch to `wss://` with broker credentials.
2. **ESP32 firmware TLS** (`embedded/web_drive/mqtt_handler.h`, `web_drive.ino`):
   `WiFiClientSecure`, broker username/password, and a way to trust the self-signed cert.
   **The human approved this as a one-off firmware exception for the cloud/GUI session on
   2026-10-12; if the embedded engineer now takes it, ownership is normal.** Do it in a scratch
   copy first; keep credentials out of git (use an uncommitted config header).
3. Instance config files: `mosquitto.conf` (TLS/auth), nginx config, cert generation script.
4. After the broker is public: re-run bench items 2-6 against it, plus the two open checks above.

## 6. Open `UPDATES.md` items that interact with this

#9 (bench bring-up, partially done above), #10 (ESP32 needs internet reachability / hotspot or
cellular), #11 (no per-topic client arbitration), #13 (stale role text), #14 (heartbeat/SAFE_HOLD
undefined). New row #15 added for the AWS account blocker.

## 7. Things not to do

- Don't commit the Wi-Fi password, broker credentials, AWS keys, or any `~/.aws` content.
- Don't leave the anonymous bench broker (1884/9001) running on a public network.
- Don't reuse the root AWS account for day-to-day work.
- Housekeeping done: old scratch logs deleted; WhatsApp Linux snap removal was **requested but not
  done** (needs `sudo snap remove --purge whatsapp-linux-app`, run by the human).
