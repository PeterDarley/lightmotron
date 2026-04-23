#!/usr/bin/env bash
set -euo pipefail

# Simple upload script that uses the project's venv mpremote if present.
# Usage: ./upload.sh [PORT]

MPREMOTE="$(pwd)/venv/bin/mpremote"
if [ ! -x "$MPREMOTE" ]; then
  # fallback to global mpremote
  MPREMOTE="mpremote"
fi

PORT=${1:-COM3}

"$MPREMOTE" connect "$PORT" fs cp -r boot.py main.py settings.py lib/ www/ :

# Remove files that exist locally but must not be on the device
"$MPREMOTE" connect "$PORT" fs rm :lib/licence 2>/dev/null || true
"$MPREMOTE" connect "$PORT" fs rm :lib/README.md 2>/dev/null || true
