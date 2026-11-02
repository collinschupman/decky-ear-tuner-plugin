#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEFAULT_DECKY_HOME="${HOME}/homebrew"
DECKY_HOME="${DECKY_HOME:-${DEFAULT_DECKY_HOME}}"
PLUGIN_DIR="${DECKY_HOME}/plugins/ear-tuner"
LEGACY_PLUGIN_DIRS=(
    "${DECKY_HOME}/plugins/audio-focus"
    "${DECKY_HOME}/plugins/asymmetrical-hearing"
)
SETTINGS_DIR="${DECKY_HOME}/settings/ear-tuner"
LEGACY_SETTINGS_DIR="${DECKY_HOME}/settings/asymmetrical-hearing"
DECKY_INSTALL_URL="https://github.com/SteamDeckHomebrew/decky-installer/releases/latest/download/install_release.sh"
NODE_VERSION="20.20.2"
PNPM_VERSION="9.15.9"
NODE_CACHE="${XDG_CACHE_HOME:-${HOME}/.cache}/ear-tuner"
BUILD_DIR="${ROOT_DIR}/native/build-current"
BACKEND_BIN="${BUILD_DIR}/decky_ear_tuner_backend"
LINK_ONLY=false
SKIP_SYSTEM_DEPS=false
SKIP_DECKY=false
RESTORE_STEAMOS_READONLY=false

restore_steamos_readonly() {
    if [[ "${RESTORE_STEAMOS_READONLY}" == true ]]; then
        if [[ "$(id -u)" -eq 0 ]]; then
            steamos-readonly enable || true
        else
            sudo steamos-readonly enable || true
        fi
        RESTORE_STEAMOS_READONLY=false
    fi
}

trap restore_steamos_readonly EXIT

while [[ $# -gt 0 ]]; do
    case "$1" in
        --link-only) LINK_ONLY=true ;;
        --skip-system-deps) SKIP_SYSTEM_DEPS=true ;;
        --skip-decky) SKIP_DECKY=true ;;
        *)
            echo "Usage: $0 [--link-only] [--skip-system-deps] [--skip-decky]" >&2
            exit 1
            ;;
    esac
    shift
done

decky_is_installed() {
    [[ -x "${DECKY_HOME}/services/PluginLoader" ]]
}

ensure_admin_access() {
    if [[ "$(id -u)" -eq 0 ]]; then
        return 0
    fi
    command -v sudo > /dev/null 2>&1 || {
        echo "ERROR: sudo is required to install system components." >&2
        return 1
    }
    if sudo -n true > /dev/null 2>&1; then
        return 0
    fi

    local password_status
    password_status="$(passwd -S "${USER}" 2> /dev/null | awk '{print $2}')"
    if [[ "${password_status}" == "NP" ]]; then
        echo "==> This account has no administrator password yet"
        echo "Create one now for SteamOS development and installation privileges."
        passwd
    fi
    sudo -v
}

missing_system_dependencies() {
    local command_name
    for command_name in cmake c++ pkg-config git curl tar pactl pw-link; do
        command -v "${command_name}" > /dev/null 2>&1 || return 0
    done
    if [[ "${SKIP_DECKY}" == false ]] && ! decky_is_installed; then
        command -v jq > /dev/null 2>&1 || return 0
    fi
    pkg-config --exists libpipewire-0.3 || return 0
    return 1
}

install_system_dependencies() {
    if [[ -n "${FLATPAK_ID:-}" ]]; then
        echo "ERROR: Native build dependencies are missing inside this Flatpak." >&2
        echo "Open Konsole on the SteamOS desktop and run ./install-decky.sh there." >&2
        return 1
    fi

    local -a elevate=()
    if [[ "$(id -u)" -ne 0 ]]; then
        ensure_admin_access
        elevate=(sudo)
    fi

    # shellcheck disable=SC1091
    . /etc/os-release
    case "${ID:-} ${ID_LIKE:-}" in
        *steamos* | *arch*)
            if command -v steamos-readonly > /dev/null 2>&1 &&
                steamos-readonly status 2> /dev/null | grep -qi enabled; then
                echo "==> Temporarily disabling the SteamOS read-only filesystem"
                "${elevate[@]}" steamos-readonly disable
                RESTORE_STEAMOS_READONLY=true
            fi
            echo "==> Installing native build and PipeWire dependencies"
            if ! "${elevate[@]}" pacman -Sy --needed --noconfirm \
                base-devel cmake pkgconf pipewire libpulse git curl jq xz; then
                return 1
            fi
            restore_steamos_readonly
            ;;
        *debian* | *ubuntu*)
            "${elevate[@]}" apt-get update
            "${elevate[@]}" apt-get install -y build-essential cmake pkg-config \
                libpipewire-0.3-dev pipewire-bin pulseaudio-utils git curl jq xz-utils
            ;;
        *fedora* | *rhel*)
            "${elevate[@]}" dnf install -y gcc-c++ make cmake pkgconf-pkg-config \
                pipewire-devel pipewire-utils pulseaudio-utils git curl jq xz
            ;;
        *opensuse* | *suse*)
            "${elevate[@]}" zypper --non-interactive install -y gcc-c++ make cmake \
                pkg-config pipewire-devel pipewire-tools pulseaudio-utils git curl jq xz
            ;;
        *)
            echo "ERROR: Unsupported distribution '${ID:-unknown}'." >&2
            echo "Install a C++ toolchain, CMake, pkg-config, PipeWire " \
                "headers/tools, PulseAudio tools, Git, curl, tar, and xz; " \
                "then rerun with --skip-system-deps." >&2
            return 1
            ;;
    esac
}

