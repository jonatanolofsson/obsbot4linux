#!/usr/bin/env bash
# Build the AppImage inside the pinned Ubuntu 22.04 container. THIS is the
# supported way to produce a release artifact.
#
# Running qt/packaging/build-appimage.sh directly works only on a machine that
# happens to have Qt 6 dev packages AND the full desktop X11/GL client stack
# installed. On anything else — notably the repo's own nix dev shell — it gets
# all the way to the deploy step and then dies with
#
#     Could not find dependency: libGLX.so.0
#
# because linuxdeploy resolves the Qt platform plugin's ELF dependencies and
# those libraries are not there. It also silently pins the artifact's glibc
# floor to whatever the host runs, which is how an AppImage ends up failing on
# the user's machine with "GLIBC_2.xx not found". The container fixes both.
#
# Usage:
#   qt/packaging/build-appimage-container.sh              # build (image cached)
#   qt/packaging/build-appimage-container.sh --rebuild    # force-rebuild image
#
# Output: dist/OBSBOT4Linux-x86_64.AppImage
set -euo pipefail

HERE=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)   # qt/packaging
REPO=$(cd -- "$HERE/../.." && pwd)                  # repo root
IMAGE=localhost/obsbot4linux-appimage-builder:22.04

REBUILD=0
[ "${1:-}" = "--rebuild" ] && REBUILD=1

ENGINE=$(command -v podman || command -v docker || true)
if [ -z "$ENGINE" ]; then
    echo "error: neither podman nor docker found — install one, or run" >&2
    echo "       qt/packaging/build-appimage.sh on a full desktop instead." >&2
    exit 1
fi
echo ">> engine: $ENGINE"

# `image exists` is podman-only; `image inspect` works on both engines.
if [ "$REBUILD" = 1 ] || ! "$ENGINE" image inspect "$IMAGE" >/dev/null 2>&1; then
    echo ">> building $IMAGE (first run downloads Qt — several minutes)"
    "$ENGINE" build -t "$IMAGE" -f "$HERE/Containerfile" "$HERE"
else
    echo ">> using cached image $IMAGE  (--rebuild to refresh)"
fi

# --userns=keep-id (podman only) keeps dist/ and the build dirs owned by the
# invoking user instead of root. Docker maps root differently; there the files
# come out root-owned, which is noted rather than worked around.
USERNS=()
case "$ENGINE" in *podman) USERNS=(--userns=keep-id) ;; esac

# :z relabels for SELinux (Fedora/RHEL). Harmless where SELinux is not enforcing.
#
# `bash -c`, NOT `bash -lc`: a login shell re-sources /etc/profile, which resets
# PATH and drops the Qt bin directory the image put there. QMAKE and
# CMAKE_PREFIX_PATH come from the image's ENV and need no help.
echo ">> packaging in $IMAGE"
"$ENGINE" run --rm "${USERNS[@]}" \
    -v "$REPO":/src:z -w /src \
    "$IMAGE" \
    bash -c 'qt/packaging/build-appimage.sh'

OUT="$REPO/dist/OBSBOT4Linux-x86_64.AppImage"
[ -f "$OUT" ] || { echo "error: packaging reported success but $OUT is missing" >&2; exit 1; }
echo ">> done: $OUT"
ls -lh "$OUT"
