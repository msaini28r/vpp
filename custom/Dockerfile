FROM ubuntu:22.04 as builder

# Install build dependencies
RUN apt-get update && apt-get install -y --no-install-recommends \
    make \
    git \
    python3 \
    python3-pip \
    build-essential \
    libmnl-dev \
    libssl-dev \
    check \
    cmake \
    ninja-build \
    ca-certificates \
    curl \
    gcc \
    g++ \
    pkg-config \
 && rm -rf /var/lib/apt/lists/*

ARG REPO=master
WORKDIR /vpp

# Clone VPP repository
RUN git clone -b ${REPO} https://github.com/FDio/vpp.git . \
    && make install-dep \
    && make build \
    && make pkg-deb

# Final image
FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
        ca-certificates \
        curl \
        gnupg \
        iproute2 \
        iputils-ping \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /vpp

# Copy built packages from builder
COPY --from=builder /vpp/build-root/*.deb ./

RUN set -eux; \
    apt-get update && apt-get install -y -V ./*.deb; \
    mkdir -p /var/log/vpp; \
    rm -rf /var/lib/apt/lists/*; \
    vppctl show version | grep 'Version:' | awk '{print $2}' > /vpp/version

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]