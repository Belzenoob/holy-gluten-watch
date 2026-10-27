#!/usr/bin/env bash
# Downloads Waveshare's Arduino board libraries for the ESP32-C6-Touch-AMOLED-2.06
# (display CO5300, touch FT3168, sensors, power manager; Apache-2.0) into
# firmware/vendor/waveshare, pinned to the commit the firmware was built with.
set -euo pipefail
COMMIT=2a3608db86ae1546662f696fb6997e58c8ffa252
DEST="$(cd "$(dirname "$0")" && pwd)/vendor/waveshare"
if [ -d "$DEST/examples/arduino/libraries" ]; then
  echo "Già presente: $DEST"
  exit 0
fi
mkdir -p "$DEST"
git -C "$DEST" init -q
git -C "$DEST" remote add origin https://github.com/waveshareteam/ESP32-C6-Touch-AMOLED-2.06
git -C "$DEST" sparse-checkout set examples/arduino
git -C "$DEST" fetch -q --depth 1 --filter=blob:none origin "$COMMIT"
git -C "$DEST" checkout -q FETCH_HEAD
echo "Librerie della scheda in $DEST"
