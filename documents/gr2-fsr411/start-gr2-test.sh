#!/bin/sh
# Starts the FSR 4 test build of the GR2 fork with this folder's own copy of the settings and saves.
cd "$(dirname "$0")" || exit 1
exec /home/yogesh/Games/GR2fork-source/build/shadps4 -g "/home/yogesh/Games/PS4 Roms/CUSA03694/eboot.bin" "$@"
