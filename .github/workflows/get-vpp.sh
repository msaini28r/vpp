#!/bin/bash

[ -z "$REPO_URL" ] && REPO_URL="https://packagecloud.io/install/repositories/fdio/${REPO:=release}"

function get_vpp () {
    ls "*.deb" 2>/dev/null && { die "remove existing *.deb files"; }

    set -exuo pipefail
    trap '' PIPE

    # Run script without sudo
    curl -sS "${REPO_URL}"/script.deb.sh | bash || {
        die "Packagecloud FD.io repo fetch failed."
    }

    artifacts=()
    allVersions=$(apt-cache -o Dir::Etc::SourceList=${REPO_URL}/script.deb.sh show vpp | grep Version: | cut -d " " -f 2)

    if [ "${REPO}" != "master" ]; then
        nonRcVersions=$(echo "$allVersions" | grep -v "\-rc[0-9]") || true
        [ -n "${nonRcVersions}" ] && allVersions=$nonRcVersions
    fi

    VPP_VERSION=$(echo "$allVersions" | head -n1) || true
    set +x
    echo "Finding packages with version: ${VPP_VERSION-}"

    for package in $(apt-cache show -- vpp | grep Package: | cut -d " " -f 2); do
        artifacts+=(${package[@]/%/=${VPP_VERSION-}})
    done
    set -x

    apt-get -y download "${artifacts[@]}" || die "Download VPP artifacts failed."
}

function die () {
    echo "${1:-Unspecified run-time error occurred!}"
    exit "${2:-1}"
}

get_vpp
