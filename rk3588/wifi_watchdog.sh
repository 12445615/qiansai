#!/bin/bash
set +e

TARGET="${WIFI_PING_TARGET:-10.139.8.231}"
CONNECT_SCRIPT="${WIFI_CONNECT_SCRIPT:-/root/wifi_auto_connect.sh}"
LOCK_FILE="/run/wifi-watchdog.lock"
LOG_FILE="/var/log/wifi_watchdog.log"
FAIL_COUNT=0

log() {
    echo "$(date '+%F %T') $*" | tee -a "$LOG_FILE"
}

exec 9>"$LOCK_FILE"
if ! flock -n 9; then
    exit 0
fi

while true; do
    if ping -c 2 -W 2 "$TARGET" >/dev/null 2>&1; then
        FAIL_COUNT=0
        sleep 30
        continue
    fi

    FAIL_COUNT=$((FAIL_COUNT + 1))
    logger -t wifi-watchdog "ping $TARGET failed count=$FAIL_COUNT"
    log "ping $TARGET failed count=$FAIL_COUNT"

    if [ "$FAIL_COUNT" -ge 1 ]; then
        logger -t wifi-watchdog "full reconnect wifi by $CONNECT_SCRIPT"
        log "full reconnect"
        timeout 90 bash "$CONNECT_SCRIPT"
        FAIL_COUNT=0
        sleep 30
    else
        sleep 10
    fi
done
