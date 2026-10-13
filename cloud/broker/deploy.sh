#!/usr/bin/env bash
# deploy.sh <broker-ip> - push the broker + dashboard to the EC2 instance
# (instance must be RUNNING; see README.md). Run from anywhere on the laptop.
#   1. make_certs.sh <ip> if there's no server cert for that IP yet
#   2. generate broker passwords once ($ROBOT_PKI/broker-creds.env, 600)
#   3. copy configs + certs + creds + cloud/dashboard/ to the instance over
#      SSH, run setup_instance.sh there as root, delete the staging copy
# Never copies ca.key. Re-running is safe (keeps the same passwords/certs).
set -euo pipefail

IP="${1:?usage: deploy.sh <broker-ip>}"
KEY="${ROBOT_SSH_KEY:-$HOME/.ssh/robot-broker.pem}"
PKI="${ROBOT_PKI:-$HOME/.robot-pki}"
HERE="$(cd "$(dirname "$0")" && pwd)"
DASH="$HERE/../dashboard"

if [[ "$(cat "$PKI/server.ip" 2>/dev/null)" != "$IP" ]]; then
  "$HERE/make_certs.sh" "$IP"
fi

CREDS="$PKI/broker-creds.env"
if [[ ! -f "$CREDS" ]]; then
  umask 077
  gen() { openssl rand -base64 24 | tr -d '/+=' | cut -c1-24; }
  printf 'ROBOT_PASSWORD=%s\nDASHBOARD_PASSWORD=%s\n' "$(gen)" "$(gen)" > "$CREDS"
  echo "Generated broker passwords in $CREDS"
fi

STAGE="$(mktemp -d)"
trap 'rm -rf "$STAGE"' EXIT
cp "$HERE"/{mosquitto-robot.conf,acl,nginx-robot.conf,setup_instance.sh} "$STAGE/"
cp "$PKI"/{ca.crt,server.crt,server.key,fullchain.crt} "$CREDS" "$STAGE/"
mkdir "$STAGE/dashboard"
cp "$DASH"/*.html "$DASH"/*.css "$DASH"/*.js "$STAGE/dashboard/"

SSH=(ssh -i "$KEY" -o StrictHostKeyChecking=accept-new "ubuntu@$IP")
"${SSH[@]}" 'rm -rf ~/robot-deploy && mkdir -m 700 ~/robot-deploy'
scp -q -i "$KEY" -r "$STAGE"/. "ubuntu@$IP:robot-deploy/"
"${SSH[@]}" 'sudo bash ~/robot-deploy/setup_instance.sh; rc=$?; rm -rf ~/robot-deploy; exit $rc'

echo
echo "Deployed. Dashboard: https://$IP/  (login user: dashboard)"
echo "ESP32 secrets.h: MQTT_USE_TLS 1, host \"$IP\", port 8883, user robot1,"
echo "  password ROBOT_PASSWORD from $CREDS, MQTT_CA_CERT = $PKI/ca.crt"
