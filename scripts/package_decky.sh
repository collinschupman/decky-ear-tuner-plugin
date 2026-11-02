#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUTPUT_DIR="${1:-${SCRIPT_DIR}/release}"

command -v node > /dev/null 2>&1 || {
    echo "ERROR: Node.js is required to package Ear Tuner." >&2
    exit 1
}
command -v pnpm > /dev/null 2>&1 || {
    echo "ERROR: pnpm is required to package Ear Tuner." >&2
    exit 1
}
command -v strip > /dev/null 2>&1 || {
    echo "ERROR: strip is required to package the native backend." >&2
    exit 1
}
command -v cmake > /dev/null 2>&1 || {
    echo "ERROR: CMake is required to package Ear Tuner." >&2
    exit 1
}
command -v tar > /dev/null 2>&1 || {
    echo "ERROR: tar is required to package Ear Tuner." >&2
    exit 1
}

cd "${SCRIPT_DIR}"
VERSION="$(node -p "require('./package.json').version")"
BUILD_DIR="${SCRIPT_DIR}/native/build"
BACKEND_BIN="${BUILD_DIR}/decky_ear_tuner_backend"
DECKY_API_DIR="$(dirname "$(node -p "require.resolve('@decky/api/package.json')")")"
DECKY_UI_DIR="$(dirname "$(node -p "require.resolve('@decky/ui/package.json')")")"
STAGE_DIR="$(mktemp -d)"
trap 'rm -rf "${STAGE_DIR}"' EXIT

cmp -s "${DECKY_API_DIR}/LICENSE" "${DECKY_UI_DIR}/LICENSE" || {
    echo "ERROR: Decky API and UI licenses differ; package each license separately." >&2
    exit 1
}

pnpm run typecheck
pnpm run build:decky
cmake -S native -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
cmake --build "${BUILD_DIR}" --target decky_ear_tuner_backend --parallel
cmake --build "${BUILD_DIR}" --parallel
ctest --test-dir "${BUILD_DIR}" --output-on-failure

[[ -f dist/index.js ]] || {
    echo "ERROR: Decky bundle was not created at dist/index.js." >&2
    exit 1
}
[[ -d dist/assets ]] || {
    echo "ERROR: Decky assets were not created at dist/assets." >&2
    exit 1
}
[[ -x "${BACKEND_BIN}" ]] || {
    echo "ERROR: Native backend was not created at ${BACKEND_BIN}." >&2
    exit 1
}
strip --strip-unneeded "${BACKEND_BIN}"

mkdir -p "${STAGE_DIR}/dist/assets" "${STAGE_DIR}/licenses" "${STAGE_DIR}/native/build"
cp LICENSE package.json plugin.json main.py service.sh "${STAGE_DIR}/"
cp docs/THIRD_PARTY_NOTICES.md "${STAGE_DIR}/"
cp "${DECKY_API_DIR}/LICENSE" "${STAGE_DIR}/licenses/LGPL-2.1.txt"
cp -R src/ear_tuner "${STAGE_DIR}/"
find "${STAGE_DIR}/ear_tuner" -type d -name "__pycache__" -prune -exec rm -rf {} +
cp dist/index.js "${STAGE_DIR}/dist/"
cp -R dist/assets/. "${STAGE_DIR}/dist/assets/"
cp "${BACKEND_BIN}" "${STAGE_DIR}/native/build/"
chmod +x "${STAGE_DIR}/service.sh" "${STAGE_DIR}/native/build/decky_ear_tuner_backend"

mkdir -p "${OUTPUT_DIR}"
PACKAGE_PATH="${OUTPUT_DIR}/ear-tuner-${VERSION}.tar.gz"
tar -C "${STAGE_DIR}" -czf "${PACKAGE_PATH}" .
tar -tzf "${PACKAGE_PATH}" | grep -Fx './package.json' > /dev/null || {
    echo "ERROR: Archive is missing the ES module package metadata." >&2
    exit 1
}
tar -tzf "${PACKAGE_PATH}" | grep -Fx './ear_tuner/application_service.py' > /dev/null || {
    echo "ERROR: Archive is missing the Ear Tuner application package." >&2
    exit 1
}
tar -tzf "${PACKAGE_PATH}" | grep -Fx './LICENSE' > /dev/null || {
    echo "ERROR: Archive is missing the BSD-3-Clause license." >&2
    exit 1
}
tar -tzf "${PACKAGE_PATH}" | grep -Fx './THIRD_PARTY_NOTICES.md' > /dev/null || {
    echo "ERROR: Archive is missing third-party notices." >&2
    exit 1
}
tar -tzf "${PACKAGE_PATH}" | grep -Fx './licenses/LGPL-2.1.txt' > /dev/null || {
    echo "ERROR: Archive is missing the bundled Decky libraries' license." >&2
    exit 1
}
[[ "$(tar -tzf "${PACKAGE_PATH}" | grep -Ec '^\./dist/assets/.*\.wav$')" -eq 1 ]] || {
    echo "ERROR: Archive must contain exactly one bundled listening sample." >&2
    exit 1
}
cp scripts/install_beta.sh "${OUTPUT_DIR}/install-beta.sh"
chmod +x "${OUTPUT_DIR}/install-beta.sh"
if command -v sha256sum > /dev/null 2>&1; then
    (cd "${OUTPUT_DIR}" && sha256sum "$(basename "${PACKAGE_PATH}")") \
        > "${PACKAGE_PATH}.sha256"
else
    (cd "${OUTPUT_DIR}" && shasum -a 256 "$(basename "${PACKAGE_PATH}")") \
        > "${PACKAGE_PATH}.sha256"
fi
printf 'Created %s\n' "${PACKAGE_PATH}"
printf 'Created %s\n' "${OUTPUT_DIR}/install-beta.sh"
printf 'Created %s.sha256\n' "${PACKAGE_PATH}"
