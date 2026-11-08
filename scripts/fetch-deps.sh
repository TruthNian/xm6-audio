#!/usr/bin/env sh
# Use the exact codec and build-tool commits recorded in this repository.
set -eu
cd "$(dirname "$0")/.."
git submodule update --init --recursive
