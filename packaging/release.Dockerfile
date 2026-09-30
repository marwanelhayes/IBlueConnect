ARG UBUNTU_VERSION=24.04
FROM ubuntu:${UBUNTU_VERSION}
ENV DEBIAN_FRONTEND=noninteractive
# Builds and package-install checks run without starting system services.
RUN printf '#!/bin/sh\nexit 101\n' > /usr/sbin/policy-rc.d && chmod 755 /usr/sbin/policy-rc.d \
    && apt-get update \
    && apt-get install -y --no-install-recommends build-essential cmake pkg-config \
       libgtk-4-dev libadwaita-1-dev libx11-dev libsqlite3-dev dpkg-dev \
       python3 python3-venv curl ca-certificates jq wireplumber pipewire-bin \
       dbus dbus-x11 xvfb xauth zenity bluez bluez-obexd ofono \
       gnome-session-canberra libcanberra-pulse libspa-0.2-bluetooth pkexec \
    && rm -rf /var/lib/apt/lists/*
WORKDIR /build