if [[ "${LINK_ONLY}" == false && "${SKIP_SYSTEM_DEPS}" == false ]] &&
    missing_system_dependencies; then
    install_system_dependencies
fi

if [[ "${LINK_ONLY}" == false ]] && missing_system_dependencies; then
    echo "ERROR: Required native build or PipeWire dependencies are still missing." >&2
    exit 1
fi

install_decky_loader() {
    if decky_is_installed; then
        echo "==> Existing Decky Loader found; leaving it unchanged"
        return 0
    fi
    if [[ "${SKIP_DECKY}" == true ]]; then
        echo "ERROR: Decky Loader is not installed at ${DECKY_HOME}." >&2
        echo "Remove --skip-decky or install Decky Loader separately." >&2
        return 1
    fi
    if [[ "${DECKY_HOME}" != "${DEFAULT_DECKY_HOME}" ]]; then
        echo "ERROR: The official Decky installer cannot bootstrap custom " \
            "DECKY_HOME paths." >&2
        echo "Install Decky there separately, then rerun with --skip-decky." >&2
        return 1
    fi
    if [[ -n "${FLATPAK_ID:-}" ]]; then
        echo "ERROR: Decky Loader must be installed from native Konsole, " \
            "not a Flatpak terminal." >&2
        return 1
    fi
    if [[ "$(id -u)" -eq 0 && -z "${SUDO_USER:-}" ]]; then
        echo "ERROR: Run this installer as the desktop user, not directly as root." >&2
        return 1
    fi

    local installer_dir installer_path
    ensure_admin_access
    installer_dir="${XDG_CACHE_HOME:-${HOME}/.cache}/ear-tuner"
    installer_path="${installer_dir}/decky-install-release.sh"
    mkdir -p "${installer_dir}"
    echo "==> Decky Loader is missing; downloading the official stable installer"
    curl -fL "${DECKY_INSTALL_URL}" -o "${installer_path}"
    sh "${installer_path}"
    decky_is_installed || {
        echo "ERROR: The official Decky installer completed without creating " \
            "PluginLoader." >&2
        return 1
    }
    echo "==> Decky Loader installed"
}

if [[ "${LINK_ONLY}" == false ]]; then
    install_decky_loader
elif ! decky_is_installed; then
    echo "ERROR: Decky Loader is missing; rerun without --link-only first." >&2
    exit 1
fi

if [[ "${LINK_ONLY}" == false ]]; then
    case "$(uname -m)" in
        x86_64)
            NODE_ARCH="x64"
            NODE_SHA256="df770b2a6f130ed8627c9782c988fda9669fa23898329a61a871e32f965e007d"
            ;;
        aarch64)
            NODE_ARCH="arm64"
            NODE_SHA256="73093db209e4e9e09dd7d15a47aeaab1b74833830df03efa5f942a1122c5fa71"
            ;;
        *)
            echo "ERROR: Unsupported architecture: $(uname -m)" >&2
            exit 1
            ;;
    esac
    NODE_DIR="${NODE_CACHE}/node-v${NODE_VERSION}-linux-${NODE_ARCH}"
    if [[ ! -x "${NODE_DIR}/bin/corepack" ]]; then
        echo "==> Downloading Node.js ${NODE_VERSION}"
        mkdir -p "${NODE_CACHE}"
        ARCHIVE="${NODE_CACHE}/node-v${NODE_VERSION}-linux-${NODE_ARCH}.tar.xz"
        curl -fL \
            "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-${NODE_ARCH}.tar.xz" \
            -o "${ARCHIVE}"
        printf '%s  %s\n' "${NODE_SHA256}" "${ARCHIVE}" | sha256sum --check
        tar -xJf "${ARCHIVE}" -C "${NODE_CACHE}"
        rm -f "${ARCHIVE}"
    fi
    export PATH="${NODE_DIR}/bin:${PATH}"
    corepack prepare "pnpm@${PNPM_VERSION}" --activate
fi

cd "${ROOT_DIR}"

