FROM ubuntu:22.04

# Install dependencies required for both methods
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

# Try installing .deb packages first
RUN if [ "$VPP_INSTALL_METHOD" = "deb" ]; then \
      echo "Attempting to install VPP using .deb packages..."; \
      curl -fSL -o vpp.deb "https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp_22.06.0_amd64.deb" && \
      curl -fSL -o vpp-plugin-core.deb "https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp-plugin-core_22.06.0_amd64.deb" && \
      curl -fSL -o vpp-plugin-dpdk.deb "https://packagecloud.io/fdio/release/packages/ubuntu/jammy/vpp-plugin-dpdk_22.06.0_amd64.deb"; \
      if dpkg-deb --info vpp.deb > /dev/null 2>&1; then \
          echo "Valid .deb files found, proceeding with installation..."; \
          dpkg -i *.deb || apt-get install -f -y; \
      else \
          echo "Downloaded .deb files are invalid! Falling back to apt install..."; \
          rm -f *.deb; \
          VPP_INSTALL_METHOD=apt; \
      fi \
    fi

# If .deb installation failed, use apt install
RUN if [ "$VPP_INSTALL_METHOD" = "apt" ]; then \
      echo "Using APT to install VPP..."; \
      echo "deb https://packagecloud.io/fdio/release/ubuntu jammy main" | tee /etc/apt/sources.list.d/fdio.list; \
      curl -fsSL https://packagecloud.io/fdio/release/gpgkey | apt-key add -; \
      apt-get update && apt-get install -y vpp vpp-plugin-core vpp-plugin-dpdk; \
    fi

# Extract VPP version and store it
RUN if dpkg-query -W vpp; then \
      dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    else \
      echo "VPP installation failed! Exiting..."; \
      exit 1; \
    fi

# Setup log directory
RUN mkdir -p /var/log/vpp

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
