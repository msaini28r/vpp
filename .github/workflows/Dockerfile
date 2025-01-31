FROM ubuntu:22.04

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

# Copy the repository code into the container
COPY . /vpp

# Build VPP from source
RUN set -eux; \
    make install-dep; \
    make build-release; \
    dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf /var/lib/apt/lists/*

# Setup log directory
RUN mkdir -p /var/log/vpp

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
