# wayland-zeroprint

**Zero-drop, hardware-level PrintScreen daemon for Linux Wayland compositors (KDE Plasma 6, GNOME, wlroots).**

`wayland-zeroprint` captures lossless screenshots for the Wayland clipboard by listening to Linux `evdev` key events and delegating frame capture to the compositor. On KWin it requests the compositor's native-resolution workspace frame, so fractional scaling does not turn a 1536×864 logical capture into a blurred upscale or an incorrectly sized 1920×1080 area.

---

## The Motivation: Reliable, Low-Friction Screenshots on Wayland

On modern Linux Wayland desktop environments (**KDE Plasma 6**, **GNOME Shell**, and **wlroots**), capturing seamless screenshots often presents practical challenges:

1. **User-Space Shortcut Dispatcher Drops**:
   When interacting with complex desktop states (system tray popups, dropdown context menus, fullscreen games, or legacy XWayland windows), user-space shortcut managers (such as KDE's `kglobalaccel`) can experience latency or occasionally drop non-modifier keys like `PrintScreen`.

2. **Transient UI Dismissal**:
   Triggering full GUI screenshot utilities or dispatching desktop notifications can disrupt active `xdg_popup` surface states, closing dropdowns or tooltips before the frame can be grabbed.

3. **Multi-Compositor Fragmentation**:
   Every desktop environment requires different shortcut configurations and capture backends. `wayland-zeroprint` keeps one evdev listener and selects the compositor-specific path at runtime; the native KDE path and the GNOME/wlroots fallbacks are documented below.

---

## The Solution: Hardware-Level evdev Pipeline

`wayland-zeroprint` operates directly below the desktop environment layer by attaching to the Linux kernel input event subsystem (`/dev/input/event*`):

```
┌─────────────────────────┐
│    Physical Keyboard    │
└───────────┬─────────────┘
            │  Hardware interrupt (configured evdev key/chord)
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
│  /dev/shm tmpfs, atomic    │         │   native data-control        │
└─────────────────────────────┘         └──────────────┬──────────────┘
                                                       │
                                                       ▼
                                        ┌─────────────────────────────┐
                                        │  Wayland Clipboard Memory   │
                                        │  (Ready for instant Ctrl+V) │
                                        └─────────────────────────────┘
```

### Core Architectural Features

* **Configurable hardware triggers**: Listens for exact physical evdev chords such as `PRINT`, `F12`, or `CTRL+ALT+P`. `PRINT` covers both common kernel codes, `KEY_SYSRQ (99)` and `KEY_PRINT (210)`.
* **Exact modifier semantics**: A plain `PRINT` binding does not also claim `Shift+Print`, `Alt+Print`, or `Meta+Print`. Modifier state is tracked per input device and recovered after evdev queue overruns and hot-plug events.
* **KDE compositor-side consume without keyboard grabbing**: On KDE, the configured chord is also registered with KGlobalAccel. KWin consumes only that exact shortcut while the always-on evdev path still starts capture. The daemon never uses `EVIOCGRAB` and never forwards the rest of the keyboard through `uinput`.
* **KWin native-resolution capture**: Calls `org.kde.KWin.ScreenShot2.CaptureWorkspace` with `native-resolution=true`; it does not guess physical dimensions from DRM or misuse logical `CaptureArea` coordinates.
* **Correct raw-frame transport**: Receives KWin's asynchronous raw QImage stream through a pipe, validates its metadata, drains the complete frame before PNG compression, and handles QImage pixel formats explicitly.
* **Atomic tmpfs output**: Writes a per-process temporary PNG under `/dev/shm`, then renames it to `/dev/shm/wayland_zeroprint.png`; clipboard readers never see a partially written image.
* **Focus-free native clipboard on Plasma 6.6+**: Publishes `image/png` with `ext-data-control-v1`, which needs neither a surface nor keyboard focus. This avoids `wl-copy` 2.2.x's transparent 1×1 toplevel workaround that makes KWin exit Peek at Desktop.
* **Pure Silent Execution**: Dispatches no desktop notifications. On the native data-control path it also creates no Wayland surface, so open dropdowns, taskbar menus, and tooltips stay open on screen.
* **Zero-side-effect KDE failure policy**: A direct KWin failure does not silently launch Spectacle by default, because creating a screenshot window can exit KWin's Peek at Desktop state. GUI fallback is available only as an explicit opt-in.
* **Universal Compositor Support**: Features automated detection with `gdbus` on GNOME and `grim` on wlroots/Hyprland; Spectacle is available only as an explicit KDE GUI fallback.

---

## Deep Dive: The 4-Stage Execution Pipeline

```
[ Physical Keypress ] ──> [ Kernel evdev epoll ] ──> [ Async pthread Worker ]
                                                                                       │
                                                          (KWin D-Bus + native-resolution)
                                                                                       │
[ Clipboard Ready ] <── [ atomic PNG + native data-control ] <──────────────────────────┘
```

1. **Stage 1: Kernel evdev event**
   The daemon watches relevant `/dev/input/event*` devices with `epoll_wait()` and wakes when an exact configured chord is pressed. The default `PRINT` chord recognizes `KEY_SYSRQ` and `KEY_PRINT`. The exact wake-up time depends on the kernel, device and scheduler; it is measured locally rather than promised as a fixed number.

2. **Stage 2: Asynchronous Worker Non-Blocking Dispatch**
   Upon receiving the keydown event, the main listener signals a dedicated worker and immediately returns to listening. Frame capture and PNG work do not block the input loop.

3. **Stage 3: Direct KWin D-Bus Frame Sink & Pure C PNG Encoder**
   On KDE Plasma 6, the worker calls `org.kde.KWin.ScreenShot2.CaptureWorkspace` with `native-resolution=true`. KWin returns raw QImage metadata and pixels through a pipe. The worker drains the complete frame first, converts supported QImage formats to RGBA, and compresses it with zlib.

4. **Stage 4: Atomic clipboard publication**
   The completed PNG is atomically renamed into `/dev/shm/wayland_zeroprint.png`. On compositors exposing `ext-data-control-v1` (including KWin 6.6+), a focus-free child publisher owns `image/png` and serves the immutable captured inode on paste. On non-KDE compositors without the native protocol, `wl-copy` remains a compatibility fallback. KDE deliberately refuses the surface-based fallback because it would exit Peek at Desktop.

---

## Benchmarks & Performance Comparison

| Metric | Standard GUI Screenshot | wayland-zeroprint (Native C + Direct KWin) |
| :--- | :--- | :--- |
| **Key handling** | Desktop-dependent | `evdev` + `epoll` + worker thread |
| **KWin capture** | Desktop-dependent | Native-resolution raw frame via `ScreenShot2` |
| **Output** | Utility-dependent | Atomic PNG in `/dev/shm`, then native data-control |
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
* `libwayland-client`, `wayland-scanner`, `wayland-protocols` 1.39+
* `pkg-config`
* Optional `wl-clipboard`: compatibility fallback for non-KDE compositors that do not expose `ext-data-control-v1`

### Fallback Screenshot Backends
* **KDE Plasma**: Built-in Direct KWin D-Bus; `spectacle` is an opt-in GUI fallback
* **GNOME**: `gdbus` (included with GNOME) or `gnome-screenshot`
* **wlroots / Hyprland / Sway**: `grim`

#### Package Installation Commands

* **Fedora / AlmaLinux / RHEL / CentOS**:
  ```bash
  sudo dnf install gcc systemd-devel zlib-devel wayland-devel wayland-protocols-devel pkgconf-pkg-config wl-clipboard
  # Optional fallback backends (Spectacle is disabled by default):
  sudo dnf install spectacle grim
  ```

* **Arch Linux / Manjaro**:
  ```bash
  sudo pacman -S gcc systemd zlib wayland wayland-protocols pkgconf wl-clipboard
  # Optional fallback backends (Spectacle is disabled by default):
  sudo pacman -S spectacle grim
  ```

* **Ubuntu / Debian**:
  ```bash
  sudo apt install gcc libsystemd-dev zlib1g-dev libwayland-dev wayland-protocols pkg-config wl-clipboard
  # Optional fallback backends (Spectacle is disabled by default):
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

## Configuration: Keys and Chords

The installer creates `~/.config/wayland-zeroprint/config` once and never overwrites an existing file:

```ini
# One or more comma-separated physical evdev chords.
triggers=PRINT

# Let KWin consume the exact chord without grabbing the keyboard device.
consume_kde_shortcuts=true

# Keep false if Peek at Desktop and zero focus side effects matter.
allow_gui_fallback=false

# clipboard | file | both
output_mode=clipboard

# Used only for file/both. Directories get an automatic timestamped name.
save_path=~/Pictures/Screenshot-%Y%m%d-%H%M%S-{ms}.png
```

Alternative examples (use one `triggers=` line, or put several chords on that line):

```ini
triggers=F12
# or: triggers=CTRL+ALT+P
# or: triggers=PRINT,META+SHIFT+S
```

Supported modifier names are `CTRL`, `ALT`, `SHIFT`, and `META`; `CONTROL`, `SUPER`, and `WIN` are aliases. Common alphanumeric, navigation, punctuation, function, Print Screen, lock, and media-key names are accepted. An advanced raw evdev code can be written as `CODE_200`; raw numeric triggers work passively but cannot be registered with KDE unless they also have a known Qt key mapping.

Matching is exact. For example, `triggers=PRINT` captures plain Print Screen while leaving `Shift+Print`, `Alt+Print`, and `Meta+Print` available to their normal OS actions. After editing the file, restart the service:

```bash
systemctl --user restart wayland-zeroprint.service
```

Validate the effective config without starting the daemon:

```bash
wayland-zeroprint --print-config
wayland-zeroprint --trigger CTRL+ALT+P --print-config
```

### Clipboard and file output

`output_mode` has three explicit modes:

| Mode | Clipboard | Persistent file |
| :--- | :---: | :---: |
| `clipboard` | Yes | No |
| `file` | No | Yes |
| `both` | Yes | Yes |

`save_path` can be a fixed filename, an existing directory, a path ending in `/`, or a filename template. Standard `strftime(3)` placeholders such as `%Y`, `%m`, `%d`, `%H`, `%M`, and `%S` are expanded; `{ms}` adds three-digit milliseconds so rapid captures do not overwrite one another. `~/` and relative paths resolve under the user's home directory. Missing parent directories are created with private permissions, and the completed PNG is atomically renamed into place.

Examples:

```ini
# Save only, with an automatic timestamped filename in this directory:
output_mode=file
save_path=~/Pictures/Screenshots/

# Copy and save to a unique filename:
output_mode=both
save_path=/mnt/screenshots/shot-%Y-%m-%d_%H-%M-%S-{ms}.png

# Windows-like clipboard-only behavior; no persistent screenshot file:
output_mode=clipboard
```

The clipboard publisher starts before the persistent file copy in `both` mode, so filesystem latency does not delay clipboard publication. In `clipboard` mode the file-saving path is not executed at all. On KWin 6.6+, publication uses `ext-data-control-v1` and never creates a Wayland surface, so it cannot activate the previously focused application or break Peek at Desktop. If a Plasma version does not expose that protocol, zeroprint refuses the focus-changing `wl-copy` fallback on KDE; use `output_mode=file` or upgrade Plasma instead.

Test an output configuration once without waiting for a key press:

```bash
wayland-zeroprint --capture --output file --save-path /tmp/zeroprint-test.png
```

### KDE conflict behavior

KGlobalAccel is used only as a compositor-side consumer; the evdev listener remains the primary, low-latency trigger. If another KDE global action already owns a configured chord, zeroprint does not steal it. The daemon logs the conflict, continues passive evdev capture, and leaves the existing OS shortcut unchanged. Choose an otherwise unused chord when one key press should do only a screenshot.

If Print Screen is already bound to Spectacle, zeroprint cannot claim it. A fresh Plasma profile may still have Print assigned to Spectacle; zeroprint deliberately reports that conflict instead of silently rewriting user shortcut settings. Check the journal, then clear only the conflicting Spectacle action in **System Settings → Keyboard → Shortcuts**:

```bash
journalctl --user -u wayland-zeroprint.service -b
```

On GNOME and wlroots compositors the evdev trigger remains passive, so any desktop action bound to the same chord will also run. Remove that desktop binding or select an unused chord if needed.

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

To remove `wayland-zeroprint` and its udev rules while preserving your user
config for a later reinstall:

```bash
./uninstall.sh
# Or with Makefile:
make uninstall
```

## Security & Volatility Design

`wayland-zeroprint` is engineered with explicit memory and persistence principles to balance speed and data privacy:

* **In-Memory Volatility by default**: With `output_mode=clipboard`, captures reside exclusively in `/dev/shm` (Linux tmpfs physical RAM) and vanish across reboot. `file` and `both` are explicit persistence opt-ins and write to `save_path`.
* **Atomic Overwriting Buffer**: Successive screenshot triggers atomically overwrite `/dev/shm/wayland_zeroprint.png`, preventing historical image buildup or unmonitored disk accumulation.
* **Least-Privilege Execution**: The background daemon executes entirely within user space under standard user credentials, relying on localized `uaccess` rules for `/dev/input` and standard session D-Bus endpoints.

---

## Architectural Trade-offs & Limitations

Operating across the kernel `evdev` driver and compositor D-Bus layers introduces specific architectural and security trade-offs:

1. **Hardware Scancodes vs. Software Keymaps**:
   Trigger names map to kernel-level physical evdev codes. Software-level key remapping configured in Desktop Environments (e.g. XKB layout remapping or virtual/touchscreen keyboards) does not change which physical key triggers the daemon. On KDE, the compositor-side consumer uses the corresponding Qt shortcut mapping, so exotic `CODE_n` bindings remain passive-only.
2. **KWin Permission Check Bypass (`KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1`)**:
   The installer provisions `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS=1` so the user-session D-Bus capture can run without an interactive permission prompt. This is a security-sensitive setting and should only be enabled for a trusted local user session; it is not a latency guarantee.
3. **Application Privacy Hooks**:
   Direct hardware interception bypasses user-space application lifecycle hooks that may attempt to obscure sensitive content (such as password managers or secure messaging windows) prior to a screenshot request.
4. **Mixed-monitor scale**:
   KWin's native-resolution workspace capture uses one canvas scale. A single flat PNG cannot be a simultaneous 1:1 physical-pixel representation for outputs configured with different fractional scales; for that setup, capture a specific output with a compositor-aware backend.
5. **Workaround Nature vs. Upstream Fixes**:
   This daemon acts as a lightweight, independent workaround for desktop environments experiencing shortcut dispatcher drops over complex UI surfaces (XWayland windows, transient menus). For native desktop integration, resolving shortcut dispatcher behavior directly in upstream compositors remains the ideal architectural solution.
6. **Global shortcut ownership outside KDE**:
   Linux evdev readers are passive unless they take an exclusive device grab. This project deliberately does not grab or re-inject a keyboard. KDE's KGlobalAccel integration consumes one exact chord safely; GNOME and wlroots users must ensure the selected chord is otherwise unbound if they do not want two actions from one press.

---

## License

This project is licensed under the **Apache License 2.0**. See the [LICENSE](LICENSE) file for complete terms.

Copyright (c) 2026 **Nguyen Dong Quan** (<nguyendongquan247@gmail.com>).
