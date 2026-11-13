#!/bin/sh
set -eu
APP=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
INITCTL=/sbin/initctl
STATE=/tmp/kindle-dashboard-service
case "${1:-start}" in
    start)
        if [ -f "$STATE/supervisor.pid" ] && kill -0 "$(cat "$STATE/supervisor.pid")" 2>/dev/null; then
            echo "Kindle Dashboard is already running"
            exit 0
        fi
        mkdir -p "$APP/logs"
        if [ -f "$APP/logs/dashboard.log" ]; then
            mv "$APP/logs/dashboard.log" "$APP/logs/dashboard.previous.log"
        fi
        nohup /bin/sh "$APP/control.sh" run >>"$APP/logs/dashboard.log" 2>&1 </dev/null &
        echo "Starting Kindle Dashboard"
        ;;
    run)
        [ -x "$APP/bin/kindle-dashboard" ]
        [ -f "$APP/config.yaml" ]
        command -v fbink >/dev/null
        mkdir "$STATE" || { echo "Service state exists; use stop before restarting"; exit 1; }
        echo "$$" >"$STATE/supervisor.pid"
        child=
        restore_framework=0
        restore_webreader=0
        prior_screensaver=$(lipc-get-prop com.lab126.powerd preventScreenSaver)
        cleanup() {
            trap - EXIT INT TERM HUP
            if [ -n "$child" ]; then
                kill -TERM "$child" 2>/dev/null || true
                wait "$child" 2>/dev/null || true
            fi
            lipc-set-prop com.lab126.powerd preventScreenSaver "$prior_screensaver" || true
            if [ "$restore_framework" = 1 ]; then "$INITCTL" start framework || true; fi
            if [ "$restore_webreader" = 1 ]; then "$INITCTL" start webreader || true; fi
            rm -f "$STATE/supervisor.pid" "$STATE/program.pid"
            rmdir "$STATE"
        }
        trap cleanup EXIT
        trap 'exit 0' INT TERM HUP
        if "$INITCTL" status framework | grep -q 'start/running'; then
            restore_framework=1
            "$INITCTL" stop framework
        fi
        if "$INITCTL" status webreader | grep -q 'start/running'; then
            restore_webreader=1
            "$INITCTL" stop webreader
        fi
        lipc-set-prop com.lab126.powerd preventScreenSaver 1
        lipc-set-prop com.lab126.powerd wakeUp 1 || true
        export CURL_CA_BUNDLE="$APP/certs/cacert.pem"
        cd "$APP"
        "$APP/bin/kindle-dashboard" --config "$APP/config.yaml" --assets "$APP/assets" &
        child=$!
        echo "$child" >"$STATE/program.pid"
        wait "$child"
        ;;
    stop)
        if [ -f "$STATE/supervisor.pid" ]; then
            supervisor=$(cat "$STATE/supervisor.pid")
            case "$supervisor" in ''|*[!0-9]*) echo 'Invalid service PID'; exit 1;; esac
            if [ -r "/proc/$supervisor/cmdline" ] && tr '\000' ' ' <"/proc/$supervisor/cmdline" | grep -Fq "$APP/control.sh run"; then
                kill -TERM "$supervisor"
                echo "Stopping Kindle Dashboard and restoring Kindle UI"
            else
                echo "Supervisor is not running; stale state requires inspection"
                exit 1
            fi
        else
            echo "Kindle Dashboard is not running"
        fi
        ;;
    status)
        if [ -f "$STATE/program.pid" ] && kill -0 "$(cat "$STATE/program.pid")" 2>/dev/null; then
            echo "running PID $(cat "$STATE/program.pid")"
        else
            echo stopped
        fi
        ;;
    *) echo 'Usage: control.sh start|stop|status'; exit 1;;
esac
