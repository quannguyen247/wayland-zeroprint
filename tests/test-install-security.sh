#!/usr/bin/env bash
set -euo pipefail

temporary_dir="$(mktemp -d)"
trap 'rm -rf -- "$temporary_dir"' EXIT

cat >"$temporary_dir/id" <<'EOF'
#!/usr/bin/env bash
case "${1-}" in
    -u)  echo 1000 ;;
    -un) echo tester ;;
    -nG)
        if [ "${2-}" = tester ]; then
            [ "${MOCK_ACCOUNT_INPUT:-0}" = 1 ] && echo "tester input" || echo "tester"
        else
            [ "${MOCK_SESSION_INPUT:-0}" = 1 ] && echo "tester input" || echo "tester"
        fi
        ;;
    *) exec /usr/bin/id "$@" ;;
esac
EOF
chmod 755 "$temporary_dir/id"

# A pre-existing account membership is a conflict the project did not create.
# The installer must stop before build, sudo, udev, or service changes.
if PATH="$temporary_dir:$PATH" MOCK_ACCOUNT_INPUT=1 ./install.sh \
        >"$temporary_dir/install.log" 2>&1; then
    echo "install.sh accepted a login account with session-wide input access" >&2
    exit 1
fi
grep -Fq 'belongs to the input group' "$temporary_dir/install.log"

# Stale supplementary groups survive until logout. Fail before any install
# steps so a successful return cannot imply that this session is isolated.
if PATH="$temporary_dir:$PATH" MOCK_SESSION_INPUT=1 \
        make --no-print-directory install-security INSTALL_USER=tester INSTALL_UID=1000 \
        >"$temporary_dir/make.log" 2>&1; then
    echo "make accepted a login session that still has input access" >&2
    exit 1
fi
grep -Fq 'log out and back in before installing' "$temporary_dir/make.log"

cat >"$temporary_dir/make" <<EOF
#!/usr/bin/env bash
touch "$temporary_dir/make.called"
exit 99
EOF
cat >"$temporary_dir/sudo" <<EOF
#!/usr/bin/env bash
touch "$temporary_dir/sudo.called"
exit 99
EOF
chmod 755 "$temporary_dir/make" "$temporary_dir/sudo"
if PATH="$temporary_dir:$PATH" MOCK_SESSION_INPUT=1 ./install.sh \
        >"$temporary_dir/stale-session.log" 2>&1; then
    echo "install.sh accepted a login session that still has input access" >&2
    exit 1
fi
grep -Fq 'Log out and back in' "$temporary_dir/stale-session.log"
test ! -e "$temporary_dir/make.called"
test ! -e "$temporary_dir/sudo.called"

if grep -qs 'gpasswd' Makefile install.sh uninstall.sh; then
    echo "installer must not alter pre-existing account group memberships" >&2
    exit 1
fi

echo "installer security-boundary tests passed."
