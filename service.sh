#!/bin/bash

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"

DEVELOPMENT_BACKEND_BIN="${SCRIPT_DIR}/native/build-current/decky_ear_tuner_backend"
RELEASE_BACKEND_BIN="${SCRIPT_DIR}/native/build/decky_ear_tuner_backend"
if [[ -x "${DEVELOPMENT_BACKEND_BIN}" ]]; then
    BACKEND_BIN="${DEVELOPMENT_BACKEND_BIN}"
else
    BACKEND_BIN="${RELEASE_BACKEND_BIN}"
fi
HTTP_PORT=19285
CONTROL_API_URL="http://127.0.0.1:${HTTP_PORT}/api/v1/control"
DIAGNOSTICS_API_URL="http://127.0.0.1:${HTTP_PORT}/api/v1/diagnostics"

_runtime="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
BACKEND_LOG="${_runtime}/ear-tuner-backend.log"
BACKEND_PID_FILE="${_runtime}/ear-tuner-backend.pid"

run_host() {
    if [[ -n "${FLATPAK_ID:-}" ]] && command -v flatpak-spawn > /dev/null 2>&1; then
        flatpak-spawn --host "$@"
    else
        "$@"
    fi
}

start_backend() {
    stop_backend
    if [[ ! -x "${BACKEND_BIN}" ]]; then
        echo "ERROR: ${BACKEND_BIN} not found" >&2
        return 1
    fi
    echo "Starting native PipeWire backend..."
    # shellcheck disable=SC2016 # Expanded by the host shell.
    run_host sh -c \
        'nohup "$2" > "$3" 2>&1 < /dev/null & echo "$!" > "$1"' \
        sh "${BACKEND_PID_FILE}" "${BACKEND_BIN}" "${BACKEND_LOG}"
    echo "Backend launch requested"
}

stop_backend() {
    local pid
    pid=$(run_host cat "${BACKEND_PID_FILE}" 2> /dev/null || true)
    if [[ -n "${pid}" ]] && run_host kill -0 "${pid}" 2> /dev/null; then
        run_host kill -TERM "${pid}" 2> /dev/null || true
    fi
    run_host pkill -TERM -f "^${BACKEND_BIN}( |$)" 2> /dev/null || true
    sleep 0.3
    run_host pkill -KILL -f "^${BACKEND_BIN}( |$)" 2> /dev/null || true
    run_host rm -f "${BACKEND_PID_FILE}"
}

remove_legacy_alert_policy() {
    local config_home policy_file
    # shellcheck disable=SC2016 # Expanded by the host shell.
    config_home=$(run_host sh -c 'printf "%s" "${XDG_CONFIG_HOME:-${HOME}/.config}"')
    policy_file="${config_home}/pipewire/pipewire-pulse.conf.d/90-ear-tuner-alert-routing.conf"
    if run_host test -f "${policy_file}"; then
        echo "Removing volume feedback routing policy..."
        run_host rm -f "${policy_file}"
        run_host systemctl --user restart pipewire-pulse.service
    fi
}

wait_for_http() {
    local timeout="${1:-10}"
    local elapsed=0
    echo "Waiting for backend HTTP..."
    while ! curl -sf "${CONTROL_API_URL}?left=0&right=0" > /dev/null 2>&1; do
        sleep 0.5
        elapsed=$((elapsed + 1))
        if ((elapsed >= timeout * 2)); then
            echo "ERROR: Backend not ready after ${timeout}s" >&2
            return 1
        fi
    done
    echo "Backend HTTP ready."
}

cmd_start() {
    echo "=== Starting Ear Tuner ==="
    if ! start_backend; then
        return 1
    fi
    if ! wait_for_http 10; then
        stop_backend
        return 1
    fi
    echo "=== Ear Tuner started ==="
}

cmd_stop() {
    echo "=== Stopping Ear Tuner ==="
    stop_backend
    echo "=== Ear Tuner stopped ==="
}

cmd_status() {
    echo "--- Ear Tuner status ---"
    local pid
    pid=$(run_host cat "${BACKEND_PID_FILE}" 2> /dev/null || true)
    if [[ -n "${pid}" ]] && run_host kill -0 "${pid}" 2> /dev/null; then
        echo "backend  : running (pid ${pid})"
        echo "scheduler: TID CLS RTPRIO PRI COMMAND (FF/RR indicates real-time)"
        run_host ps -L -p "${pid}" -o tid=,cls=,rtprio=,pri=,comm= || true
        local diagnostics
        diagnostics=$(curl -sf "${DIAGNOSTICS_API_URL}" 2> /dev/null || true)
        if [[ -n "${diagnostics}" ]]; then
            echo "audio    : ${diagnostics}"
        else
            echo "audio    : diagnostics unavailable"
        fi
    elif [[ -n "${pid}" ]]; then
        echo "backend  : stopped (stale pid file)"
    else
        echo "backend  : stopped"
    fi
    echo "---------------------------"
}

trap cmd_stop SIGINT SIGTERM

case "${1:-}" in
    start)
        cmd_start
        ;;
    stop)
        cmd_stop
        ;;
    status)
        cmd_status
        ;;
    output-id)
        run_host pactl get-default-sink
        ;;
    output-ready)
        run_host pw-cli info 0 > /dev/null 2>&1
        ;;
    uninstall)
        cmd_stop
        remove_legacy_alert_policy
        ;;
    *)
        echo "Usage: $0 {start|stop|status|output-id|output-ready|uninstall}" >&2
        exit 1
        ;;
esac
