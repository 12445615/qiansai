#!/bin/bash
set +e

IFACE="${WIFI_IFACE:-wlP4p65s0}"
WPA_CONF="${WPA_CONF:-/etc/wpa_supplicant.conf}"
PING_TARGET="${WIFI_PING_TARGET:-10.139.8.231}"
LOG_FILE="/var/log/wifi_auto_connect.log"

log() {
    echo "$(date '+%F %T') $*" | tee -a "$LOG_FILE"
}

dmesg -n 1

log "start full reconnect iface=$IFACE target=$PING_TARGET"

cd /lib/firmware/ || exit 1
if [ -f "iwlwifi-ty-a0-gf-a0.pnvm" ]; then
    mv iwlwifi-ty-a0-gf-a0.pnvm iwlwifi-ty-a0-gf-a0.pnvm.bak
fi

cd /root/ || exit 1
rmmod iwlmvm 2>/dev/null
rmmod iwlwifi 2>/dev/null
insmod iwlwifi.ko 2>/dev/null
insmod iwlmvm.ko 2>/dev/null

sleep 2
rfkill unblock all 2>/dev/null
ifconfig "$IFACE" up 2>/dev/null

killall -9 wpa_supplicant 2>/dev/null
rm -f "/var/run/wpa_supplicant/$IFACE"
wpa_supplicant -B -i "$IFACE" -c "$WPA_CONF"

sleep 3
timeout 5 dhclient -r "$IFACE" 2>/dev/null
timeout 20 dhclient -1 "$IFACE"

cat > /etc/resolv.conf <<EOF
nameserver 114.114.114.114
nameserver 8.8.8.8
EOF

ip addr show "$IFACE"
ping -c 2 -W 2 "$PING_TARGET"
log "WiFi Auto Connect Finished."
