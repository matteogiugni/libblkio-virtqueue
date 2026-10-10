#!/bin/bash
set -euo pipefail

SOCKET="${RUSTVQ_SOCKET:-/tmp/rfl-vhost-user-blk.sock}"

if [[ ! -S "$SOCKET" ]]; then
    echo "ERROR: vhost-user socket not found: $SOCKET" >&2
    exit 1
fi

echo "Running RustVQ correctness test"
echo "Socket: $SOCKET"

./examples/rustvq "$SOCKET"

echo "RustVQ correctness test PASSED"