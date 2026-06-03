# Apollo Ubuntu

Apollo Ubuntu is a GNOME Wayland fork of [Apollo](https://github.com/ClassicOldSong/Apollo), targeted at Ubuntu hosts running the GNOME (Mutter) desktop.
It keeps Apollo's Moonlight/Artemis streaming workflow while maintaining the GNOME Wayland virtual display and packaging work in this repository instead of trying to merge those changes back into the Windows-first parent.

> **Scope:** the virtual display is built on GNOME Mutter's D-Bus APIs (`ScreenCast`, `RemoteDesktop`, `DisplayConfig`). It requires a GNOME (Mutter) Wayland session and does **not** work on other compositors such as KDE/KWin or wlroots-based ones (Sway, Hyprland). It is portable across GNOME Wayland distributions with a recent enough Mutter, but Ubuntu is the only supported/packaged target.

## What This Fork Provides

- **Kernel-free virtual display streaming on GNOME Wayland.** A real virtual monitor is created entirely through GNOME Mutter's RecordVirtual D-Bus API — no out-of-tree kernel module to build or load.
- **Secure Boot safe.** There is no DKMS module to sign and no MOK (`Machine Owner Key`) enrollment, so UEFI Secure Boot can stay enabled.
- **Real, isolated desktop.** Streamed sessions render onto a dedicated virtual head instead of mirroring the physical display; the real desktop and its windows relocate onto it, and the physical monitor is powered down while streaming.
- **High resolution and high frame rate.** Native client resolution up to 4K, with 120 fps+ capture (verified at 2880x1800), paced by the compositor. Capture is zero-copy DMA-BUF (GPU→encoder) when the encode GPU matches GNOME's, with automatic fallback to a CPU-mapped path otherwise (see [Virtual Display Backend](#virtual-display-backend)).
- **AMD, Intel, and NVIDIA encoding** through the Linux encoder stack available on the host.
- **Optional Gamescope backend** for launching games into an Apollo-owned headless compositor.
- User service packaging, udev rules, PipeWire integration, and Ubuntu install documentation.
- A cautious upstream-tracking workflow for reviewing ClassicOldSong/Apollo changes before merging them into this GNOME Wayland fork.

## Supported Host

This branch targets Ubuntu desktop hosts, especially:

- Ubuntu 24.04 LTS or newer.
- Ubuntu 26.04 development/current testing builds.
- GNOME on Wayland (Mutter ScreenCast/RemoteDesktop API version 4+) with the streaming user already logged in.
- A Moonlight-compatible client such as Artemis or Moonlight.

UEFI Secure Boot can remain enabled — the virtual display path does not load any kernel module. Other Linux distributions with GNOME Wayland may still build and run, but Ubuntu is the supported release target for this fork.

## Recommended Install

Use the `.deb` artifact from this fork's releases when available.

```bash
sudo apt update
sudo apt install ./ApolloUbuntu*.deb
sudo reboot
```

After reboot:

```bash
systemctl --user enable --now sunshine.service
journalctl --user -u sunshine.service -f
```

Open the web UI:

```text
https://localhost:47990
```

The browser will warn about the self-signed certificate. That is expected for the local web UI.

## Required GNOME User Session

Apollo Ubuntu's supported virtual display path requires an active GNOME Wayland
desktop session for the streaming user. The host cannot stream from the GDM
login screen before that user has logged in.

This is a GNOME/Wayland session boundary, not just a systemd startup ordering
issue:

- `sunshine.service` is installed as a user service under
  `xdg-desktop-autostart.target`.
- The default backend talks to the logged-in user's session bus.
- Mutter ScreenCast, Mutter RemoteDesktop, PipeWire, display layout state, and
  pointer injection are all owned by the active user session.
- `loginctl enable-linger` can keep a user systemd manager alive after logout,
  but it does not create a GNOME Shell, Mutter, Wayland, or PipeWire desktop
  session.

For reboot-and-stream hosts, enable GNOME automatic login for the streaming
user. The user account can still have a password; automatic login only tells
GDM to enter the desktop session after boot.

Edit `/etc/gdm3/custom.conf`:

```bash
sudo nano /etc/gdm3/custom.conf
```

Set the login user:

```ini
[daemon]
AutomaticLoginEnable=true
AutomaticLogin=your-username
```

Then reboot and verify the desktop session and Apollo service:

```bash
sudo reboot
loginctl
systemctl --user status sunshine.service
curl -k https://localhost:47990
```

If the system uses full-disk encryption, the disk still has to be unlocked
before GDM can start the automatic login.

## Ubuntu Runtime Requirements

The Debian package is intended to install the required runtime dependencies, including PipeWire, GIO/GLib, DRM, VAAPI, and input rules. If you are preparing a host manually, install:

```bash
sudo apt update
sudo apt install pipewire wireplumber
```

For the optional Gamescope backend, also install:

```bash
sudo apt install gamescope libei1
```

Verify the virtual display prerequisites:

```bash
systemctl --user status pipewire wireplumber
```

The virtual display is created over D-Bus through GNOME Mutter, so there is no kernel module to load and nothing to rebuild after a kernel update.

### Secure Boot

No action required. The virtual display path is kernel-free, so there is no DKMS
module to sign and no MOK (`Machine Owner Key`) enrollment step. You can leave
UEFI Secure Boot enabled.

```bash
mokutil --sb-state   # Secure Boot may remain enabled
```

## Build From Source On Ubuntu

Install build dependencies:

```bash
./scripts/linux_build.sh deps
```

Build:

```bash
cmake -B build -G Ninja -S . \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX=/usr \
  -DSUNSHINE_ENABLE_WAYLAND=ON \
  -DSUNSHINE_ENABLE_X11=ON \
  -DSUNSHINE_ENABLE_DRM=ON

cmake --build build -j"$(nproc)"
```

Install locally:

```bash
sudo cmake --install build
sudo setcap cap_sys_admin+p "$(command -v sunshine)"
systemctl --user daemon-reload
systemctl --user enable --now sunshine.service
```

Build a `.deb`:

```bash
cpack -G DEB --config build/CPackConfig.cmake
```

## Flatpak Status

Flatpak packaging is kept in-tree for experimentation and future distribution, but the Ubuntu `.deb` is the recommended install path for the virtual display backend.

The virtual display path talks to the host's GNOME session bus (Mutter ScreenCast/RemoteDesktop and PipeWire), so the Flatpak still needs an active GNOME Wayland session for the streaming user, but it no longer depends on any host kernel module.

After installing the Flatpak artifact:

```bash
flatpak run --command=additional-install.sh io.github.primezx.ApolloUbuntu
flatpak run io.github.primezx.ApolloUbuntu
```

## Virtual Display Backend

Default:

```text
linux_virtual_display_backend = auto
linux_virtual_capture_backend = auto
linux_pipewire_dmabuf = auto
linux_gamescope_session_command =
```

`auto` uses the kernel-free GNOME Mutter RecordVirtual/PipeWire path. That is the supported GNOME Wayland backend: Apollo asks Mutter to create a virtual monitor, relocates the real desktop onto it, captures it over PipeWire, and injects input through Mutter RemoteDesktop.

Alternatives:

- `mutter`: explicitly select the Mutter RecordVirtual/PipeWire path (same as `auto`).
- `gamescope`: start an Apollo-owned headless Gamescope compositor and capture its PipeWire node. App commands are launched into the Gamescope Wayland/Xwayland session.

Gamescope session command:

- `linux_gamescope_session_command =`: leave blank to run a `sleep infinity` supervisor while Apollo launches the selected app into Gamescope.
- Set it to a long-running command such as a lightweight window manager or diagnostic app when testing the Desktop entry, which otherwise has no app command of its own.
- `APOLLO_GAMESCOPE_COMMAND` can override this temporarily for live diagnostics.

Per-app backend routing:

- Apps may set `"linux-virtual-display-backend": "gamescope"` in `apps.json` to use Gamescope for that app only.
- Leave the normal `Desktop` entry unset so it inherits the global `auto` (Mutter RecordVirtual) path.

Capture acceleration:

By default Apollo uses zero-copy DMA-BUF capture (GPU→encoder, `data_type=3`) when the NVENC
encode GPU matches the GPU GNOME composites on — this eliminates the per-frame GPU→CPU readback
(copy time drops to microseconds) and gives headroom for 4K120. When zero-copy isn't possible it
transparently falls back to CPU-mapped PipeWire buffers (a GPU→CPU→encoder copy), which still
sustains 120 fps+ at the tested resolutions.

- `linux_virtual_capture_backend = auto`: use the PipeWire path with the configured DMA-BUF policy.
- `linux_virtual_capture_backend = pipewire`: force the Mutter/PipeWire capture path.
- `linux_virtual_capture_backend = nvidia`: explicitly try NVIDIA/NvFBC capture for the active virtual display session.
- `linux_pipewire_dmabuf = off`: keep the known-good mapped PipeWire frame path.
- `linux_pipewire_dmabuf = auto`: attempt zero-copy DMA-BUF and transparently fall back to the mapped path if import fails.
- `linux_pipewire_dmabuf = force`: explicitly test DMA-BUF capture and fail the session if the compositor or encoder path cannot use it.

Zero-copy DMA-BUF only works when the NVENC encode GPU is the same GPU GNOME composites on
(the GPU driving the connected physical display); cross-GPU block-linear import fails. Apollo
auto-detects that display GPU and routes the encoder, the DMA-BUF importer, and the modifier
probe to it (you can override the choice with `adapter_name`, e.g. `adapter_name = /dev/dri/renderD129`).
On multi-GPU systems where the encode GPU and the compositor GPU differ — or when detection
misses (e.g. headless) — `auto` transparently falls back to the CPU-mapped path so video is
never interrupted; the fallback happens once per session.

When DMA-BUF diagnostics are enabled in logs, `data_type=3` means PipeWire delivered DMA-BUF. `data_type=1` or `data_type=2` means Apollo is on a mapped CPU/shared-memory fallback.

For temporary diagnostics only, the environment variable below overrides the config value:

```bash
systemctl --user set-environment APOLLO_LINUX_VIRTUAL_BACKEND=gamescope
systemctl --user restart sunshine.service

systemctl --user set-environment APOLLO_LINUX_VIRTUAL_CAPTURE=pipewire APOLLO_PIPEWIRE_DMABUF=force
systemctl --user restart sunshine.service
```

`APOLLO_PIPEWIRE_DMABUF=force` is experimental. On GNOME Wayland systems that reject the negotiated DMA-BUF format, Moonlight may report `connection terminated`; return to `off` for normal streaming.

To clear diagnostic overrides:

```bash
systemctl --user unset-environment APOLLO_LINUX_VIRTUAL_BACKEND APOLLO_LINUX_VIRTUAL_CAPTURE APOLLO_PIPEWIRE_DMABUF
systemctl --user restart sunshine.service
```

## Logs And Troubleshooting

Service logs:

```bash
journalctl --user -u sunshine.service -f
```

Virtual display checks:

```bash
ls /dev/dri
systemctl --user status pipewire wireplumber
```

If Moonlight cannot see the host, verify the service is running and the web/API ports are listening:

```bash
systemctl --user status sunshine.service
curl -k https://localhost:47990
curl http://localhost:47989/serverinfo
```

If the machine rebooted and is sitting at the GDM login screen, log in locally
or enable GNOME automatic login. The Apollo service and the virtual display
backend are expected to become usable only after the user's GNOME Wayland
session exists.

## Fork Maintenance

ClassicOldSong/Apollo remains the parent project for Apollo behavior, but this repository is the Ubuntu/Linux release line. Parent changes should be imported deliberately, tested against GNOME Wayland, Mutter ScreenCast/RemoteDesktop, PipeWire, and Moonlight streaming, and then merged into this fork only after Linux compatibility is reviewed.

See [docs/fork-maintenance.md](docs/fork-maintenance.md) for the upstream tracking workflow.

## Credits

This fork builds on:

- [ClassicOldSong/Apollo](https://github.com/ClassicOldSong/Apollo)
- [LizardByte/Sunshine](https://github.com/LizardByte/Sunshine)
- GNOME Mutter ScreenCast, RemoteDesktop, and PipeWire

## Support

Use issues in this fork for Ubuntu/Linux virtual display bugs:

```text
https://github.com/primez-x/Apollo-Ubuntu/issues
```
