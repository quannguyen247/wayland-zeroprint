#!/usr/bin/env bash
set -euo pipefail

RED='\033[0;31m'
GREEN='\033[0;32m'
BLUE='\033[0;34m'
YELLOW='\033[0;33m'
NC='\033[0m'

echo -e "${BLUE}==>${NC} Installing wayland-zeroprint (Direct KWin Engine + Universal Backend)..."

install_uid=$(id -u)
install_user=$(id -un)
if [ "$install_uid" -eq 0 ]; then
    echo -e "${RED}[ERROR]${NC} Run this installer as the desktop user, not as root."
    exit 1
fi
if id -nG "$install_user" | tr ' ' '\n' | grep -qx input; then
    echo -e "${RED}[ERROR]${NC} $install_user belongs to the input group, which grants session-wide evdev access."
    echo -e "       Review other tools that use this group and remove membership yourself before installing."
    exit 1
fi
if id -nG | tr ' ' '\n' | grep -qx input; then
    echo -e "${RED}[ERROR]${NC} This login session still has the input group from an earlier group list."
    echo -e "       Log out and back in, then rerun the installer to complete isolation."
    exit 1
fi

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

if ! command -v pkg-config >/dev/null 2>&1 ||
   ! command -v wayland-scanner >/dev/null 2>&1 ||
   ! pkg-config --exists wayland-client wayland-protocols; then
    echo -e "${RED}[ERROR]${NC} Missing Wayland build dependencies."
    echo -e "       Install pkg-config, wayland-devel/libwayland-dev, and wayland-protocols-devel/wayland-protocols."
    exit 1
fi

# Native data-control is preferred. wl-copy is only a compatibility fallback.
if ! command -v wl-copy >/dev/null 2>&1; then
    echo -e "${YELLOW}[WARNING]${NC} 'wl-copy' is missing."
    echo -e "          Native clipboard works on KWin 6.6+; older/non-data-control compositors may need wl-clipboard."
fi

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
echo -e "${BLUE}==>${NC} Compiling native C daemon with $CC (-O3 -flto -march=native)..."
make clean
make CC="$CC"
sudo install -d /usr/local/bin
sudo install -m 755 build/wayland-zeroprint /usr/local/bin/wayland-zeroprint

# 2. Configure KWin Environment permission rule for permanent direct D-Bus access
mkdir -p "$HOME/.config/environment.d"
echo "KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1" > "$HOME/.config/environment.d/10-kwin-screenshot.conf"
echo -e "${GREEN}[OK]${NC} Configured KWin direct screenshot permission environment."

# Install a safe default config without overwriting user customizations.
mkdir -p "$HOME/.config/wayland-zeroprint"
if [ ! -e "$HOME/.config/wayland-zeroprint/config" ]; then
    cp config/wayland-zeroprint.conf "$HOME/.config/wayland-zeroprint/config"
    chmod 644 "$HOME/.config/wayland-zeroprint/config"
fi
echo -e "${GREEN}[OK]${NC} Trigger config: $HOME/.config/wayland-zeroprint/config"

# 3. Install udev rule (requires root)
if [ -d "/etc/udev/rules.d" ]; then
    echo -e "${BLUE}==>${NC} Installing udev rules to /etc/udev/rules.d/ (requires sudo)..."
    sudo cp -f udev/99-wayland-zeroprint.rules /etc/udev/rules.d/99-wayland-zeroprint.rules
    sudo udevadm control --reload-rules
    sudo udevadm trigger --subsystem-match=input
    echo -e "${GREEN}[OK]${NC} Udev rules deployed and active."
fi

# 4. Give evdev access to the service without changing the account's groups.

systemctl --user disable --now wayland-zeroprint.service 2>/dev/null || true
rm -f "$HOME/.config/systemd/user/wayland-zeroprint.service"
rm -f "$HOME/.local/bin/wayland-zeroprint"
systemctl --user daemon-reload

sudo install -d /etc/systemd/system
sudo install -m 644 systemd/wayland-zeroprint@.service /etc/systemd/system/wayland-zeroprint@.service
sudo install -m 644 systemd/wayland-zeroprint@.path /etc/systemd/system/wayland-zeroprint@.path

sudo systemctl daemon-reload
sudo systemctl enable --now "wayland-zeroprint@${install_uid}.path"
if [ -S "/run/user/${install_uid}/wayland-0" ]; then
    sudo systemctl restart "wayland-zeroprint@${install_uid}.service"
fi

echo -e "${GREEN}[SUCCESS]${NC} wayland-zeroprint (Direct KWin Engine) is installed and active!"
echo -e "${BLUE}==>${NC} Try it out: press PrintScreen and immediately paste (Ctrl+V) anywhere."
echo -e "${BLUE}==>${NC} Custom trigger example: triggers=PRINT,CTRL+ALT+P"
