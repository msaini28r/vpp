FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    gnupg \
    iproute2 \
    iputils-ping \
    make \
    python3 \
    python3-pip \
    build-essential \
    libmnl-dev \
    libssl-dev \
    check \
    cmake \
    ninja-build \
 && rm -rf /var/lib/apt/lists/*

WORKDIR /vpp

# Since we're in the vpp repository, we just need to copy everything
COPY . .

RUN make install-dep \
    && make build \
    && make pkg-deb \
    && apt-get update \
    && apt-get install -y -V ./build-root/*.deb \
    && rm -rf /var/lib/apt/lists/* \
    && mkdir -p /var/log/vpp \
    && vppctl show version | grep 'Version:' | awk '{print $2}' > /vpp/version

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]