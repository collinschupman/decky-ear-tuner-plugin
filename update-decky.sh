#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "${ROOT_DIR}"

command -v git > /dev/null 2>&1 || {
    echo "ERROR: Git is required to update Ear Tuner." >&2
    exit 1
}

[[ -d .git ]] || {
    echo "ERROR: ${ROOT_DIR} is not a Git checkout." >&2
    exit 1
}

if [[ -n "$(git status --porcelain --untracked-files=normal)" ]]; then
    echo "ERROR: The Ear Tuner checkout has local changes." >&2
    echo "Commit or stash them before updating." >&2
    exit 1
fi

UPSTREAM="$(git rev-parse --abbrev-ref --symbolic-full-name '@{upstream}' \
    2> /dev/null || true)"
[[ -n "${UPSTREAM}" ]] || {
    echo "ERROR: The current branch has no upstream branch." >&2
    exit 1
}

echo "==> Pulling the latest Ear Tuner update from ${UPSTREAM}"
git pull --ff-only

echo "==> Building and installing the updated Decky plugin"
exec "${ROOT_DIR}/install-decky.sh" "$@"
