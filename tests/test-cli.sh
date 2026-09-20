#!/usr/bin/env bash
set -euo pipefail

binary="${1:-build/wayland-zeroprint}"
temporary_dir="$(mktemp -d)"
trap 'rm -rf -- "$temporary_dir"' EXIT

"$binary" --self-test

cat >"$temporary_dir/config" <<'EOF'
triggers=CTRL+ALT+P,F12
consume_kde_shortcuts=false
allow_gui_fallback=false
output_mode=both
save_path=~/Pictures/Zeroprint-%Y-{ms}.png
EOF

output="$($binary --config "$temporary_dir/config" --print-config)"
grep -Fq 'CTRL+ALT+P (evdev=25, KDE-consumable)' <<<"$output"
grep -Fq 'F12 (evdev=88, KDE-consumable)' <<<"$output"
grep -Fq 'Consume KDE shortcuts: false' <<<"$output"
grep -Fq 'Output mode: both' <<<"$output"
grep -Fq 'Save path: ~/Pictures/Zeroprint-%Y-{ms}.png' <<<"$output"

output="$($binary --output file --save-path "$temporary_dir/captures/" --print-config)"
grep -Fq 'Output mode: file' <<<"$output"
grep -Fq "Save path: $temporary_dir/captures/" <<<"$output"

if "$binary" --trigger CTRL+NO_SUCH_KEY --print-config >/dev/null 2>&1; then
    echo "invalid trigger was accepted" >&2
    exit 1
fi

cat >"$temporary_dir/bad-config" <<'EOF'
unknown_setting=true
EOF
if "$binary" --config "$temporary_dir/bad-config" --print-config >/dev/null 2>&1; then
    echo "unknown config option was accepted" >&2
    exit 1
fi

echo "CLI/config integration tests passed."
