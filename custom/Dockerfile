# Use the official Ubuntu as the base image
FROM ubuntu:22.04

# Set the working directory to /vpp
WORKDIR /vpp

# Install dependencies and setup the VPP installation repository
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    iproute2 \
    iputils-ping \
    lsb-release \
    build-essential \
    && rm -rf /var/lib/apt/lists/* \
    && curl -fsSL https://packagecloud.io/install/repositories/fdio/release/script.deb.sh | bash

# Install VPP (with the correct version)
RUN curl -fsSL https://packagecloud.io/fdio/release/gpgkey | apt-key add - \
    && apt-get update \
    && apt-get install -y vpp=24.10-release vpp-plugin-core=24.10-release vpp-plugin-dpdk=24.10-release \
    && rm -rf /var/lib/apt/lists/*

# Create a directory for logs
RUN mkdir -p /var/log/vpp

# Copy any local files or configurations to the container (if needed)
# COPY . .

# Set the default command to start VPP
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
