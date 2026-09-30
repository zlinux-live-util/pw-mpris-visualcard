#!/usr/bin/env bash
set -euo pipefail

PREFIX="${PREFIX:-/usr/local}"
BIN_DIR="${PREFIX}/bin"
UNIT_DIR="${HOME}/.config/systemd/user"

echo "==> Installing pw-mpris-visualcard to ${BIN_DIR}..."
install -d "${BIN_DIR}"
BIN_SRC="pw-mpris-visualcard"
if [[ ! -f "$BIN_SRC" && -f "pw-mpris-visualcard-native" ]]; then
  BIN_SRC="pw-mpris-visualcard-native"
fi

install -m 755 "${BIN_SRC}" "${BIN_DIR}/pw-mpris-visualcard"

if [[ -f pw-mpris-visualcard.service ]]; then
  echo "==> Installing systemd user unit to ${UNIT_DIR}..."
  install -d "${UNIT_DIR}"
  sed -e "s|@REPO@/pw-mpris-visualcard-native|${BIN_DIR}/pw-mpris-visualcard|g" \
      -e "s|@REPO@|${BIN_DIR}|g" \
      -e "s|@ARGS@|--node pw-mpris-visualcard --size 460x690 --fps 30 --lyrics 3|g" \
      pw-mpris-visualcard.service > "${UNIT_DIR}/pw-mpris-visualcard.service"

  if command -v systemctl >/dev/null 2>&1; then
    systemctl --user daemon-reload || true
  fi
fi

echo "==> Installation complete!"
echo "Run directly: pw-mpris-visualcard"
echo "Or start as systemd service: systemctl --user enable --now pw-mpris-visualcard"
