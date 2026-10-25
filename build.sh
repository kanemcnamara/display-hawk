#!/usr/bin/env bash
# Builds Display Hawk on macOS (or Linux) and, on macOS, bundles the Qt frameworks into the .app.
#   QT_DIR=~/Qt/6.8.3/macos ./build.sh [Release|Debug]
set -euo pipefail

CONFIG="${1:-Release}"
QT_DIR="${QT_DIR:-$HOME/Qt/6.8.3/macos}"
ROOT="$(cd "$(dirname "$0")" && pwd)"
BUILD="$ROOT/build/$CONFIG"

cmake -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE="$CONFIG" -DCMAKE_PREFIX_PATH="$QT_DIR"
cmake --build "$BUILD"

if [[ "$(uname)" == "Darwin" ]]; then
    "$QT_DIR/bin/macdeployqt" "$BUILD/DisplayHawk.app"
    echo "Built $BUILD/DisplayHawk.app"
else
    echo "Built $BUILD/DisplayHawk"
fi
