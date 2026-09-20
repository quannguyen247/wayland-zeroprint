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

echo "systemd unit verification passed."
