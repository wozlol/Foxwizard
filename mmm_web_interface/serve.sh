#!/bin/sh
# Foxwizard Patchling has to be served over http(s), not opened as a file:// page — Chrome blocks
# ES module <script type="module"> imports under file://, and service workers require a real
# origin too. This just serves this folder on localhost so both work.
cd "$(dirname "$0")"
PORT="${1:-8080}"
echo "Serving Foxwizard Patchling at http://localhost:$PORT — Ctrl+C to stop"
python3 -m http.server "$PORT"
