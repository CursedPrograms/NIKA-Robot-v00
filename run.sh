#!/usr/bin/env bash
# Sets up (or reuses) a venv in ./venv, makes sure pygame/requests are
# installed, then launches the NIKA WiFi controller.
set -e

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$DIR"

if [ ! -d "venv" ]; then
  echo "Creating venv..."
  python3 -m venv venv
fi

source venv/bin/activate
pip install -q -r requirements.txt

python scripts/nika_controller.py "$@"
