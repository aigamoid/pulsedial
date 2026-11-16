#!/usr/bin/env bash
# Copy the built firmware into docs/firmware/ for the web installer (GitHub Pages) and write docs/manifest.json.
# Run after `idf.py build`:   tools/make_release.sh 1.0.0
# The parts are written separately (not one merged image) so that the NVS partition (Wi-Fi settings and the
# Spotify key) between the partition table and otadata is left untouched when someone updates.
set -euo pipefail
cd "$(dirname "$0")/.."
VER="${1:?usage: tools/make_release.sh <version>}"
OUT=docs/firmware
mkdir -p "$OUT"
cp build/bootloader/bootloader.bin "$OUT/bootloader.bin"
cp build/partition_table/partition-table.bin "$OUT/partition-table.bin"
cp build/ota_data_initial.bin "$OUT/ota_data_initial.bin"
cp build/StopWatch-UserDemo.bin "$OUT/pulsedial.bin"
# offsets from the build (so they follow partitions.csv)
off() { printf '%d' "$(awk -v f="$1" '$2 == f {print $1}' build/flash_args)"; }
BL=$(off bootloader/bootloader.bin); PT=$(off partition_table/partition-table.bin)
OD=$(off ota_data_initial.bin); APP=$(off StopWatch-UserDemo.bin)
cat > docs/manifest.json <<JSON
{
  "name": "PulseDial",
  "version": "$VER",
  "new_install_prompt_erase": true,
  "builds": [
    {
      "chipFamily": "ESP32-S3",
      "parts": [
        { "path": "firmware/bootloader.bin", "offset": $BL },
        { "path": "firmware/partition-table.bin", "offset": $PT },
        { "path": "firmware/ota_data_initial.bin", "offset": $OD },
        { "path": "firmware/pulsedial.bin", "offset": $APP }
      ]
    }
  ]
}
JSON
echo "docs/firmware and docs/manifest.json updated for $VER"
