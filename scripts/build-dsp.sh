#!/usr/bin/env bash
# Build PitchGrid DSP plugin for Ableton Move (ARM64) and optionally deploy.
#
# Usage: ./scripts/build-dsp.sh [--deploy] [host]
#   --deploy: Deploy to Move after building
#   host:     Move hostname (default: move.local)
#
# Cross-compiles scalatrix C++ library and links with PitchGrid C sources.
# Output: build/dsp.so
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO_ROOT="$(dirname "$SCRIPT_DIR")"
MOVE_ANYTHING_DIR="$REPO_ROOT/move-anything"
SCALATRIX_DIR="$REPO_ROOT/scalatrix"
IMAGE_NAME="move-anything-builder"

DEPLOY=0
HOST="move.local"
for arg in "$@"; do
    if [ "$arg" = "--deploy" ]; then
        DEPLOY=1
    elif [ "$arg" != "--deploy" ]; then
        HOST="$arg"
    fi
done

USER="ableton"
REMOTE_DIR="/data/UserData/move-anything/modules/midi_fx/pitchgrid"
SSH_OPTS="-o ConnectTimeout=5 -o BatchMode=yes"

# Ensure submodules are initialized
if [ ! -d "$MOVE_ANYTHING_DIR/src" ] || [ ! -d "$SCALATRIX_DIR/src" ]; then
    echo "Initializing submodules..."
    git -C "$REPO_ROOT" submodule update --init --recursive
fi

if [ ! -d "$MOVE_ANYTHING_DIR/src" ]; then
    echo "Error: move-anything submodule not found. Run: git submodule update --init"
    exit 1
fi

if [ ! -d "$SCALATRIX_DIR/src" ]; then
    echo "Error: scalatrix submodule not found. Run: git submodule update --init"
    exit 1
fi

mkdir -p "$REPO_ROOT/build"

# Check for Docker image
if ! docker image inspect "$IMAGE_NAME" &>/dev/null; then
    echo "Docker image '$IMAGE_NAME' not found."
    echo "Build it first: cd $MOVE_ANYTHING_DIR && docker build -t $IMAGE_NAME ."
    exit 1
fi

echo "=== Building PitchGrid DSP (ARM64) ==="

docker run --rm \
    -v "$REPO_ROOT:/pitchgrid" \
    -u "$(id -u):$(id -g)" \
    "$IMAGE_NAME" \
    /bin/bash -c "
        set -e
        CXX=aarch64-linux-gnu-g++
        CC=aarch64-linux-gnu-gcc
        AR=aarch64-linux-gnu-ar
        CXXFLAGS='-O2 -fPIC -std=c++17'
        CFLAGS='-O2 -fPIC -g'

        SCALATRIX_SRC=/pitchgrid/scalatrix/src
        SCALATRIX_INC=/pitchgrid/scalatrix/include
        BUILDDIR=/pitchgrid/build
        DSP_DIR=/pitchgrid/dsp

        # --- Build scalatrix static lib ---
        echo 'Compiling scalatrix sources...'
        mkdir -p \$BUILDDIR/scalatrix_obj

        SCALATRIX_SRCS='
            affine_transform.cpp
            scale.cpp
            lattice.cpp
            params.cpp
            mos.cpp
            pitchset.cpp
            linear_solver.cpp
            label_calculator.cpp
            node.cpp
            main.cpp
            spectrum.cpp
            consonance.cpp
            c_api.cpp
        '

        for src in \$SCALATRIX_SRCS; do
            echo \"  \$src\"
            \$CXX \$CXXFLAGS -I\$SCALATRIX_INC -c \$SCALATRIX_SRC/\$src -o \$BUILDDIR/scalatrix_obj/\${src%.cpp}.o
        done

        echo 'Creating libscalatrix.a...'
        \$AR rcs \$BUILDDIR/libscalatrix_arm64.a \$BUILDDIR/scalatrix_obj/*.o

        # --- Compile pad_hooks sources ---
        echo 'Compiling pad_hooks sources...'
        mkdir -p \$BUILDDIR/pg_obj
        PAD_HOOKS_DIR=/pitchgrid/pad_hooks

        PAD_HOOKS_SRCS='hook_engine.c pad_hooks.c'
        for src in \$PAD_HOOKS_SRCS; do
            echo \"  pad_hooks/\$src\"
            \$CC \$CFLAGS -c \
                \$PAD_HOOKS_DIR/\$src \
                -o \$BUILDDIR/pg_obj/\${src%.c}.o \
                -I\$PAD_HOOKS_DIR
        done

        # --- Compile PitchGrid C sources ---
        echo 'Compiling PitchGrid sources...'

        PG_SRCS='pitchgrid_scale.c pitchgrid_layout.c pitchgrid_mpe.c pitchgrid_dsp.c'

        for src in \$PG_SRCS; do
            echo \"  dsp/\$src\"
            \$CC \$CFLAGS -c \
                \$DSP_DIR/\$src \
                -o \$BUILDDIR/pg_obj/\${src%.c}.o \
                -I/pitchgrid/move-anything/src \
                -I\$SCALATRIX_INC \
                -I\$DSP_DIR \
                -I\$PAD_HOOKS_DIR
        done

        # --- Link dsp.so ---
        echo 'Linking dsp.so...'
        \$CXX -shared -o \$BUILDDIR/dsp.so \
            \$BUILDDIR/pg_obj/*.o \
            \$BUILDDIR/libscalatrix_arm64.a \
            -lm

        echo 'Done.'
        file \$BUILDDIR/dsp.so
        ls -lh \$BUILDDIR/dsp.so
    "

echo ""
echo "=== Built: build/dsp.so ==="
file "$REPO_ROOT/build/dsp.so"
ls -lh "$REPO_ROOT/build/dsp.so"

# --- Deploy ---
if [ "$DEPLOY" -eq 1 ]; then
    echo ""
    echo "=== Deploying to $HOST ==="

    ssh $SSH_OPTS "$USER@$HOST" "mkdir -p $REMOTE_DIR"
    scp $SSH_OPTS "$REPO_ROOT/src/module.json" "$REPO_ROOT/build/dsp.so" "$USER@$HOST:$REMOTE_DIR/"

    # Clear previous log
    ssh $SSH_OPTS "$USER@$HOST" "rm -f /data/UserData/move-anything/pitchgrid.log"

    echo ""
    echo "=== Deployed ==="
    echo "Module: $REMOTE_DIR"
    echo ""
    echo "Next steps:"
    echo "  1. On Move: Shift+Menu to reload modules"
    echo "  2. Add 'PitchGrid' as MIDI FX in a chain slot"
    echo "  3. Check log:"
    echo "     ssh $USER@$HOST cat /data/UserData/move-anything/pitchgrid.log"
else
    echo ""
    echo "To deploy: ./scripts/build-dsp.sh --deploy [$HOST]"
fi
