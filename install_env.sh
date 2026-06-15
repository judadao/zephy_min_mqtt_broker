#!/usr/bin/env bash
# Install Zephyr development environment for mqtt_min_broker (ESP32).
#
# Isolation guarantees:
#   - All Python packages go into a venv at $ZEPHYR_WORKSPACE/.venv
#     (nothing written to ~/.local or system site-packages).
#   - ~/.bashrc is NOT modified. A standalone env.sh is generated instead.
#
# Unavoidable global side effects (Zephyr requirement):
#   - west zephyr-export + SDK setup write to ~/.cmake/packages/Zephyr*
#     so that CMake find_package(Zephyr) works. These entries point to
#     this workspace and do not affect non-Zephyr CMake projects.
#   - SDK setup installs udev rules to /etc/udev/rules.d/ (via sudo).
#
# Safe to re-run: each step checks if already done before proceeding.
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
VENV="${ZEPHYR_WORKSPACE}/.venv"
SDK_VERSION="0.16.8"
SDK_DIR="$HOME/zephyr-sdk-${SDK_VERSION}"
SDK_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VERSION}/zephyr-sdk-${SDK_VERSION}_linux-x86_64.tar.xz"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ENV_SH="${SCRIPT_DIR}/env.sh"

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------
_info()  { echo "[install] $*"; }
_ok()    { echo "[install] OK: $*"; }
_skip()  { echo "[install] skip: $*"; }
_error() { echo "[install] error: $*" >&2; exit 1; }

_require_cmd() {
    command -v "$1" &>/dev/null || _error "'$1' not found — $2"
}

# ---------------------------------------------------------------------------
# Steps
# ---------------------------------------------------------------------------
_apt_install() {
    _info "Installing system packages..."
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends \
        git cmake ninja-build gperf wget xz-utils file make \
        gcc gcc-multilib g++-multilib \
        python3-dev python3-pip python3-venv \
        device-tree-compiler dfu-util ccache \
        libsdl2-dev libmagic1
    _ok "system packages installed."
}

_setup_venv() {
    if [ -f "$VENV/bin/activate" ]; then
        _skip "venv already exists at $VENV"
    else
        _info "Creating Python venv at $VENV ..."
        python3 -m venv "$VENV"
        _ok "venv created."
    fi
    # activate for the rest of this script only
    # shellcheck disable=SC1091
    source "$VENV/bin/activate"
}

_install_west() {
    if "$VENV/bin/python" -m west --version &>/dev/null 2>&1; then
        _skip "west already installed in venv ($("$VENV/bin/python" -m west --version))"
        return
    fi
    _info "Installing west into venv..."
    "$VENV/bin/pip" install --quiet west
    _ok "west $("$VENV/bin/python" -m west --version) installed."
}

_init_workspace() {
    if [ -f "$ZEPHYR_WORKSPACE/.west/config" ]; then
        _skip "west workspace already exists at $ZEPHYR_WORKSPACE"
    else
        _info "Initialising west workspace at $ZEPHYR_WORKSPACE ..."
        mkdir -p "$ZEPHYR_WORKSPACE"
        (cd "$ZEPHYR_WORKSPACE" && west init)
        _ok "west init done."
    fi

    _info "Running west update (this may take a while)..."
    (cd "$ZEPHYR_WORKSPACE" && west update)
    _ok "west update done."

    # NOTE: writes ~/.cmake/packages/Zephyr* — see header comment above.
    _info "Exporting Zephyr CMake package (~/.cmake/packages/Zephyr*)..."
    (cd "$ZEPHYR_WORKSPACE" && west zephyr-export)
    _ok "zephyr-export done."

    _info "Installing Zephyr Python requirements into venv..."
    "$VENV/bin/pip" install --quiet -r "$ZEPHYR_WORKSPACE/zephyr/scripts/requirements.txt"
    _ok "Python requirements installed."
}

_install_sdk() {
    if [ -f "$SDK_DIR/setup.sh" ]; then
        _skip "Zephyr SDK ${SDK_VERSION} already at $SDK_DIR"
        return
    fi

    local archive="/tmp/zephyr-sdk-${SDK_VERSION}.tar.xz"
    if [ ! -f "$archive" ]; then
        _info "Downloading Zephyr SDK ${SDK_VERSION} (~1 GB)..."
        wget -q --show-progress -O "$archive" "$SDK_URL"
    else
        _skip "archive already downloaded: $archive"
    fi

    _info "Extracting SDK to $HOME ..."
    tar -xf "$archive" -C "$HOME"
    _ok "SDK extracted to $SDK_DIR"

    # -t: only ESP32 toolchain   -h: host tools   -c: cmake user registry
    # NOTE: -c writes ~/.cmake/packages/ZephyrSdk* — see header comment above.
    _info "Running SDK setup (ESP32 toolchain only)..."
    "$SDK_DIR/setup.sh" -t xtensa-espressif_esp32_zephyr-elf -h -c
    _ok "SDK setup done."
}

_write_env_sh() {
    _info "Writing $ENV_SH ..."
    cat > "$ENV_SH" <<EOF
#!/usr/bin/env bash
# Source this file to activate the Zephyr build environment:
#   source $(basename "$ENV_SH")
#
# Nothing here modifies ~/.bashrc or ~/.local.
# Scope: current shell session only.

ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE}"
VENV="${VENV}"

if [ ! -f "\$VENV/bin/activate" ]; then
    echo "error: venv not found at \$VENV — run install_env.sh first" >&2
    return 1
fi

source "\$VENV/bin/activate"
export ZEPHYR_BASE="\${ZEPHYR_WORKSPACE}/zephyr"
source "\${ZEPHYR_WORKSPACE}/zephyr/zephyr-env.sh"

echo "[env] ZEPHYR_BASE : \$ZEPHYR_BASE"
echo "[env] west        : \$(west --version)"
echo "[env] python      : \$(python --version)"
echo "[env] Ready."
EOF
    chmod +x "$ENV_SH"
    _ok "env.sh written."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
_info "=== Zephyr ESP32 environment installer ==="
_info "Workspace : $ZEPHYR_WORKSPACE"
_info "Venv      : $VENV"
_info "SDK       : $SDK_DIR (v${SDK_VERSION})"
echo

_require_cmd python3 "install python3"
_require_cmd sudo    "script requires sudo for apt"

_apt_install
_setup_venv
_install_west
_init_workspace
_install_sdk
_write_env_sh

echo
_info "=== Installation complete ==="
echo
echo "  Activate the environment (current shell only):"
echo "    source ${ENV_SH}"
echo
echo "  Then build:"
echo "    ./setup.sh all"
