#!/bin/bash

# Ensure we are in the project root
cd "$(dirname "$0")/.."

VENV_DIR=".venv-compiledb"

# Create venv if it doesn't exist
if [ ! -d "$VENV_DIR" ]; then
    echo "Creating virtual environment for compiledb..."
    python3 -m venv "$VENV_DIR"
    "$VENV_DIR/bin/pip" install compiledb
fi

# Activate venv
source "$VENV_DIR/bin/activate"

echo "Generating compile_commands.json for Gateway.pro and Node.pro..."

# Clean previous build artifacts to ensure a fresh build log
# make clean > /dev/null 2>&1

# Run qmake to generate Makefiles
# We need to process both Gateway.pro and Node.pro. 
# Since they are subdirs projects, we can try running compiledb on the top level make.

# Check if Makefile exists, if not run qmake
if [ ! -f "Makefile" ]; then
    echo "Running qmake..."
    qmake -r
fi

# Generate compile_commands.json
# Using -n (dry run) with make is faster and sufficient for compiledb
compiledb -n make -j$(nproc)

echo "Done. compile_commands.json generated."
