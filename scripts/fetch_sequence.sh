#!/usr/bin/env bash
# fetch_sequence.sh <name> <outdir>  - downloads one raw YUV from Google Drive (ids in sequences.json).
# Path 1: gdown (works when the file is shared "anyone with the link").
# Path 2: rclone with a Drive token in secret RCLONE_GDRIVE_TOKEN (JSON from `rclone authorize drive`) for restricted shares.
set -euo pipefail
name=$1; out=$2; mkdir -p "$out"
id=$(python3 -c "import json,sys; print(json.load(open('sequences.json'))['$name']['id'])")
want=$(python3 -c "import json,sys; print(json.load(open('sequences.json'))['$name']['bytes'])")
dst="$out/$name.yuv"
if [ -s "$dst" ] && [ "$(stat -c %s "$dst")" = "$want" ]; then echo "have $dst"; exit 0; fi
pip install -q --disable-pip-version-check gdown >/dev/null 2>&1 || true
if gdown --fuzzy --no-cookies -O "$dst" "https://drive.google.com/uc?id=$id" 2>&1 | tail -3 && [ -s "$dst" ] && [ "$(stat -c %s "$dst")" = "$want" ]; then
  echo "downloaded via gdown: $dst"; exit 0
fi
rm -f "$dst"
if [ -n "${RCLONE_GDRIVE_TOKEN:-}" ]; then
  curl -fsSL https://rclone.org/install.sh | sudo bash >/dev/null 2>&1 || true
  mkdir -p ~/.config/rclone
  printf '[gdrive]\ntype = drive\nscope = drive.readonly\ntoken = %s\n' "$RCLONE_GDRIVE_TOKEN" > ~/.config/rclone/rclone.conf
  rclone backend copyid gdrive: "$id" "$dst" -P
  [ "$(stat -c %s "$dst")" = "$want" ] && { echo "downloaded via rclone: $dst"; exit 0; }
fi
echo "FAILED to fetch $name (id $id). Share the file 'anyone with the link' or add the RCLONE_GDRIVE_TOKEN secret." >&2
exit 1
