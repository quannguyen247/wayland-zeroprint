# wayland-zeroprint

**Zero-drop, hardware-level PrintScreen daemon for Linux Wayland compositors (KDE Plasma 6, GNOME, wlroots).**

`wayland-zeroprint` captures lossless screenshots for the Wayland clipboard by listening to Linux `evdev` key events and delegating frame capture to the compositor. On KWin it requests the compositor's native-resolution workspace frame, so fractional scaling does not turn a 1536×864 logical capture into a blurred upscale or an incorrectly sized 1920×1080 area.

---

## The Motivation: Reliable, Zero-Config Screenshots on Wayland

On modern Linux Wayland desktop environments (**KDE Plasma 6**, **GNOME Shell**, and **wlroots**), capturing seamless screenshots often presents practical challenges:

1. **User-Space Shortcut Dispatcher Drops**:
   When interacting with complex desktop states (system tray popups, dropdown context menus, fullscreen games, or legacy XWayland windows), user-space shortcut managers (such as KDE's `kglobalaccel`) can experience latency or occasionally drop non-modifier keys like `PrintScreen`.

2. **Transient UI Dismissal**:
   Triggering full GUI screenshot utilities or dispatching desktop notifications can disrupt active `xdg_popup` surface states, closing dropdowns or tooltips before the frame can be grabbed.

3. **Multi-Compositor Fragmentation**:
   Every desktop environment requires different shortcut configurations and backends. `wayland-zeroprint` provides a single, unified daemon that auto-detects your compositor and works out-of-the-box everywhere.

---

## The Solution: Hardware-Level evdev Pipeline

`wayland-zeroprint` operates directly below the desktop environment layer by attaching to the Linux kernel input event subsystem (`/dev/input/event*`):

```
┌─────────────────────────┐
│    Physical Keyboard    │
└───────────┬─────────────┘
            │  Hardware Interrupt (KEY_SYSRQ / KEY_PRINT)
            ▼
┌─────────────────────────┐
│ Linux Kernel: evdev subsystem  │
│   (/dev/input/event*)   │
└───────────┬─────────────┘
            │  epoll_wait() (idle until the key event arrives)
            ▼
┌─────────────────────────────────────────────────────────────┐
│                 wayland-zeroprint Daemon                    │
│                                                             │
│  - Kernel-level hardware trigger (independent of desktop shortcuts) │
│  - Silent execution (no notifications, popup-safe)          │
│  - Auto-detects compositor: KDE / GNOME / wlroots           │
└──────┬───────────────────────────────────────────────┬──────┘
       │                                               │
       ▼                                               ▼
┌─────────────────────────────┐         ┌─────────────────────────────┐
│  Compositor Frame Grab      │         │   Asynchronous Streamer     │
│  Native raw frame + PNG    │ ──────> │   Pipes complete PNG to      │
│  /dev/shm tmpfs, atomic    │         │   wl-copy clipboard          │
└─────────────────────────────┘         └──────────────┬──────────────┘
                                                       │
                                                       ▼
                                        ┌─────────────────────────────┐
                                        │  Wayland Clipboard Memory   │
                                        │  (Ready for instant Ctrl+V) │
                                        └─────────────────────────────┘
```

### Core Architectural Features

* **Kernel-Level Hardware Trigger**: Listens for `KEY_SYSRQ (99)` and `KEY_PRINT (210)` directly at the kernel driver layer. Immune to user-space shortcut dispatcher dropouts and XWayland focus quirks.
* **KWin native-resolution capture**: Calls `org.kde.KWin.ScreenShot2.CaptureWorkspace` with `native-resolution=true`; it does not guess physical dimensions from DRM or misuse logical `CaptureArea` coordinates.
* **Correct raw-frame transport**: Receives KWin's asynchronous raw QImage stream through a pipe, validates its metadata, drains the complete frame before PNG compression, and handles QImage pixel formats explicitly.
* **Atomic tmpfs output**: Writes a per-process temporary PNG under `/dev/shm`, then renames it to `/dev/shm/wayland_zeroprint.png`; clipboard readers never see a partially written image.
* **Pure Silent Execution**: Dispatches no desktop notifications and creates no focus stealing windows, ensuring open dropdowns, taskbar menus, and tooltips stay open on screen.
* **Universal Compositor Support**: Features automated detection with seamless fallbacks (`gdbus` on GNOME, `grim` on wlroots/Hyprland, and `spectacle` fallback on KDE).

---

## Deep Dive: The 4-Stage Execution Pipeline

```
[ Physical Keypress ] ──> [ Kernel evdev epoll ] ──> [ Async pthread Worker ]
                                                                                       │
                                                          (KWin D-Bus + native-resolution)
                                                                                       │
[ Clipboard Ready ] <────── [ atomic PNG + wl-copy ] <──────────────────────────────────┘
```

1. **Stage 1: Kernel evdev event**
   The daemon watches `/dev/input/event*` with `epoll_wait()` and wakes when the kernel reports `KEY_SYSRQ` or `KEY_PRINT`. The exact wake-up time depends on the kernel, device and scheduler; it is measured locally rather than promised as a fixed number.

2. **Stage 2: Asynchronous Worker Non-Blocking Dispatch**
   Upon receiving the keydown event, the main listener signals a dedicated worker and immediately returns to listening. Frame capture and PNG work do not block the input loop.

3. **Stage 3: Direct KWin D-Bus Frame Sink & Pure C PNG Encoder**
   On KDE Plasma 6, the worker calls `org.kde.KWin.ScreenShot2.CaptureWorkspace` with `native-resolution=true`. KWin returns raw QImage metadata and pixels through a pipe. The worker drains the complete frame first, converts supported QImage formats to RGBA, and compresses it with zlib.

4. **Stage 4: Atomic clipboard publication**
   The completed PNG is atomically renamed into `/dev/shm/wayland_zeroprint.png`, then streamed into `wl-copy -t image/png`. Temporary files are removed on failure.

---

## Benchmarks & Performance Comparison

| Metric | Standard GUI Screenshot | wayland-zeroprint (Native C + Direct KWin) |
| :--- | :--- | :--- |
| **Key handling** | Desktop-dependent | `evdev` + `epoll` + worker thread |
| **KWin capture** | Desktop-dependent | Native-resolution raw frame via `ScreenShot2` |
| **Output** | Utility-dependent | Atomic PNG in `/dev/shm`, then `wl-copy` |
| **Measured example** | Not measured here | 20–48 ms observed for capture + PNG on 1920×1080 at scale 1.25; content and system load affect the result |
| **Idle behavior** | N/A | Sleeps in `epoll_wait()` |

---

## Supported Environments

* **Linux Kernels**: 5.x, 6.x+ with standard `evdev` support.
* **Compositors & Desktop Environments**:
  * **KDE Plasma 6** (KWin Wayland)
  * **GNOME 40+** (Mutter Wayland)
  * **Hyprland, Sway, Wayfire** (wlroots)
* **Distributions Tested**: AlmaLinux 9/10, RHEL 9/10, Fedora 39/40/41, Arch Linux, Ubuntu 22.04/24.04, Debian 12.

---

## Dependencies

### Core Requirements
* `gcc` or `clang` (C compiler toolchain)
* `libsystemd` (`systemd-devel` / `libsystemd-dev`)
* `zlib` (`zlib-devel` / `zlib1g-dev`)
* `wl-clipboard` (`wl-copy`)

### Fallback Screenshot Backends
* **KDE Plasma**: Built-in Direct KWin D-Bus (with `spectacle` as fallback)
* **GNOME**: `gdbus` (included with GNOME) or `gnome-screenshot`
* **wlroots / Hyprland / Sway**: `grim`

#### Package Installation Commands

* **Fedora / AlmaLinux / RHEL / CentOS**:
  ```bash
  sudo dnf install gcc systemd-devel zlib-devel wl-clipboard
  # Optional fallback backends:
  sudo dnf install spectacle grim
  ```

* **Arch Linux / Manjaro**:
  ```bash
  sudo pacman -S gcc systemd zlib wl-clipboard
  # Optional fallback backends:
  sudo pacman -S spectacle grim
  ```

* **Ubuntu / Debian**:
  ```bash
  sudo apt install gcc libsystemd-dev zlib1g-dev wl-clipboard
  # Optional fallback backends:
  sudo apt install spectacle gnome-screenshot grim
  ```

---

## Installation

### Method 1: Automated Installer (Recommended)

1. Clone the repository:
   ```bash
   git clone https://github.com/quannguyen247/wayland-zeroprint.git
   cd wayland-zeroprint
   ```

2. Run the installation script:
   ```bash
   ./install.sh
   ```

### Method 2: Makefile

```bash
# Build native binary, install systemd user service, and configure udev rules
make install
```

---

## Configuration: Disabling Conflicting Shortcuts

To ensure the desktop environment does not intercept `PrintScreen` with its default GUI editor:

* **KDE Plasma**: Open **System Settings** → **Shortcuts** → **Spectacle**, and set the shortcut for `Launch Spectacle` or `Capture Entire Desktop` to **None** or another key combination.
* **GNOME**: Open **Settings** → **Keyboard** → **Keyboard Shortcuts** → **Screenshots**, and adjust default bindings if needed.

---

## Verification & Status

Check the status of the background user service:
```bash
systemctl --user status wayland-zeroprint.service
```

Verify that it is enabled across system reboots:
```bash
systemctl --user is-enabled wayland-zeroprint.service
```

---

## Uninstallation

To completely remove `wayland-zeroprint` and its udev rules:

```bash
./uninstall.sh
# Or with Makefile:
make uninstall
```

## Security & Volatility Design

`wayland-zeroprint` is engineered with explicit memory and persistence principles to balance speed and data privacy:

* **In-Memory Volatility**: Captures reside exclusively in `/dev/shm` (Linux tmpfs physical RAM). Data is never committed to persistent disk blocks, eliminating residual image artifacts on physical SSD/NVMe flash storage and vanishing entirely across system reboots.
* **Atomic Overwriting Buffer**: Successive screenshot triggers atomically overwrite `/dev/shm/wayland_zeroprint.png`, preventing historical image buildup or unmonitored disk accumulation.
* **Least-Privilege Execution**: The background daemon executes entirely within user space under standard user credentials, relying on localized `uaccess` rules for `/dev/input` and standard session D-Bus endpoints.

---

## Architectural Trade-offs & Limitations

Operating across the kernel `evdev` driver and compositor D-Bus layers introduces specific architectural and security trade-offs:

1. **Hardware Scancodes vs. Software Keymaps**:
   The daemon binds directly to kernel-level input scancodes (`KEY_SYSRQ` 99, `KEY_PRINT` 210). Software-level key remapping configured in Desktop Environments (e.g. XKB layout remapping, desktop shortcut overrides, or virtual/touchscreen keyboards) will not trigger the daemon.
2. **KWin Permission Check Bypass (`KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1`)**:
   The installer provisions `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1` so the user-session D-Bus capture can run without an interactive permission prompt. This is a security-sensitive setting and should only be enabled for a trusted local user session; it is not a latency guarantee.
3. **Application Privacy Hooks**:
   Direct hardware interception bypasses user-space application lifecycle hooks that may attempt to obscure sensitive content (such as password managers or secure messaging windows) prior to a screenshot request.
4. **Mixed-monitor scale**:
   KWin's native-resolution workspace capture uses one canvas scale. A single flat PNG cannot be a simultaneous 1:1 physical-pixel representation for outputs configured with different fractional scales; for that setup, capture a specific output with a compositor-aware backend.
5. **Workaround Nature vs. Upstream Fixes**:
   This daemon acts as a lightweight, independent workaround for desktop environments experiencing shortcut dispatcher drops over complex UI surfaces (XWayland windows, transient menus). For native desktop integration, resolving shortcut dispatcher behavior directly in upstream compositors remains the ideal architectural solution.

---

## License

This project is licensed under the **Apache License 2.0**. See the [LICENSE](LICENSE) file for complete terms.

Copyright (c) 2026 **Nguyen Dong Quan** (<nguyendongquan247@gmail.com>).
