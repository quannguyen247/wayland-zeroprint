#!/usr/bin/env bash
set -euo pipefail

temporary_dir="$(mktemp -d)"
trap 'rm -rf -- "$temporary_dir"' EXIT

# Verify an instantiated unit while substituting an executable that is present
# before installation. Keep the installed ExecStart path covered by grep.
grep -Fxq 'ExecStart=/usr/local/bin/wayland-zeroprint' systemd/wayland-zeroprint@.service
sed 's|ExecStart=/usr/local/bin/wayland-zeroprint|ExecStart=/usr/bin/true|' \
    systemd/wayland-zeroprint@.service >"$temporary_dir/wayland-zeroprint@.service"
cp systemd/wayland-zeroprint@.path "$temporary_dir/wayland-zeroprint@.path"
chmod 644 "$temporary_dir"/*

SYSTEMD_UNIT_PATH="$temporary_dir:/etc/systemd/system:/usr/lib/systemd/system" \
    systemd-analyze verify \
        wayland-zeroprint@1000.service \
        wayland-zeroprint@1000.path

# Upgrades must replace the old boot-time enablement with user-manager
# enablement. Exercise systemctl's real symlink migration without a live bus.
unit_dir="$temporary_dir/root/etc/systemd/system"
mkdir -p "$unit_dir/multi-user.target.wants"
install -m 644 systemd/wayland-zeroprint@.path "$unit_dir/wayland-zeroprint@.path"
cat >"$unit_dir/user@.service" <<'EOF'
[Service]
Type=oneshot
ExecStart=/usr/bin/true
RemainAfterExit=yes
EOF
ln -s ../wayland-zeroprint@.path \
    "$unit_dir/multi-user.target.wants/wayland-zeroprint@1000.path"
systemctl --root="$temporary_dir/root" reenable wayland-zeroprint@1000.path
test ! -L "$unit_dir/multi-user.target.wants/wayland-zeroprint@1000.path"
test -L "$unit_dir/user@1000.service.wants/wayland-zeroprint@1000.path"

# A fresh installation must use the same login activation path.
systemctl --root="$temporary_dir/root" disable wayland-zeroprint@1000.path
systemctl --root="$temporary_dir/root" enable wayland-zeroprint@1000.path
test ! -L "$unit_dir/multi-user.target.wants/wayland-zeroprint@1000.path"
test -L "$unit_dir/user@1000.service.wants/wayland-zeroprint@1000.path"

echo "systemd unit and login enablement verification passed."
