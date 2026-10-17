#!/bin/bash
# The benchmark's server: official sing-box, two inbounds - VLESS + Vision +
# REALITY on 443/tcp and Hysteria 2 on 8443/udp (a self-signed certificate:
# clients take it with insecure=1) - one user, plain direct out. Prints
# nothing secret: the share links go to /root/bench/links.txt.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq curl openssl ca-certificates >/dev/null
mkdir -p /etc/apt/keyrings
curl -fsSL https://sing-box.app/gpg.key -o /etc/apt/keyrings/sagernet.asc
echo "deb [arch=amd64 signed-by=/etc/apt/keyrings/sagernet.asc] https://deb.sagernet.org/ * *" > /etc/apt/sources.list.d/sagernet.list
apt-get update -qq
apt-get install -y -qq sing-box >/dev/null

mkdir -p /root/bench /etc/sing-box
cd /root/bench
UUID=$(sing-box generate uuid)
KEYS=$(sing-box generate reality-keypair)
PRIV=$(echo "$KEYS" | awk '/PrivateKey/ {print $2}')
PUB=$(echo "$KEYS" | awk '/PublicKey/ {print $2}')
SID=$(openssl rand -hex 8)
HYPASS=$(openssl rand -hex 16)
SNI=www.ebay.com
IP=$(curl -4 -s https://api.ipify.org)
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
  -keyout /etc/sing-box/hy.key -out /etc/sing-box/hy.crt -subj "/CN=bench.local" 2>/dev/null

cat > /etc/sing-box/config.json <<EOF
{
  "log": { "level": "warn" },
  "inbounds": [
    {
      "type": "vless", "tag": "reality", "listen": "::", "listen_port": 443,
      "users": [ { "uuid": "$UUID", "flow": "xtls-rprx-vision" } ],
      "tls": {
        "enabled": true, "server_name": "$SNI",
        "reality": { "enabled": true, "handshake": { "server": "$SNI", "server_port": 443 },
                     "private_key": "$PRIV", "short_id": [ "$SID" ] }
      }
    },
    {
      "type": "hysteria2", "tag": "hy2", "listen": "::", "listen_port": 8443,
      "users": [ { "password": "$HYPASS" } ],
      "tls": { "enabled": true, "server_name": "bench.local",
               "certificate_path": "/etc/sing-box/hy.crt", "key_path": "/etc/sing-box/hy.key" }
    }
  ],
  "outbounds": [ { "type": "direct", "tag": "direct" } ]
}
EOF
sing-box check -c /etc/sing-box/config.json
systemctl enable --now sing-box >/dev/null 2>&1
systemctl restart sing-box
sleep 1
systemctl is-active sing-box

cat > /root/bench/links.txt <<EOF
vless://$UUID@$IP:443?encryption=none&flow=xtls-rprx-vision&security=reality&sni=$SNI&fp=chrome&pbk=$PUB&sid=$SID&type=tcp#bench-REALITY
hysteria2://$HYPASS@$IP:8443?sni=bench.local&insecure=1#bench-Hysteria2
EOF
chmod 600 /root/bench/links.txt
sing-box version | head -1
ss -lntup | grep sing-box | awk '{print $1, $5}'
