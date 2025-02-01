# Set default Ubuntu version
ARG UBUNTU_VERSION=22.04
FROM ubuntu:${UBUNTU_VERSION}

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

# Set working directory to /vpp
WORKDIR /vpp

# Copy the get-vpp.sh script to the container
COPY custom/get-vpp.sh /vpp/get-vpp.sh

# Make the script executable and run it to fetch and install VPP
RUN chmod +x /vpp/get-vpp.sh && /vpp/get-vpp.sh

# Install any additional VPP-related dependencies if necessary
RUN dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf /var/lib/apt/lists/*;

# Create necessary directories for logging
RUN mkdir -p /var/log/vpp

# Default command to run VPP with a startup configuration
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
