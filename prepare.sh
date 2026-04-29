#!/bin/bash

set -e

if [ -z "$GITHUB_WORKSPACE" ]; then
	GITHUB_WORKSPACE=$(dirname "$(realpath "$0")")
fi

pushd "$GITHUB_WORKSPACE"

mkdir -p third-party
pushd third-party
git clone https://github.com/microsoft/vcpkg --depth 1
pushd vcpkg
./bootstrap-vcpkg.sh --disableMetrics
popd
popd
popd
