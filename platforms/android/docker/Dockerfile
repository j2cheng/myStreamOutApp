# syntax=docker/dockerfile:1.6
#
# streamOutApp Android build image.
#
# Step 1 of the build flow: this image installs every OS-level tool needed to
#   - run the Android sdkmanager (install_sdk_ndk.sh)
#   - bootstrap and build GStreamer with Cerbero (build_gstreamer.sh)
#   - run gradle / ndk-build against the produced sysroot (build_android.sh)
#
# It does NOT install the Android SDK/NDK or GStreamer itself. Those are
# produced by the scripts under platforms/android/scripts/ and live in the
# builder's $HOME (a bind-mounted volume in docker-compose.yml) so image
# rebuilds do not throw away the ~1h Cerbero build.
#
# Base: Temurin JDK 17 on Ubuntu 22.04 — matches the JDK version used by CI
# (.github/workflows/android.yml).

FROM eclipse-temurin:17-jdk-jammy

ARG DEBIAN_FRONTEND=noninteractive

# ---------------------------------------------------------------------------
# OS packages
# ---------------------------------------------------------------------------
# Groups:
#   * Host build essentials (gcc, make, cmake, ninja, meson, pkg-config, ...)
#   * Cerbero bootstrap prerequisites (autotools, gettext, gperf, flex, bison,
#     libtool, m4, texinfo, nasm, yasm, intltool, docbook-xsl, ...)
#   * Archive / download tooling (curl, wget, unzip, zip, xz-utils, bzip2)
#   * Python (cerbero + meson)
#   * Misc runtime helpers (git, ca-certificates, sudo, locales, file, procps)
#   * Cerbero's Linux bootstrapper (cerbero/bootstrap/linux.py) shells out to
#     `sudo apt-get install <big list>`. Pre-install that exact list so the
#     bootstrap step is a no-op instead of hitting "Unable to locate package".
#     Do NOT `rm -rf /var/lib/apt/lists/*` at the end -- cerbero re-runs
#     apt-get install on every bootstrap and needs a working package index.
RUN apt-get update && apt-get install -y --no-install-recommends \
      ca-certificates \
      curl \
      wget \
      git \
      sudo \
      locales \
      file \
      procps \
      unzip \
      zip \
      xz-utils \
      bzip2 \
      tar \
      patch \
      make \
      cmake \
      ninja-build \
      meson \
      pkg-config \
      build-essential \
      ccache \
      autoconf \
      automake \
      autopoint \
      autotools-dev \
      binutils \
      libtool \
      libtool-bin \
      m4 \
      gettext \
      gperf \
      flex \
      bison \
      texinfo \
      intltool \
      nasm \
      yasm \
      docbook-xsl \
      libglib2.0-dev \
      libssl-dev \
      libasound2-dev \
      libclang-dev \
      libcurl4-openssl-dev \
      libdrm-dev \
      libegl1-mesa-dev \
      libgl1-mesa-dev \
      libglu1-mesa-dev \
      libpulse-dev \
      libva-dev \
      libx11-dev \
      libx11-xcb-dev \
      libxcomposite-dev \
      libxdamage-dev \
      libxext-dev \
      libxfixes-dev \
      libxi-dev \
      libxrandr-dev \
      libxrender-dev \
      libxtst-dev \
      libxv-dev \
      x11proto-record-dev \
      xutils-dev \
      python3 \
      python3-pip \
      python3-setuptools \
      python3-dev \
      python3-distro \
      python3-venv \
      python-is-python3 \
  && apt-get clean

RUN locale-gen en_US.UTF-8
ENV LANG=en_US.UTF-8 \
    LANGUAGE=en_US:en \
    LC_ALL=en_US.UTF-8

# ---------------------------------------------------------------------------
# Non-root build user
# ---------------------------------------------------------------------------
# UID/GID are overridable at build time so bind-mounted volumes stay writable
# on Linux hosts. Defaults match the typical single-user host layout.
ARG USER_NAME=builder
ARG USER_UID=1000
ARG USER_GID=1000

RUN groupadd --gid "${USER_GID}" "${USER_NAME}" \
 && useradd --uid "${USER_UID}" --gid "${USER_GID}" \
      --shell /bin/bash --create-home "${USER_NAME}" \
 && echo "${USER_NAME} ALL=(ALL) NOPASSWD:ALL" > /etc/sudoers.d/${USER_NAME} \
 && chmod 0440 /etc/sudoers.d/${USER_NAME}

# Pre-create the named-volume mountpoints under $HOME so Docker inherits the
# builder-owned perms when first initializing each volume. Without this, a
# named volume mounted onto a path missing from the image is created as
# root:root and the builder user cannot write to it (e.g. gradle wrapper's
# ~/.gradle/wrapper/dists lock file).
RUN mkdir -p /home/${USER_NAME}/.gradle /home/${USER_NAME}/.cache \
 && chown -R ${USER_UID}:${USER_GID} /home/${USER_NAME}

USER ${USER_NAME}
WORKDIR /workspace

# JAVA_HOME is provided by the eclipse-temurin base image; re-export for
# clarity and so env.sh's fallback (/opt/java/openjdk) is used.
ENV JAVA_HOME=/opt/java/openjdk \
    PATH=/opt/java/openjdk/bin:${PATH}

# Default interactive shell — build steps are invoked via the scripts under
# platforms/android/scripts/ and the top-level build_android.sh.
CMD ["/bin/bash"]
