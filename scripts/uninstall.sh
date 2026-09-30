#!/usr/bin/env bash
set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
BIN_DIR="${PREFIX}/bin"
UNIT_DIR="${HOME}/.config/systemd/user"

echo "==> Uninstalling pw-mpris-visualcard..."
if command -v systemctl >/dev/null 2>&1; then
  systemctl --user disable --now pw-mpris-visualcard 2>/dev/null || true
fi

rm -f "${BIN_DIR}/pw-mpris-visualcard"
rm -f "${UNIT_DIR}/pw-mpris-visualcard.service"

if command -v systemctl >/dev/null 2>&1; then
  systemctl --user daemon-reload || true
fi

echo "==> Uninstallation complete!"
