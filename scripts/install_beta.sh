#!/usr/bin/env bash
set -euo pipefail

DECKY_HOME="${DECKY_HOME:-${HOME}/homebrew}"
PLUGINS_DIR="${DECKY_HOME}/plugins"
PLUGIN_DIR="${PLUGINS_DIR}/ear-tuner"

usage() {
    echo "Usage: $0 <archive-path-or-https-url> [sha256]" >&2
}

fail() {
    echo "ERROR: $*" >&2
    exit 1
}

[[ $# -ge 1 && $# -le 2 ]] || {
    usage
    exit 1
}

ARCHIVE_SOURCE="$1"
EXPECTED_SHA256="${2:-}"
DOWNLOAD_FILE=""
STAGE_DIR=""
cleanup() {
    [[ -z "${STAGE_DIR}" ]] || rm -rf "${STAGE_DIR}"
    [[ -z "${DOWNLOAD_FILE}" ]] || rm -f "${DOWNLOAD_FILE}"
}
trap cleanup EXIT

[[ -x "${DECKY_HOME}/services/PluginLoader" ]] || fail \
    "Decky Loader is required. Install it first from https://decky.xyz/."

if [[ "${ARCHIVE_SOURCE}" =~ ^https:// ]]; then
    command -v curl > /dev/null 2>&1 || fail "curl is required to download the release."
    DOWNLOAD_FILE="$(mktemp)"
    ARCHIVE="${DOWNLOAD_FILE}"
    echo "==> Downloading Ear Tuner beta release"
    curl --fail --location --show-error --output "${ARCHIVE}" "${ARCHIVE_SOURCE}"
else
    ARCHIVE="${ARCHIVE_SOURCE}"
fi

[[ -f "${ARCHIVE}" ]] || fail "Release archive was not found: ${ARCHIVE}"

if [[ -n "${EXPECTED_SHA256}" ]]; then
    command -v sha256sum > /dev/null 2>&1 || fail "sha256sum is required to verify the release."
    ACTUAL_SHA256="$(sha256sum "${ARCHIVE}" | awk '{print $1}')"
    [[ "${ACTUAL_SHA256}" == "${EXPECTED_SHA256}" ]] || fail "Release checksum does not match."
fi

command -v tar > /dev/null 2>&1 || fail "tar is required to install the release."
ARCHIVE_CONTENTS="$(tar -tzf "${ARCHIVE}")" || fail "Release archive is not a valid .tar.gz file."
if grep -Eq '(^/|(^|/)\.\.(/|$))' <<< "${ARCHIVE_CONTENTS}"; then
    fail "Release archive contains an unsafe path."
fi
NORMALIZED_CONTENTS=""
while IFS= read -r archive_path; do
    NORMALIZED_CONTENTS+="${archive_path#./}"$'\n'
done <<< "${ARCHIVE_CONTENTS}"
for required_file in package.json plugin.json main.py service.sh dist/index.js \
    native/build/decky_ear_tuner_backend ear_tuner/application_service.py; do
    grep -Fxq "${required_file}" <<< "${NORMALIZED_CONTENTS}" ||
        fail "Release archive is missing ${required_file}."
done

mkdir -p "${PLUGINS_DIR}"
[[ ! -L "${PLUGIN_DIR}" ]] || fail "Refusing to replace the symlink at ${PLUGIN_DIR}."

STAGE_DIR="$(mktemp -d "${DECKY_HOME}/.ear-tuner.stage.XXXXXX")"
BACKUP_DIR="${DECKY_HOME}/.ear-tuner.backup.$$"

tar -xzf "${ARCHIVE}" -C "${STAGE_DIR}"
[[ -f "${STAGE_DIR}/plugin.json" ]] || fail "Release extraction did not produce plugin.json."
[[ -x "${STAGE_DIR}/native/build/decky_ear_tuner_backend" ]] ||
    fail "Release extraction did not produce an executable native backend."

if [[ -x "${PLUGIN_DIR}/service.sh" ]]; then
    echo "==> Stopping the installed Ear Tuner service"
    "${PLUGIN_DIR}/service.sh" stop || true
fi

if [[ -e "${PLUGIN_DIR}" ]]; then
    mv "${PLUGIN_DIR}" "${BACKUP_DIR}"
fi
if ! mv "${STAGE_DIR}" "${PLUGIN_DIR}"; then
    [[ ! -e "${BACKUP_DIR}" ]] || mv "${BACKUP_DIR}" "${PLUGIN_DIR}"
    fail "Could not install the release. The previous plugin was restored."
fi
rm -rf "${BACKUP_DIR}"

echo "Installed Ear Tuner at ${PLUGIN_DIR}."
echo "Return to Game Mode and reload Decky to load the new version."
