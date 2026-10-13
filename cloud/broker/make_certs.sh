#!/usr/bin/env bash
# make_certs.sh <broker-ip> - create the project CA (once) and a server cert
# for <broker-ip>, on the LAPTOP. Output goes to $ROBOT_PKI (default
# ~/.robot-pki), outside the repo: ca.key must never be committed or copied
# to the instance - only ca.crt, server.crt and server.key are deployed.
#
# The ESP32 pins ca.crt (secrets.h MQTT_CA_CERT), so a new IP only needs a
# new server cert (re-run this with the new IP), not a firmware CA change.
set -euo pipefail

IP="${1:?usage: make_certs.sh <broker-ip>}"
PKI="${ROBOT_PKI:-$HOME/.robot-pki}"
mkdir -p "$PKI"; chmod 700 "$PKI"
cd "$PKI"
umask 077

if [[ ! -f ca.key ]]; then
  echo "Creating project CA in $PKI"
  openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
    -keyout ca.key -out ca.crt -days 3650 -subj "/CN=controls_pro robot CA" \
    -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
    -addext "keyUsage=critical,keyCertSign,cRLSign"
fi

# Server cert: SAN IP must equal the exact host the clients dial (the
# ESP32's MQTT_BROKER_HOST, and the address typed into the browser).
echo "Issuing server cert for IP:$IP"
openssl req -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes \
  -keyout server.key -out server.csr -subj "/CN=$IP"
openssl x509 -req -in server.csr -CA ca.crt -CAkey ca.key -CAcreateserial \
  -out server.crt -days 825 -extfile <(printf '%s\n' \
    "basicConstraints=critical,CA:FALSE" \
    "keyUsage=critical,digitalSignature" \
    "extendedKeyUsage=serverAuth" \
    "subjectAltName=IP:$IP")
rm -f server.csr
cat server.crt ca.crt > fullchain.crt
openssl verify -CAfile ca.crt server.crt
echo "$IP" > server.ip
