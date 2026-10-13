#!/usr/bin/env bash
# setup_instance.sh - runs ON the EC2 instance (as root, via deploy.sh) from
# the staging dir deploy.sh copied over. Installs/updates Mosquitto + nginx
# with this project's config. Safe to re-run: every step overwrites.
set -euo pipefail
cd "$(dirname "$0")"

export DEBIAN_FRONTEND=noninteractive
if ! command -v mosquitto >/dev/null || ! command -v nginx >/dev/null; then
  apt-get update -q
  apt-get install -y -q mosquitto mosquitto-clients nginx
fi

# ---- Mosquitto ----
install -d -m 750 -o root -g mosquitto /etc/mosquitto/certs
install -m 644 -o root -g mosquitto ca.crt server.crt /etc/mosquitto/certs/
install -m 640 -o root -g mosquitto server.key /etc/mosquitto/certs/
install -m 644 mosquitto-robot.conf /etc/mosquitto/conf.d/robot.conf
install -m 640 -o root -g mosquitto acl /etc/mosquitto/acl

# broker-creds.env holds ROBOT_PASSWORD / DASHBOARD_PASSWORD (generated on the
# laptop by deploy.sh). Rebuild the password file from it, then delete it.
# shellcheck disable=SC1091
source broker-creds.env
rm -f /etc/mosquitto/passwd
install -m 640 -o root -g mosquitto /dev/null /etc/mosquitto/passwd
mosquitto_passwd -b /etc/mosquitto/passwd robot1 "$ROBOT_PASSWORD"
mosquitto_passwd -b /etc/mosquitto/passwd dashboard "$DASHBOARD_PASSWORD"
chown root:mosquitto /etc/mosquitto/passwd; chmod 640 /etc/mosquitto/passwd

systemctl enable mosquitto >/dev/null
systemctl restart mosquitto

# ---- nginx ----
install -d -m 750 -o root -g www-data /etc/nginx/robot
install -m 644 fullchain.crt /etc/nginx/robot/
install -m 640 -o root -g www-data server.key /etc/nginx/robot/
install -m 644 nginx-robot.conf /etc/nginx/sites-available/robot
ln -sf /etc/nginx/sites-available/robot /etc/nginx/sites-enabled/robot
rm -f /etc/nginx/sites-enabled/default
rm -rf /var/www/robot-dashboard
install -d -m 755 /var/www/robot-dashboard
cp dashboard/*.html dashboard/*.css dashboard/*.js /var/www/robot-dashboard/
nginx -t
systemctl enable nginx >/dev/null
systemctl reload-or-restart nginx

systemctl is-active mosquitto nginx
