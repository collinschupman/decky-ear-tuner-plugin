#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DECKY_HOME="${DECKY_HOME:-${HOME}/homebrew}"
PLUGIN_DIR="${DECKY_HOME}/plugins/ear-tuner"
LEGACY_PLUGIN_DIR="${DECKY_HOME}/plugins/asymmetrical-hearing"

echo "==> Stopping audio processing and removing Ear Tuner filter nodes"
bash "${ROOT_DIR}/service.sh" uninstall || true

removed=false
for plugin_dir in "${PLUGIN_DIR}" "${LEGACY_PLUGIN_DIR}"; do
    if [[ -L "${plugin_dir}" ]]; then
        LINK_TARGET="$(readlink -f "${plugin_dir}" || true)"
        if [[ "${LINK_TARGET}" != "${ROOT_DIR}" ]]; then
            echo "ERROR: ${plugin_dir} points to ${LINK_TARGET}, not this " \
                "repository." >&2
            echo "Refusing to remove it." >&2
            exit 1
        fi
        rm -- "${plugin_dir}"
        removed=true
    elif [[ -e "${plugin_dir}" ]]; then
        echo "ERROR: ${plugin_dir} is not a symlink; refusing to remove it." >&2
        exit 1
    fi
done
if [[ "${removed}" == true ]]; then
    echo "Removed Decky development plugin link."
else
    echo "Decky development plugin link is already absent."
fi

if [[ "${1:-}" == "--purge-settings" ]]; then
    rm -rf -- "${DECKY_HOME}/settings/ear-tuner" \
        "${DECKY_HOME}/settings/asymmetrical-hearing"
    echo "Removed persisted Ear Tuner settings."
fi

echo "Return to Game Mode and use Decky Settings to reload the plugin list."
