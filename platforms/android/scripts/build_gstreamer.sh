#!/usr/bin/env bash
#
# Step 3: build the Android arm64 GStreamer sysroot with Cerbero and expose it
# to the Android build via GSTREAMER_SYSROOT.
#
# Consumes: env.sh at the repository root (GST_VERSION / CERBERO_BRANCH,
#           CERBERO_HOME, GSTREAMER_PREFIX, SHARED_ANDROID_ENV_FILE).
# Produces: an extracted sysroot under $GSTREAMER_PREFIX and a fully populated
#           $SHARED_ANDROID_ENV_FILE (default $HOME/shared/platforms/android/env.sh)
#           that build_android.sh sources unchanged.
#
# Cerbero's first bootstrap+package run is ~1h. This script is idempotent:
# reruns skip clone/bootstrap/package if their outputs already exist.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$(readlink -f "$0")")" && pwd)"
REPO_ROOT="$(cd "$SCRIPT_DIR/../../.." && pwd)"

if [ ! -f "$REPO_ROOT/env.sh" ]; then
    echo "ERROR: $REPO_ROOT/env.sh not found." >&2
    exit 1
fi
# shellcheck disable=SC1091
. "$REPO_ROOT/env.sh"

: "${CERBERO_BRANCH:?env.sh must export CERBERO_BRANCH}"
: "${CERBERO_HOME:?env.sh must export CERBERO_HOME}"
: "${GSTREAMER_PREFIX:?env.sh must export GSTREAMER_PREFIX}"
: "${ANDROID_HOME:?env.sh must export ANDROID_HOME}"
: "${ANDROID_NDK_HOME:?env.sh must export ANDROID_NDK_HOME}"

echo "==> build_gstreamer.sh"
echo "    CERBERO_BRANCH   = $CERBERO_BRANCH"
echo "    CERBERO_HOME     = $CERBERO_HOME"
echo "    GSTREAMER_PREFIX = $GSTREAMER_PREFIX"

# The SDK / NDK must already be present -- install_sdk_ndk.sh runs before us.
if [ ! -d "$ANDROID_NDK_HOME" ]; then
    echo "ERROR: NDK not found at $ANDROID_NDK_HOME." >&2
    echo "       Run platforms/android/scripts/install_sdk_ndk.sh first." >&2
    exit 1
fi

# ---------------------------------------------------------------------------
# Clone Cerbero at the pinned branch
# ---------------------------------------------------------------------------
if [ ! -d "$CERBERO_HOME/.git" ]; then
    echo "==> Cloning cerbero ${CERBERO_BRANCH}"
    mkdir -p "$(dirname "$CERBERO_HOME")"
    git clone --depth 1 --branch "$CERBERO_BRANCH" \
        https://gitlab.freedesktop.org/gstreamer/cerbero.git "$CERBERO_HOME"
else
    echo "==> Reusing existing cerbero checkout at $CERBERO_HOME"
fi

cd "$CERBERO_HOME"

CBC="config/cross-android-arm64.cbc"

# ---------------------------------------------------------------------------
# Disable vvdec on android_arm64
# ---------------------------------------------------------------------------
# vvdec 3.1.0's CMakeLists hard-codes `-fuse-ld=gold` when linking its test
# and app binaries (vvdecapp, vvdec_unit_test). NDK r29 shipped only lld and
# removed the gold linker, so clang falls back to the host's /usr/bin/ld.gold
# (x86_64-only) and dies with `unsupported ELF machine number 183` on the
# aarch64 objects. libvvdec.a itself builds fine, but the test binaries take
# down the whole recipe.
#
# We don't ship VVC/H.266 decoding, so cut vvdec out of the dependency
# graph entirely: (1) drop `vvdec:libs` from the restricted-codecs package,
# and (2) remove `vvdec` from the gst-plugins-rs codecs_restricted list
# (that block also conditionally re-adds vvdec as a hard dep).
# Both edits are idempotent -- reruns are a no-op once applied.
CODEC_PKG="packages/gstreamer-1.0-codecs-restricted.package"
RS_RECIPE="recipes/gst-plugins-rs.recipe"

if grep -q "'vvdec:libs'" "$CODEC_PKG"; then
    echo "==> Patching $CODEC_PKG to drop vvdec:libs"
    sed -i "s/'vvdec:libs',[[:space:]]*//g" "$CODEC_PKG"
