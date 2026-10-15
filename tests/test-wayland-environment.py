#!/usr/bin/env python3
"""Exercise real UNIX socket selection without input access or screenshots."""
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

binary = str(Path(sys.argv[1]).resolve())
with tempfile.TemporaryDirectory(prefix="zeroprint-sockets-") as directory:
    runtime = Path(directory)
    environment = dict(os.environ, XDG_RUNTIME_DIR=directory)
    # An unavailable bus ensures explicit displays do not need systemd.
    environment["DBUS_SESSION_BUS_ADDRESS"] = f"unix:path={directory}/absent-bus"
    sockets = []
    try:
        for name in ("wayland-0", "wayland-1", "wayland-10", "custom-display"):
            server = socket.socket(socket.AF_UNIX)
            server.bind(str(runtime / name))
            sockets.append(server)
        (runtime / "wayland-1.lock").touch()
        for display in ("wayland-0", "wayland-1", "wayland-10", "custom-display",
                        str(runtime / "wayland-1")):
            result = subprocess.run([binary, "--print-wayland-display"],
                                    env=dict(environment, WAYLAND_DISPLAY=display),
                                    text=True, capture_output=True)
            assert result.returncode == 0, result.stderr
            assert result.stdout.strip() == display, result.stdout
        for display in ("wayland-1.lock", "wayland-2"):
            result = subprocess.run([binary, "--print-wayland-display"],
                                    env=dict(environment, WAYLAND_DISPLAY=display),
                                    text=True, capture_output=True)
            assert result.returncode == 1, result.stdout
            assert "not a socket owned by this UID" in result.stderr, result.stderr
        environment.pop("WAYLAND_DISPLAY", None)
        result = subprocess.run([binary, "--print-wayland-display"],
                                env=environment, text=True, capture_output=True)
        assert result.returncode == 1, result.stdout
        assert "cannot read session environment" in result.stderr, result.stderr
    finally:
        for server in sockets:
            server.close()
print("Wayland socket/environment tests passed.")
