#!/usr/bin/env bash
set -u
cd "$(dirname "$0")"
exec ./dlssnr-helper "${1:-start}"