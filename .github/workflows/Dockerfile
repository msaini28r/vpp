FROM ubuntu:22.04

# Install basic dependencies required for both methods
RUN apt-get update && apt-get install -y --no-install-recommends \
    apt-transport-https \
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

WORKDIR /vpp

# Set build argument to choose installation method (default: apt)
ARG VPP_INSTALL_METHOD=apt

# Install VPP using APT if the user chooses 'apt'
RUN if [ "$VPP_INSTALL_METHOD" = "apt" ]; then \
      echo "Using APT to install VPP..."; \
      echo "deb https://packagecloud.io/fdio/release/ubuntu jammy main" | tee /etc/apt/sources.list.d/fdio.list; \
      curl -fsSL https://packagecloud.io/fdio/release/gpgkey | apt-key add -; \
      apt-get update && apt-get install -y vpp vpp-plugin-core vpp-plugin-dpdk; \
    fi

# Install VPP using .deb packages if the user chooses 'deb'
RUN if [ "$VPP_INSTALL_METHOD" = "deb" ]; then \
      echo "Using .deb packages to install VPP..."; \
      curl -O https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp.deb/download.deb; \
      curl -O https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp-plugin-core.deb/download.deb; \
      curl -O https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp-plugin-dpdk.deb/download.deb; \
      dpkg -i *.deb || apt-get install -f -y; \
    fi

# Extract VPP version and store it
RUN dpkg-query -f '${Version}\n' -W vpp > /vpp/version

# Setup log directory
RUN mkdir -p /var/log/vpp

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
