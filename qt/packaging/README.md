# Packaging — AppImage

A self-contained **AppImage** so end users just download it, `chmod +x`, and run —
no Qt install, no build. Bundles Qt 6, the QML runtime, the platform plugins
(xcb + wayland + offscreen), and the OBSBOT SDK (`libdev.so`). Targets **modern
Linux** (recent glibc) — no old-distro compatibility shims.

> **Prebuilt artifact:** `dist/OBSBOT4Linux-x86_64.AppImage` (validated:
> loads the full GUI headless with no errors; `--self-test` runs discovery +
> clean shutdown against the bundled SDK). Just `chmod +x` and run it.

## Build it

```sh
qt/packaging/build-appimage-container.sh
```

Output: `dist/OBSBOT4Linux-x86_64.AppImage`. Then:

```sh
chmod +x dist/OBSBOT4Linux-x86_64.AppImage
./dist/OBSBOT4Linux-x86_64.AppImage            # runs on KDE and GNOME
./dist/OBSBOT4Linux-x86_64.AppImage --self-test
```

That wrapper builds `qt/packaging/Containerfile` (Ubuntu 22.04 + a pinned Qt via
aqtinstall) and runs the real packaging script inside it. Needs `podman` or
`docker` and nothing else; the first run downloads Qt and takes several minutes,
after which the image is cached.

**Use the container for anything you intend to ship.** The base image is the
oldest distro that can still host the build, and that floor is set by the OBSBOT
SDK — `libdev.so` needs `GLIBC_2.34` / `GLIBCXX_3.4.30`. An AppImage cannot run
on a glibc older than the one it was built against, so building on a current
host silently restricts the artifact to hosts as new as yours, which surfaces
much later as `GLIBC_2.xx not found` on someone else's machine.

### Building directly on the host (development only)

```sh
qt/packaging/build-appimage.sh
```

This is what the container runs, and it is fine for a quick local check — but it
only works where **both** Qt 6 dev packages and the full desktop X11/GL *client*
stack are installed, and it inherits the host's glibc floor.

- `cmake`, a C++ compiler, and **Qt 6 dev** (`qmake` on PATH).
  - Arch/CachyOS: `sudo pacman -S --needed cmake qt6-base qt6-declarative`
- Desktop X11/xcb client libraries. `linuxdeploy` walks the Qt platform plugin's
  ELF dependencies, so a missing one aborts the deploy step with
  `Could not find dependency: …` (e.g. `libGLX.so.0`, `libxcb-cursor.so.0`).
- Non-PATH Qt (e.g. an aqt install): pass `QMAKE=… CMAKE_PREFIX_PATH=…`.

> The repo's **nix dev shell is not a packaging environment**. `nix develop`
> gives a Qt 6 toolchain for building and running the app, but deliberately
> omits the X11/GL client libraries, so `build-appimage.sh` under it always
> fails at the deploy step with `Could not find dependency: libGLX.so.0`. Use
> the container.

### Platform: xcb by default (runs everywhere)
The AppImage ships the **xcb** platform plugin, which runs natively under X11 and
via XWayland under Wayland — so it works on KDE and GNOME out of the box. The
GPU/GL stack (`libGL`/`libEGL`/`libGLX`/`libgbm`/`libdrm`) is **deliberately not
bundled** so the host's GPU driver provides it (bundling it breaks GL context
creation → `QRhiGles2: Failed to create context` → SIGABRT).

Native Wayland is opt-in (`WITH_WAYLAND=1`), but note `linuxdeploy-plugin-qt` does
not bundle the wayland-egl graphics-integration plugin, so a native-Wayland build
can fail with "Failed to load client buffer integration wayland-egl". xcb via
XWayland is the reliable default.

If a specific GPU/driver still can't create a GL context, force software
rendering at runtime:
```sh
QT_QUICK_BACKEND=software ./OBSBOT4Linux-x86_64.AppImage
```

## Where settings live

The installed app stores settings per-user (XDG), not in the repo:
```
~/.config/obsbot4linux/obsbot4linux.json
```
Override with `OBSBOT4LINUX_CONFIG=/path`.

## Icon

`icons/obsbot4linux.svg` (source) + PNGs at 16–512 px, regenerable with
`python packaging/make_icon.py` (needs Pillow). The coral OBSBOT ring on obsidian.

## glibc floor

An AppImage runs only on a glibc at least as new as the one it was built
against, so the build environment decides how far the artifact travels. The
container pins that to **Ubuntu 22.04**, which is not a preference — it is the
oldest base the OBSBOT SDK loads on (`libdev.so` needs `GLIBC_2.34` and
`GLIBCXX_3.4.30`).

Build on a current rolling distro instead and the artifact inherits *that*
glibc, which fails on anything older with `GLIBC_2.xx not found` — reported by
users, not by the build, which succeeds either way. That is the whole reason
`build-appimage-container.sh` exists.

A Flatpak manifest can be added later if Flathub distribution is wanted.
