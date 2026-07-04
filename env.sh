#!/usr/bin/env bash
# Shared Android build environment for streamOutApp.
#
# This file lives at the repository root and is sourced by:
#   - platforms/android/scripts/install_sdk_ndk.sh
#   - platforms/android/scripts/build_gstreamer.sh
#
# Those scripts then materialise the "shared" env file at
#   $HOME/shared/platforms/android/env.sh
# which is the path build_android.sh (unchanged) actually sources.
#
# Version pins here mirror .github/workflows/android.yml. Keep them in sync
# with CI; CI is the source of truth.

# ---------------------------------------------------------------------------
# Version pins (mirror .github/workflows/android.yml)
# ---------------------------------------------------------------------------
export GST_VERSION="${GST_VERSION:-1.28.4}"
export CERBERO_BRANCH="${CERBERO_BRANCH:-${GST_VERSION}}"
export ANDROID_API_LEVEL="${ANDROID_API_LEVEL:-32}"
export ANDROID_BUILD_TOOLS="${ANDROID_BUILD_TOOLS:-36.0.0}"
export ANDROID_NDK_VERSION="${ANDROID_NDK_VERSION:-30.0.14904198}"
export ANDROID_CMAKE_VERSION="${ANDROID_CMAKE_VERSION:-3.22.1}"
export ANDROID_CMDLINE_TOOLS_VERSION="${ANDROID_CMDLINE_TOOLS_VERSION:-11076708}"

# ---------------------------------------------------------------------------
# Repo root + cache layout
# ---------------------------------------------------------------------------
# Everything heavy (SDK, NDK, Cerbero checkout, GStreamer sysroot) lives under
# a single in-repo cache dir so:
#   * it is visible from both host and container (repo is bind-mounted)
#   * `docker compose down -v` cannot destroy it
#   * bumping any pinned version above creates a NEW toolchain-* dir instead
#     of overwriting the previous one, so rollback is a symlink swap
#
# Layout under $CACHE_DIR:
#   sources/                  cerbero clone + any other upstream sources
#   toolchain-<bt>-<ndk>/     Android SDK (== ANDROID_HOME); NDK lives inside
#                             at ndk/<ANDROID_NDK_VERSION> where AGP expects it
#   target_sysroot/           extracted GStreamer Android arm64 sysroot

# Resolve repo root from this file's own location so `. env.sh` works no matter
# where it is sourced from.
_ENV_SH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]:-$0}")" && pwd)"
export REPO_ROOT="${REPO_ROOT:-$_ENV_SH_DIR}"
unset _ENV_SH_DIR

export CACHE_DIR="${CACHE_DIR:-$REPO_ROOT/platforms/android/cache}"
export SRC_DIR="${SRC_DIR:-$CACHE_DIR/sources}"
export INSTALL_DIR="${INSTALL_DIR:-$CACHE_DIR/target_sysroot}"

# ---------------------------------------------------------------------------
# Toolchain locations (derived from the cache layout)
# ---------------------------------------------------------------------------
# Versioned toolchain dir: bumping ANDROID_BUILD_TOOLS or ANDROID_NDK_VERSION
# yields a fresh install path rather than mutating the previous one.
export ANDROID_HOME="${ANDROID_HOME:-$CACHE_DIR/toolchain-${ANDROID_BUILD_TOOLS}-${ANDROID_NDK_VERSION}}"
export ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-$ANDROID_HOME}"
# sdkmanager installs the NDK at $ANDROID_HOME/ndk/<version> and AGP looks it
# up the same way via android.ndkVersion -- keep this path in sync.
export ANDROID_NDK_HOME="${ANDROID_NDK_HOME:-$ANDROID_HOME/ndk/$ANDROID_NDK_VERSION}"

# Cerbero clone + extracted GStreamer tree.
export CERBERO_HOME="${CERBERO_HOME:-$SRC_DIR/cerbero}"
export GSTREAMER_PREFIX="${GSTREAMER_PREFIX:-$INSTALL_DIR}"

# GSTREAMER_SYSROOT is written by build_gstreamer.sh once cerbero has produced
# an artifact. It must contain share/gst-android/ndk-build.
export GSTREAMER_SYSROOT="${GSTREAMER_SYSROOT:-}"

# ---------------------------------------------------------------------------
# JDK / PATH
# ---------------------------------------------------------------------------
# JAVA_HOME may be pre-set by the container. Fall back to the standard Temurin
# install path baked into the Dockerfile if not.
if [ -z "${JAVA_HOME:-}" ] && [ -d "/opt/java/openjdk" ]; then
    export JAVA_HOME="/opt/java/openjdk"
fi

export PATH="${JAVA_HOME:+$JAVA_HOME/bin:}$ANDROID_HOME/cmdline-tools/latest/bin:$ANDROID_HOME/platform-tools:$PATH"

# ---------------------------------------------------------------------------
# Shared env file consumed by build_android.sh
# ---------------------------------------------------------------------------
export SHARED_ANDROID_ENV_DIR="${SHARED_ANDROID_ENV_DIR:-$HOME/shared/platforms/android}"
export SHARED_ANDROID_ENV_FILE="${SHARED_ANDROID_ENV_FILE:-$SHARED_ANDROID_ENV_DIR/env.sh}"
