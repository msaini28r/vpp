# Base image (Ubuntu version can be set dynamically)
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

# Install dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
        iproute2 \
        iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /vpp

# Install FD.io VPP repository
RUN curl -s https://packagecloud.io/install/repositories/fdio/release/script.deb.sh | bash

# Install VPP packages using APT
RUN apt-get update && apt-get install -y vpp vpp-plugin-core vpp-plugin-dpdk

# Store installed VPP version
RUN dpkg-query -f '${Version}\n' -W vpp > /vpp/version

# Ensure log directory exists
RUN mkdir -p /var/log/vpp

# Set default command
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
