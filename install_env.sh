#!/usr/bin/env bash
# Install Zephyr development environment for mqtt_min_broker (ESP32).
# Safe to re-run: each step checks if already done before proceeding.
set -euo pipefail

# ---------------------------------------------------------------------------
# Configuration
# ---------------------------------------------------------------------------
ZEPHYR_WORKSPACE="${ZEPHYR_WORKSPACE:-$HOME/zephyrproject}"
SDK_VERSION="0.16.8"
SDK_DIR="$HOME/zephyr-sdk-${SDK_VERSION}"
SDK_URL="https://github.com/zephyrproject-rtos/sdk-ng/releases/download/v${SDK_VERSION}/zephyr-sdk-${SDK_VERSION}_linux-x86_64.tar.xz"
SHELL_RC="${HOME}/.bashrc"

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

_apt_install() {
    _info "Installing system packages..."
    sudo apt-get update -qq
    sudo apt-get install -y --no-install-recommends \
        git cmake ninja-build gperf wget xz-utils file make \
        gcc gcc-multilib g++-multilib \
        python3-dev python3-pip python3-setuptools python3-venv \
        device-tree-compiler dfu-util ccache \
        libsdl2-dev libmagic1
    _ok "system packages installed."
}

_install_west() {
    if command -v west &>/dev/null; then
        _skip "west already installed ($(west --version))"
        return
    fi
    _info "Installing west..."
    pip3 install --user west
    # make sure ~/.local/bin is in PATH for the rest of this script
    export PATH="$HOME/.local/bin:$PATH"
    _ok "west $(west --version) installed."
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

    _info "Exporting Zephyr CMake package..."
    (cd "$ZEPHYR_WORKSPACE" && west zephyr-export)
    _ok "zephyr-export done."

    _info "Installing Zephyr Python requirements..."
    pip3 install --user -r "$ZEPHYR_WORKSPACE/zephyr/scripts/requirements.txt"
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

    _info "Running SDK setup (installs udev rules + cmake packages)..."
    "$SDK_DIR/setup.sh" -t xtensa-espressif_esp32_zephyr-elf -h -c
    _ok "SDK setup done."
}

_write_env_snippet() {
    local marker="# >>> zephyr-env (mqtt_min_broker) <<<"
    if grep -qF "$marker" "$SHELL_RC" 2>/dev/null; then
        _skip "env snippet already in $SHELL_RC"
        return
    fi

    _info "Adding Zephyr env to $SHELL_RC ..."
    cat >> "$SHELL_RC" <<EOF

${marker}
export ZEPHYR_BASE="${ZEPHYR_WORKSPACE}/zephyr"
export PATH="\$HOME/.local/bin:\$PATH"
# source ${ZEPHYR_WORKSPACE}/zephyr/zephyr-env.sh  # uncomment for full env
# <<< zephyr-env <<<
EOF
    _ok "env snippet written. Run 'source ~/.bashrc' or open a new terminal."
}

# ---------------------------------------------------------------------------
# Main
# ---------------------------------------------------------------------------
_info "=== Zephyr ESP32 environment installer ==="
_info "Workspace : $ZEPHYR_WORKSPACE"
_info "SDK       : $SDK_DIR (v${SDK_VERSION})"
echo

_require_cmd python3 "install python3"
_require_cmd pip3    "install python3-pip"
_require_cmd sudo    "script requires sudo for apt"

_apt_install
_install_west
_init_workspace
_install_sdk
_write_env_snippet

echo
_info "=== Installation complete ==="
echo
echo "  Next steps:"
echo "    1. source ~/.bashrc          (or open a new terminal)"
echo "    2. source ${ZEPHYR_WORKSPACE}/zephyr/zephyr-env.sh"
echo "    3. cd $(dirname "$(realpath "$0")")"
echo "    4. ./setup.sh all            (verify env + build)"
