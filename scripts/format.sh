#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TOOL_DIR="${ROOT_DIR}/.venv/bin"

resolve_tool() {
    local name="${1}"
    if [[ -x "${TOOL_DIR}/${name}" ]]; then
        printf '%s\n' "${TOOL_DIR}/${name}"
    elif command -v "${name}" > /dev/null 2>&1; then
        command -v "${name}"
    else
        echo "ERROR: ${name} is unavailable; install requirements-dev.txt." >&2
        return 1
    fi
}

PRETTIER="${ROOT_DIR}/node_modules/.bin/prettier"
if [[ ! -x "${PRETTIER}" ]]; then
    echo "ERROR: Prettier is unavailable; run pnpm install." >&2
    exit 1
fi

RUFF="$(resolve_tool ruff)"
CLANG_FORMAT="$(resolve_tool clang-format)"
CMAKE_FORMAT="$(resolve_tool cmake-format)"
SHFMT="$(resolve_tool shfmt)"

cd "${ROOT_DIR}"

CPP_FILES=()
while IFS= read -r file_path; do
    CPP_FILES+=("${file_path}")
done < <(find src/native src/dsp -type f \( -name '*.h' -o -name '*.cpp' \))

"${PRETTIER}" --write .
"${RUFF}" check --fix main.py src/ear_tuner src/tests
"${RUFF}" format main.py src/ear_tuner src/tests
"${CLANG_FORMAT}" -i "${CPP_FILES[@]}"
"${CMAKE_FORMAT}" -i native/CMakeLists.txt native/cmake/check_rtsan_guard.cmake
"${SHFMT}" -w -i 4 -ci -sr \
    install-decky.sh service.sh uninstall-decky.sh update-decky.sh \
    .githooks/pre-push scripts/*.sh
