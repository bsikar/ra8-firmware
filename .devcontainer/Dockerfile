#
# ra8d2-firmware development container.
#
# Ships an Ubuntu 24.04 base with the ARM GNU Toolchain, cmake,
# clang-format, clang-tidy, doxygen, and the J-Link toolchain
# prerequisites. Everything needed to build, test, and flash the
# firmware. SEGGER J-Link package is NOT baked in because it
# requires an EULA click-through -- install manually after first
# container start if you want `make flash` to work.
#

FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
      build-essential \
      ca-certificates \
      clang \
      clang-format \
      clang-tidy \
      cmake \
      cppcheck \
      curl \
      doxygen \
      gcc-arm-none-eabi \
      gcovr \
      git \
      graphviz \
      libnewlib-arm-none-eabi \
      ninja-build \
      pkg-config \
      python3 \
      python3-pip \
      sudo \
      unzip \
      wget \
    && rm -rf /var/lib/apt/lists/*

# Create a non-root user so post-create scripts run as a human.
ARG USERNAME=vscode
ARG USER_UID=1000
ARG USER_GID=$USER_UID

RUN groupadd --gid ${USER_GID} ${USERNAME} \
    && useradd --uid ${USER_UID} --gid ${USER_GID} -m ${USERNAME} \
    && echo "${USERNAME} ALL=(ALL) NOPASSWD:ALL" >> /etc/sudoers.d/${USERNAME}

USER ${USERNAME}
WORKDIR /workspace
