#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo -e "${BLUE}==>${NC} Installing wayland-zeroprint (Direct KWin Engine + Universal Backend)..."

# Check build dependencies
CC="${CC:-gcc}"
if ! command -v "$CC" >/dev/null 2>&1; then
    if command -v clang >/dev/null 2>&1; then
        CC="clang"
    else
        echo -e "${RED}[ERROR]${NC} C compiler (gcc or clang) is not installed."
        echo -e "       Please install gcc: 'sudo dnf install gcc' or 'sudo apt install gcc'."
        exit 1
    fi
fi

# Check runtime dependencies
for cmd in wl-copy; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo -e "${RED}[ERROR]${NC} Required core tool '$cmd' is not installed or not in PATH."
        echo -e "       Please install 'wl-clipboard' for your distribution."
        exit 1
    fi
done

# Check screenshot fallback backend availability
backend_found=0
for backend in spectacle grim gnome-screenshot gdbus; do
    if command -v "$backend" >/dev/null 2>&1; then
        backend_found=1
        echo -e "${GREEN}[OK]${NC} Detected fallback compositor backend: $backend"
        break
    fi
done

if [ "$backend_found" -eq 0 ]; then
    echo -e "${YELLOW}[WARNING]${NC} No common fallback screenshot backend found."
fi

# 1. Compile native C binary with direct KWin D-Bus engine and zlib
echo -e "${BLUE}==>${NC} Compiling native C daemon with $CC (-O3 -flto -march=native -lsystemd -lz)..."
mkdir -p "$HOME/.local/bin"
$CC -O3 -Wall -Wextra -pthread -flto -march=native src/main.c -lsystemd -lz -o "$HOME/.local/bin/wayland-zeroprint"
chmod 755 "$HOME/.local/bin/wayland-zeroprint"

# 2. Configure KWin Environment permission rule for permanent direct D-Bus access
mkdir -p "$HOME/.config/environment.d"
echo "KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1" > "$HOME/.config/environment.d/10-kwin-screenshot.conf"
echo -e "${GREEN}[OK]${NC} Configured KWin direct screenshot permission environment."

# 3. Install udev rule (requires root)
if [ -d "/etc/udev/rules.d" ]; then
    echo -e "${BLUE}==>${NC} Installing udev rules to /etc/udev/rules.d/ (requires sudo)..."
    sudo cp -f udev/99-wayland-zeroprint.rules /etc/udev/rules.d/99-wayland-zeroprint.rules
    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=input
    echo -e "${GREEN}[OK]${NC} Udev rules deployed and active."
fi

# 4. Install and start systemd user service
mkdir -p "$HOME/.config/systemd/user"
cp -f systemd/wayland-zeroprint.service "$HOME/.config/systemd/user/wayland-zeroprint.service"
systemctl --user daemon-reload
systemctl --user restart wayland-zeroprint.service || systemctl --user enable --now wayland-zeroprint.service

echo -e "${GREEN}[SUCCESS]${NC} wayland-zeroprint (Direct KWin Engine) is installed and active!"
echo -e "${BLUE}==>${NC} Try it out: Press PrintScreen and immediately paste (Ctrl+V) anywhere."