if [[ "${LINK_ONLY}" == false ]]; then
    echo "==> Installing frontend dependencies"
    corepack pnpm install --frozen-lockfile

    echo "==> Type-checking and building Decky plugin"
    corepack pnpm run typecheck
    corepack pnpm run build:decky

    echo "==> Building and testing native backend"
    cmake -S native -B "${BUILD_DIR}" -DCMAKE_BUILD_TYPE=Release
    cmake --build "${BUILD_DIR}" -j"$(nproc)"
    ctest --test-dir "${BUILD_DIR}" --output-on-failure
fi

[[ -f dist/index.js ]] || {
    echo "ERROR: dist/index.js is missing. Run ./install-decky.sh without " \
        "--link-only." >&2
    exit 1
}
DECKY_SAMPLE="$(find dist/assets -maxdepth 1 -type f \
    -name 'deckySounds-*.wav' -print -quit 2> /dev/null || true)"
[[ -n "${DECKY_SAMPLE}" ]] || {
    echo "ERROR: The bundled listening sample is missing from dist/assets." >&2
    echo "Run ./install-decky.sh without --link-only to rebuild it." >&2
    exit 1
}
[[ -x "${BACKEND_BIN}" || -x native/build/decky_ear_tuner_backend ]] || {
    echo "ERROR: Native backend is missing. Run ./install-decky.sh without " \
        "--link-only." >&2
    exit 1
}

chmod +x service.sh
[[ ! -f "${BACKEND_BIN}" ]] || chmod +x "${BACKEND_BIN}"
mkdir -p "${DECKY_HOME}/plugins"

for legacy_plugin_dir in "${LEGACY_PLUGIN_DIRS[@]}"; do
    if [[ -e "${legacy_plugin_dir}" || -L "${legacy_plugin_dir}" ]]; then
        if [[ ! -L "${legacy_plugin_dir}" ]]; then
            echo "ERROR: ${legacy_plugin_dir} is not a symlink; refusing to replace it." >&2
            exit 1
        fi
        legacy_target="$(readlink -f "${legacy_plugin_dir}" || true)"
        if [[ "${legacy_target}" != "${ROOT_DIR}" ]]; then
            echo "ERROR: ${legacy_plugin_dir} links to ${legacy_target}, not this " \
                "checkout." >&2
            exit 1
        fi
        if [[ -w "${DECKY_HOME}/plugins" ]]; then
            rm -- "${legacy_plugin_dir}"
        else
            echo "==> Removing the legacy plugin link requires administrator permission"
            ensure_admin_access
            sudo rm -- "${legacy_plugin_dir}"
        fi
        echo "==> Removed legacy plugin link: ${legacy_plugin_dir}"
    fi
done

if [[ -d "${LEGACY_SETTINGS_DIR}" && ! -e "${SETTINGS_DIR}" ]]; then
    mv -- "${LEGACY_SETTINGS_DIR}" "${SETTINGS_DIR}"
    echo "==> Migrated saved settings to Ear Tuner"
fi

PLUGIN_LINK_CURRENT=false
if [[ -e "${PLUGIN_DIR}" || -L "${PLUGIN_DIR}" ]]; then
    if [[ ! -L "${PLUGIN_DIR}" ]]; then
        echo "ERROR: ${PLUGIN_DIR} exists and is not a symlink; refusing to " \
            "replace it." >&2
        exit 1
    fi
    EXISTING_TARGET="$(readlink -f "${PLUGIN_DIR}" || true)"
    if [[ "${EXISTING_TARGET}" != "${ROOT_DIR}" ]]; then
        echo "ERROR: ${PLUGIN_DIR} already links to ${EXISTING_TARGET}." >&2
        echo "Refusing to replace another development checkout." >&2
        exit 1
    fi
    PLUGIN_LINK_CURRENT=true
fi

if [[ "${PLUGIN_LINK_CURRENT}" == true ]]; then
    echo "==> Development plugin link is already current"
elif [[ -w "${DECKY_HOME}/plugins" ]]; then
    ln -sfnT "${ROOT_DIR}" "${PLUGIN_DIR}"
elif command -v sudo > /dev/null 2>&1; then
    echo "==> Decky's plugin directory requires administrator permission"
    sudo ln -sfnT "${ROOT_DIR}" "${PLUGIN_DIR}"
else
    echo "ERROR: Decky's plugin directory is not writable from this environment." >&2
    echo "Open native Konsole and run:" >&2
    echo "  cd ${ROOT_DIR} && ./install-decky.sh --link-only" >&2
    exit 1
fi

echo
echo "Installed development plugin: ${PLUGIN_DIR} -> ${ROOT_DIR}"
echo "Return to Game Mode, then reload Ear Tuner in Decky Settings."
echo "Use Decky's reload control rather than restarting plugin_loader.service directly."
