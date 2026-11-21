#!/bin/sh
# Starts the Manchester Ride server from this folder. Its console is this terminal:
# type help, list, op <name>, say <message>, update, stop ...
cd "$(dirname "$0")" || exit 1
case "$(uname -m)" in
  aarch64|arm64) BIN=./manchesterride-server-arm64 ;;
  *)             BIN=./manchesterride-server ;;
esac
chmod +x "$BIN" 2>/dev/null
exec "$BIN" "$@"
