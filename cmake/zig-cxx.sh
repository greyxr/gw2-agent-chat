#!/bin/sh
set -eu
: "${GW2_ZIG:?Set GW2_ZIG to the Zig 0.15.2 executable}"
exec "$GW2_ZIG" c++ -target x86_64-windows-gnu "$@"
