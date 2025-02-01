FROM ubuntu:22.04

RUN apt-get update && apt-get install -y --no-install-recommends \
		ca-certificates \
		curl \
		gnupg \
		iproute2 \
		iputils-ping \
 	&& rm -rf /var/lib/apt/lists/*

WORKDIR /vpp

# Copy all .deb files from the current directory to the /vpp directory
COPY *.deb ./

# Install the .deb files using dpkg and fix dependencies with apt-get
RUN set -eux; \
    dpkg -i ./*.deb; \
    apt-get install -f -y; \
    dpkg-query -f '${Version}\n' -W vpp > /vpp/version; \
    rm -rf /var/lib/apt/lists/*;

RUN mkdir -p /var/log/vpp

CMD ["/usr/bin/vpp", "-c", "/etc/vpp/startup.conf"]
