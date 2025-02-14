FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    apt-transport-https \
    ca-certificates \
    curl \
    gnupg \
    iproute2 \
    iputils-ping \
    && rm -rf /var/lib/apt/lists/*

ARG REPO
ARG VPP_VERSION

WORKDIR /vpp

# Copy the get-vpp.sh script from .github/workflows/
COPY .github/workflows/get-vpp.sh /get-vpp.sh

RUN set -eux; \
    chmod +x /get-vpp.sh; \
    /get-vpp.sh; \
    apt-get update && apt-get install -y -V ./*.deb; \
    dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf vom*.deb vpp-dbg*.deb; \
    rm -rf /var/lib/apt/lists/*;

RUN mkdir -p /var/log/vpp

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
