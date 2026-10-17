#!/usr/bin/env bash
# Run ON the EC2 instance as root, from the directory holding these files plus dashboard/.
# Usage: sudo ./setup.sh <public-ip>
# Prints the generated broker passwords ONCE to stdout; nothing is stored in plaintext.
set -euo pipefail
IP="${1:?usage: setup.sh <public-ip>}"
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq mosquitto mosquitto-clients nginx openssl >/dev/null

# Self-signed cert for the Elastic IP (10 years); shared by Mosquitto and nginx.
install -d -m 755 /etc/mosquitto/certs /etc/nginx/certs
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 3650 \
  -keyout /tmp/server.key -out /tmp/server.crt -subj "/CN=$IP" \
  -addext "subjectAltName=IP:$IP" 2>/dev/null
install -m 644 /tmp/server.crt /etc/mosquitto/certs/server.crt
install -m 640 -o root -g mosquitto /tmp/server.key /etc/mosquitto/certs/server.key
install -m 644 /tmp/server.crt /etc/nginx/certs/server.crt
install -m 600 /tmp/server.key /etc/nginx/certs/server.key
shred -u /tmp/server.key; rm -f /tmp/server.crt

# Broker users with random passwords.
gen() { openssl rand -base64 24 | tr -d '/+=' | cut -c1-24; }
PW_ESP=$(gen); PW_DASH=$(gen)
: > /etc/mosquitto/passwd
mosquitto_passwd -b /etc/mosquitto/passwd esp32 "$PW_ESP"
mosquitto_passwd -b /etc/mosquitto/passwd dashboard "$PW_DASH"
chown mosquitto:mosquitto /etc/mosquitto/passwd; chmod 600 /etc/mosquitto/passwd
install -m 644 -o root -g mosquitto acl /etc/mosquitto/acl
install -m 644 mosquitto.conf /etc/mosquitto/conf.d/robot.conf
# Drop the package default listener config so only ours applies.
sed -i 's/^\(listener\|allow_anonymous\)/#\1/' /etc/mosquitto/mosquitto.conf 2>/dev/null || true

# Dashboard static files + nginx.
install -d /var/www/dashboard; cp -r dashboard/. /var/www/dashboard/
install -m 644 nginx-robot.conf /etc/nginx/sites-available/robot
ln -sf /etc/nginx/sites-available/robot /etc/nginx/sites-enabled/robot
rm -f /etc/nginx/sites-enabled/default

systemctl enable --now mosquitto nginx >/dev/null 2>&1
systemctl restart mosquitto; nginx -t; systemctl reload nginx

echo "BROKER_HOST=$IP"
echo "ESP32_USER=esp32  ESP32_PASSWORD=$PW_ESP"
echo "DASHBOARD_USER=dashboard  DASHBOARD_PASSWORD=$PW_DASH"
