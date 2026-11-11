#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
transport_tmp=$(mktemp -d)
trap 'rm -rf "$transport_tmp"' EXIT
"${CXX:-g++}" -std=c++17 -O2 -pthread tests/test-queue.cpp -o "$transport_tmp/queue"
"$transport_tmp/queue"
python3 -I -B tests/test-scene.py
python3 -I -B tests/test-recovery.py
python3 -I -B tests/test-resize.py
"${CC:-gcc}" -std=c11 -O1 -Wall -Wextra -Werror -pthread tests/test-shim.c -o "$transport_tmp/shim"
"$transport_tmp/shim"
