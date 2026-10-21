#!/bin/bash

# SOLVFX: build the APFX plug-in bundle (APFX nodes: Noise, ...) from the Natron
# source tree into the OFX plug-ins deploy folder. It is small, so it is built in
# full and in fast test builds alike.

set -e # Exit immediately if a command exits with a non-zero status
set -u # Treat unset variables as an error when substituting.

source common.sh
source manageBuildOptions.sh
source manageLog.sh

updateBuildOptions

APFX_SRC="$TMP_PATH/Natron/Plugins/APFX"
if [ ! -d "$APFX_SRC" ]; then
    echo "Info: no APFX sources in $APFX_SRC, skipping"
    exit 0
fi

printStatusMessage "Building APFX plug-ins..."
mkdir -p "$TMP_BINARIES_PATH/OFX/Plugins"
cd "$APFX_SRC"
make clean >/dev/null 2>&1 || true
make -j"${MKJOBS}" CXX="${CXX:-g++}" OFX_ROOT="$TMP_PATH/Natron/libs/OpenFX"
rm -rf "$TMP_BINARIES_PATH/OFX/Plugins/APFX.ofx.bundle"
cp -a APFX.ofx.bundle "$TMP_BINARIES_PATH/OFX/Plugins/"
ls -R "$TMP_BINARIES_PATH/OFX/Plugins/APFX.ofx.bundle"
echo "Info: APFX plug-ins built"
