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

# Set build argument (default: deb)
ARG VPP_INSTALL_METHOD=deb

# Download .deb packages if using .deb method
RUN if [ "$VPP_INSTALL_METHOD" = "deb" ]; then \
      echo "Attempting to install VPP using .deb packages..."; \
      curl -O https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp.deb/download.deb || export DEB_MISSING=true; \
      curl -O https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp-plugin-core.deb/download.deb || export DEB_MISSING=true; \
      curl -O https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp-plugin-dpdk.deb/download.deb || export DEB_MISSING=true; \
      if [ "$DEB_MISSING" = "true" ]; then \
          echo "Some .deb packages are missing, falling back to apt install"; \
          export VPP_INSTALL_METHOD=apt; \
      fi \
    fi

# If .deb files were found, install them; otherwise, use apt
RUN if [ "$VPP_INSTALL_METHOD" = "deb" ]; then \
      dpkg -i *.deb || apt-get install -f -y; \
    else \
      echo "Using APT to install VPP..."; \
      echo "deb https://packagecloud.io/fdio/release/ubuntu jammy main" | tee /etc/apt/sources.list.d/fdio.list; \
      curl -fsSL https://packagecloud.io/fdio/release/gpgkey | apt-key add -; \
      apt-get update && apt-get install -y vpp vpp-plugin-core vpp-plugin-dpdk; \
    fi

# Extract VPP version and store it
RUN dpkg-query -f '${Version}\n' -W vpp > /vpp/version

# Setup log directory
RUN mkdir -p /var/log/vpp

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
