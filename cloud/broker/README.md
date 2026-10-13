# EC2 broker + dashboard — runbook

Public MQTT broker for the robot and HTTPS host for `cloud/dashboard/` (design:
`docs/superpowers/specs/2026-10-04-cloud-web-dashboard-design.md`, D2–D5). The local, anonymous
rehearsal stack is `cloud/docker-compose.yml` + `cloud/mosquitto/` — never deploy that one.

**`CLAUDE.md` rule: no AWS command, resource or spend without the human's explicit OK.**

## What exists (created 2026-10-13, all in Sydney `ap-southeast-2`)

The AWS account (687633314412, "Golden Hour Code") is on the **free plan, which only allows
`ap-southeast-2`** — every other region returns `OptInRequired` / "Region unavailable". Never click
"Activate advanced features" in the console without the human's OK (that upgrades to the paid plan).

| Resource | ID / value |
|---|---|
| Budget alarm | `robot-monthly-10usd` — $10/mo, emails at 80% actual / 100% forecast |
| Key pair | `robot-broker` → private key `~/.ssh/robot-broker.pem` (only on the laptop that created it) |
| Security group | `sg-044c61d5a0f9867c5` — 443, 8883 from anywhere; 22 from one owner IP only |
| Instance | `i-0ee4e7a7d4034817c` — `t3.small`, Ubuntu 24.04, 20 GB gp3 encrypted, CPU credits `standard` |
| Elastic IP | **`3.24.67.217`** (`eipalloc-0f5e81a6f78414798`) |

All tagged `project=controls_pro`. Console: region selector → Asia Pacific (Sydney).

Cost (AWS Price List, Sydney, checked 2026-10-13): instance $0.0264/h, Elastic IP $0.005/h (also
while stopped), disk $0.096/GB-mo → ≈ $24.85/mo running 24/7, ≈ $5.57/mo stopped. Paid from the
free-plan credit ($120, ends 2027-04-13). **Stop the instance when not in use.**

## CLI login

AWS CLI v2 at `~/.local/bin/aws`, profile `robot2` (browser sign-in, assumed role
`AccountFullAccessRole`, not root):

```
aws login --profile robot2 --region ap-southeast-2
```

Shorthand used below: `A="--profile robot2 --region ap-southeast-2"`, `I=i-0ee4e7a7d4034817c`.

## Start / stop

```
aws ec2 start-instances --instance-ids $I $A && aws ec2 wait instance-running --instance-ids $I $A
aws ec2 stop-instances  --instance-ids $I $A
```

The Elastic IP stays attached across stop/start — the address never changes. Releasing it would
change the IP (new server cert + ESP32 reflash), so don't. **Terminate ≠ stop** (terminate deletes).

## SSH from a new network / laptop

Port 22 only allows one IP. If yours changed (`curl -s https://checkip.amazonaws.com`):

```
aws ec2 revoke-security-group-ingress    $A --group-id sg-044c61d5a0f9867c5 --protocol tcp --port 22 --cidr <old-ip>/32
aws ec2 authorize-security-group-ingress $A --group-id sg-044c61d5a0f9867c5 --protocol tcp --port 22 --cidr <new-ip>/32
```

A different laptop also needs `~/.ssh/robot-broker.pem` and `~/.robot-pki/` copied over securely
(not via git), or a new key pair + `make_certs.sh` run (a **new CA means re-flashing the ESP32's
`MQTT_CA_CERT`**).

## Deploy / redeploy

Instance must be running. From the repo:

```
cloud/broker/deploy.sh 3.24.67.217
```

- `make_certs.sh` — project CA (once) + server cert with `subjectAltName=IP:<ip>`, in
  `~/.robot-pki/` (outside the repo, 700). **`ca.key` never leaves the laptop.**
- Broker passwords generated once into `~/.robot-pki/broker-creds.env` (600): `robot1` (ESP32)
  and `dashboard` (browser).
- `setup_instance.sh` runs on the instance: installs Mosquitto + nginx, installs
  `mosquitto-robot.conf` (TLS 8883 public, WebSocket 9001 on localhost only, no anonymous),
  `acl`, `nginx-robot.conf` (HTTPS 443, `/mqtt` → 9001), copies `cloud/dashboard/`.

Re-run after editing any config or dashboard file; it keeps the same certs/passwords.

## Verified (2026-10-13, after first deploy)

Server cert verifies on 8883 and 443 (and a wrong IP is rejected); 9001/1883 closed from outside;
anonymous and wrong-password logins refused; `robot1` publishing `drive` is dropped by the ACL;
dashboard over `wss://…/mqtt` receives telemetry published by `robot1` over TLS 8883.

## ACL (who can do what)

| User | Read | Write |
|---|---|---|
| `robot1` (ESP32) | `robot/robot1/drive` | `robot/robot1/telemetry` |
| `dashboard` (browser) | `robot/robot1/telemetry` | `robot/robot1/drive` |

## Using it

- **Dashboard:** `https://3.24.67.217/` → the browser warns about the self-signed cert once
  (or install `~/.robot-pki/ca.crt` on the phone as a trusted CA) → log in as `dashboard`.
- **ESP32:** copy `embedded/web_drive/secrets.example.h` → `secrets.h` (gitignored), set
  `MQTT_USE_TLS 1`, host `"3.24.67.217"`, port `8883`, user `robot1`, `ROBOT_PASSWORD` from
  `broker-creds.env`, and paste `~/.robot-pki/ca.crt` into `MQTT_CA_CERT`.

## Never

- Commit `secrets.h`, `~/.robot-pki/*`, `*.pem`, or `~/.aws`.
- Copy `ca.key` anywhere.
- Open 9001 or 1883 in the security group, or use `allow_anonymous true` here.
