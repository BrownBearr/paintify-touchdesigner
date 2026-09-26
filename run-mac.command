#!/bin/bash
# Double-click in Finder to open gpu-sbr, building it first if needed.
# From a terminal, any arguments are passed through: ./run-mac.command --in photo.jpg
cd "$(dirname "$0")"
if [ ! -x build/gpu-sbr ]; then
    tools/build-mac.sh || exit 1
fi
exec build/gpu-sbr "$@"
