#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo -e "${BLUE}==>${NC} Installing wayland-zeroprint..."

# Check core dependencies
for cmd in python3 wl-copy; do
    if ! command -v "$cmd" >/dev/null 2>&1; then
        echo -e "${RED}[ERROR]${NC} Required core tool '$cmd' is not installed or not in PATH."
        echo -e "       Please install 'python3' and 'wl-clipboard' for your distribution."
        exit 1
    fi
done

# Check screenshot backend availability
backend_found=0
for backend in spectacle grim gnome-screenshot gdbus; do
    if command -v "$backend" >/dev/null 2>&1; then
        backend_found=1
        echo -e "${GREEN}[OK]${NC} Detected screenshot backend: $backend"
        break
    fi
done

if [ "$backend_found" -eq 0 ]; then
    echo -e "${YELLOW}[WARNING]${NC} No common screenshot backend found (spectacle, grim, gnome-screenshot, or gdbus)."
    echo -e "          Please install 'spectacle' (KDE), 'grim' (wlroots/Hyprland), or 'gnome-screenshot' (GNOME)."
fi

# 1. Install binary
mkdir -p "$HOME/.local/bin"
cp -f bin/wayland-zeroprint "$HOME/.local/bin/wayland-zeroprint"
chmod +x "$HOME/.local/bin/wayland-zeroprint"
echo -e "${GREEN}[OK]${NC} Installed binary to ~/.local/bin/wayland-zeroprint"

# 2. Install udev rule (requires root)
if [ -d "/etc/udev/rules.d" ]; then
    echo -e "${BLUE}==>${NC} Installing udev rules to /etc/udev/rules.d/ (requires sudo)..."
    sudo cp -f udev/99-wayland-zeroprint.rules /etc/udev/rules.d/99-wayland-zeroprint.rules
    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=input
    echo -e "${GREEN}[OK]${NC} Udev rules deployed and active."
fi

# 3. Install and start systemd user service
mkdir -p "$HOME/.config/systemd/user"
cp -f systemd/wayland-zeroprint.service "$HOME/.config/systemd/user/wayland-zeroprint.service"
systemctl --user daemon-reload
systemctl --user enable --now wayland-zeroprint.service

echo -e "${GREEN}[SUCCESS]${NC} wayland-zeroprint is installed and running!"
echo -e "${BLUE}==>${NC} Try it out: Press PrintScreen and immediately paste (Ctrl+V) anywhere."