fi

if grep -q "^[[:space:]]*'vvdec',$" "$RS_RECIPE"; then
    echo "==> Patching $RS_RECIPE to drop 'vvdec' from codecs_restricted"
    sed -i "/^[[:space:]]*'vvdec',$/d" "$RS_RECIPE"
fi

# ---------------------------------------------------------------------------
# Bootstrap (idempotent -- cerbero skips components it already provisioned).
# ---------------------------------------------------------------------------
echo "==> cerbero bootstrap (android arm64)"
./cerbero-uninstalled -c "$CBC" bootstrap

# ---------------------------------------------------------------------------
# Package gstreamer-1.0 as a tarball artifact.
# ---------------------------------------------------------------------------
# Skip repackaging if a tarball already exists AND we already extracted a
# valid sysroot on a previous run.
NEED_PACKAGE=1
if compgen -G "$CERBERO_HOME/gstreamer-1.0-android-*.tar.xz" >/dev/null; then
    if find "$GSTREAMER_PREFIX" -type d -path '*/share/gst-android/ndk-build' \
            -print -quit 2>/dev/null | grep -q .; then
        NEED_PACKAGE=0
    fi
fi

if [ "$NEED_PACKAGE" = "1" ]; then
    echo "==> cerbero package gstreamer-1.0 (this may take a while)"
    ./cerbero-uninstalled -c "$CBC" package gstreamer-1.0 --artifact tarball
else
    echo "==> Reusing existing cerbero tarball + extracted sysroot"
fi

# ---------------------------------------------------------------------------
# Extract tarballs into $GSTREAMER_PREFIX and locate the sysroot.
# ---------------------------------------------------------------------------
mkdir -p "$GSTREAMER_PREFIX"

mapfile -t GST_TARBALLS < <(find "$CERBERO_HOME" -maxdepth 2 -type f \
    -name 'gstreamer-1.0-android-*.tar.xz' | sort)

if [ "${#GST_TARBALLS[@]}" -eq 0 ]; then
    echo "ERROR: no gstreamer-1.0-android-*.tar.xz produced by cerbero" >&2
    exit 1
fi

for tarball in "${GST_TARBALLS[@]}"; do
    echo "==> Extracting $(basename "$tarball")"
    tar -xf "$tarball" -C "$GSTREAMER_PREFIX"
done

GSTREAMER_SYSROOT="$(find "$GSTREAMER_PREFIX" -type d \
    -path '*/share/gst-android/ndk-build' -print -quit \
    | sed 's#/share/gst-android/ndk-build##')"

# Fallback to cerbero's in-tree dist if the packaged tree does not expose the
# expected layout (mirrors the CI workflow).
if [ -z "$GSTREAMER_SYSROOT" ]; then
    GSTREAMER_SYSROOT="$(find "$CERBERO_HOME" -type d \
        -path '*/share/gst-android/ndk-build' -print -quit \
        | sed 's#/share/gst-android/ndk-build##')"
fi

if [ -z "$GSTREAMER_SYSROOT" ] \
   || [ ! -d "$GSTREAMER_SYSROOT/share/gst-android/ndk-build" ]; then
    echo "ERROR: could not locate a sysroot containing share/gst-android/ndk-build" >&2
    exit 1
fi

echo "==> GSTREAMER_SYSROOT = $GSTREAMER_SYSROOT"

# ---------------------------------------------------------------------------
# Rewrite the shared env file with the resolved GSTREAMER_SYSROOT.
# ---------------------------------------------------------------------------
mkdir -p "$SHARED_ANDROID_ENV_DIR"
cat > "$SHARED_ANDROID_ENV_FILE" <<EOF
# Auto-generated by platforms/android/scripts/build_gstreamer.sh -- do not edit.
export ANDROID_HOME="$ANDROID_HOME"
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME="$ANDROID_NDK_HOME"
export GSTREAMER_SYSROOT="$GSTREAMER_SYSROOT"
export CERBERO_BRANCH="$CERBERO_BRANCH"
EOF

echo "==> Wrote $SHARED_ANDROID_ENV_FILE"
echo "==> GStreamer build complete."
echo
echo "Next step: ./build_android.sh debug"
