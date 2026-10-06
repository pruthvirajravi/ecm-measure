#!/usr/bin/env bash
# fetch_sequence.sh <name> <outdir>  — downloads one raw YUV from Google Drive (ids in sequences.json).
# Path 1: gdown (works when the file is shared "anyone with the link"); the runner image ships an old apt gdown
#         without --fuzzy, so a current gdown is installed into the user site and invoked as `python3 -m gdown`.
# Path 2: plain curl with the Drive confirm token (large files).
# Path 3: rclone with a Drive token in secret RCLONE_GDRIVE_TOKEN (JSON from `rclone authorize drive`) for restricted shares.
set -euo pipefail
name=$1; out=$2; mkdir -p "$out"
id=$(python3 -c "import json,sys; print(json.load(open('sequences.json'))['$name']['id'])")
want=$(python3 -c "import json,sys; print(json.load(open('sequences.json'))['$name']['bytes'])")
dst="$out/$name.yuv"
ok() { [ -s "$dst" ] && [ "$(stat -c %s "$dst")" = "$want" ]; }
if ok; then echo "have $dst"; exit 0; fi

python3 -m pip install -q --user --upgrade --disable-pip-version-check gdown >/dev/null 2>&1 || true
export PATH="$HOME/.local/bin:$PATH"
echo "gdown version: $(python3 -m gdown --version 2>/dev/null || echo none)"
if python3 -m gdown --fuzzy -O "$dst" "https://drive.google.com/uc?id=$id" 2>&1 | tail -3 && ok; then
  echo "downloaded via gdown: $dst"; exit 0
fi
rm -f "$dst"

# plain curl with confirm token (works for public files, including > 100 MB)
tmpc=$(mktemp)
curl -sL -c "$tmpc" "https://drive.google.com/uc?export=download&id=$id" -o "$dst" || true
if ! ok; then
  token=$(grep -o 'confirm=[0-9A-Za-z_-]*' "$dst" 2>/dev/null | head -1 | cut -d= -f2 || true)
  uuid=$(grep -o 'name="uuid" value="[^"]*"' "$dst" 2>/dev/null | head -1 | sed 's/.*value="//;s/"//' || true)
  if [ -n "$token" ]; then
    curl -sL -b "$tmpc" "https://drive.usercontent.google.com/download?id=$id&export=download&confirm=$token${uuid:+&uuid=$uuid}" -o "$dst" || true
  fi
fi
rm -f "$tmpc"
if ok; then echo "downloaded via curl: $dst"; exit 0; fi
rm -f "$dst"

if [ -n "${RCLONE_GDRIVE_TOKEN:-}" ]; then
  curl -fsSL https://rclone.org/install.sh | sudo bash >/dev/null 2>&1 || true
  mkdir -p ~/.config/rclone
  printf '[gdrive]\ntype = drive\nscope = drive.readonly\ntoken = %s\n' "$RCLONE_GDRIVE_TOKEN" > ~/.config/rclone/rclone.conf
  rclone backend copyid gdrive: "$id" "$dst" -P
  if ok; then echo "downloaded via rclone: $dst"; exit 0; fi
fi
echo "FAILED to fetch $name (id $id). Share the file 'anyone with the link' or add the RCLONE_GDRIVE_TOKEN secret." >&2
exit 1
