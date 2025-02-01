# Use Ubuntu 22.04 as the base image
FROM ubuntu:22.04

# Install necessary dependencies and clean up to reduce image size
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    iproute2 \
    iputils-ping \
    build-essential \
    make \
    python3 \
    git \
  && rm -rf /var/lib/apt/lists/*

# Set the working directory to /vpp
WORKDIR /vpp

# Copy the .deb files into the container
COPY *.deb ./

# Install the VPP and related packages using dpkg
RUN set -eux; \
    apt-get update && apt-get install -y -V ./*.deb; \
    dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf /var/lib/apt/lists/*;

# Create the directory for logs
RUN mkdir -p /var/log/vpp

# Command to start VPP when the container runs
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
