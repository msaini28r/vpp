FROM ubuntu:22.04

# Install necessary dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    iproute2 \
    iputils-ping \
    && rm -rf /var/lib/apt/lists/*

# Set working directory to /vpp
WORKDIR /vpp

# Copy all .deb packages into the container
COPY *.deb ./

# Install .deb packages
RUN set -eux; \
    apt-get update && apt-get install -y -V ./*.deb; \
    dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf /var/lib/apt/lists/*;

# Create directory for logs
RUN mkdir -p /var/log/vpp

# Set the default command to run VPP with the startup config
CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
