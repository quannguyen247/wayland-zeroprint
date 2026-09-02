#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}==>${NC} Uninstalling wayland-zeroprint..."

# Stop and disable systemd service
if systemctl --user is-active --quiet wayland-zeroprint.service 2>/dev/null; then
    systemctl --user stop wayland-zeroprint.service
fi
if systemctl --user is-enabled --quiet wayland-zeroprint.service 2>/dev/null; then
    systemctl --user disable wayland-zeroprint.service
fi
rm -f "$HOME/.config/systemd/user/wayland-zeroprint.service"
systemctl --user daemon-reload

# Remove binary
rm -f "$HOME/.local/bin/wayland-zeroprint"
echo -e "${YELLOW}[INFO]${NC} Preserved user config: $HOME/.config/wayland-zeroprint/config"

# Remove udev rule
if [ -f "/etc/udev/rules.d/99-wayland-zeroprint.rules" ]; then
    echo -e "${BLUE}==>${NC} Removing udev rule (requires sudo)..."
    sudo rm -f /etc/udev/rules.d/99-wayland-zeroprint.rules
    sudo udevadm control --reload-rules
fi

echo -e "${GREEN}[OK]${NC} wayland-zeroprint was removed; your user config was preserved."
