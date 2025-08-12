#!/bin/bash

set -e

mkdir -p third-party
pushd third-party
git clone https://github.com/microsoft/vcpkg
pushd vcpkg
./bootstrap-vcpkg.sh --disableMetrics
popd
popd
