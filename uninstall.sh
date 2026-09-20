#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
NC='\033[0m'

echo -e "${BLUE}==>${NC} Uninstalling wayland-zeroprint..."

# Stop and disable both the current system service and the legacy user service.
install_uid=$(id -u)
sudo systemctl disable --now "wayland-zeroprint@${install_uid}.path" 2>/dev/null || true
sudo systemctl stop "wayland-zeroprint@${install_uid}.service" 2>/dev/null || true
sudo rm -f /etc/systemd/system/wayland-zeroprint@.service
sudo rm -f /etc/systemd/system/wayland-zeroprint@.path
sudo systemctl daemon-reload

if systemctl --user is-active --quiet wayland-zeroprint.service 2>/dev/null; then
    systemctl --user stop wayland-zeroprint.service
fi
if systemctl --user is-enabled --quiet wayland-zeroprint.service 2>/dev/null; then
    systemctl --user disable wayland-zeroprint.service
fi
rm -f "$HOME/.config/systemd/user/wayland-zeroprint.service"
systemctl --user daemon-reload

# Remove both current and legacy binary locations.
sudo rm -f /usr/local/bin/wayland-zeroprint
rm -f "$HOME/.local/bin/wayland-zeroprint"
rm -f "$HOME/.config/environment.d/10-kwin-screenshot.conf"
echo -e "${YELLOW}[INFO]${NC} Preserved user config: $HOME/.config/wayland-zeroprint/config"

# Remove udev rule
if [ -f "/etc/udev/rules.d/99-wayland-zeroprint.rules" ]; then
    echo -e "${BLUE}==>${NC} Removing udev rule (requires sudo)..."
    sudo rm -f /etc/udev/rules.d/99-wayland-zeroprint.rules
    sudo udevadm control --reload-rules
fi

echo -e "${GREEN}[OK]${NC} wayland-zeroprint was removed; your user config was preserved."
