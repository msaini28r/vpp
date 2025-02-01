# Base image (change the version dynamically in GitHub Actions)
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

# Install required dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    iproute2 \
    iputils-ping \
    && rm -rf /var/lib/apt/lists/*

# Set working directory
WORKDIR /vpp

# Install VPP using packagecloud script
RUN curl -s https://packagecloud.io/install/repositories/fdio/release/script.deb.sh | bash \
    && apt-get update && apt-get install -y vpp vpp-plugin-core vpp-plugin-dpdk \
    && dpkg-query -f '${Version}\n' -W vpp > /vpp/version

# Create log directory
RUN mkdir -p /var/log/vpp

# Default command to run VPP
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
