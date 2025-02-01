# Use a specific Ubuntu base image
FROM ubuntu:22.04

# Install necessary packages, including dependencies for VPP
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

# Set working directory
WORKDIR /vpp

# Copy .deb package files from the build context
COPY *.deb ./

# Install the .deb packages
RUN set -eux; \
    apt-get update && apt-get install -y -V ./*.deb; \
    dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf /var/lib/apt/lists/*;

# Create a log directory for VPP
RUN mkdir -p /var/log/vpp

# Set the default command to start VPP with the appropriate configuration
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
